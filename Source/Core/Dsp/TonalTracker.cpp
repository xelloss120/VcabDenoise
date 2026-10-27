#include "TonalTracker.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

namespace
{
constexpr int kFloorGap = 3;              // 山のすぐ隣は周りの平均に含めない（山の裾で平均が上がらないように）
constexpr double kForgetSeconds = 0.3;    // 削る前の山は、これだけ見えなければ忘れる
constexpr double kAttackSeconds = 0.2;    // 削り始めるときに、これだけかけて深くする
constexpr double kVoiceLowHz = 80;        // これより低い谷は太くする（声がないため）
constexpr double kMainsMaxHz = 2000;      // 電源の倍数とみなす上限（これより上は、倍数の間が狭くなりすぎる）

// 電源の 50 / 60Hz のちょうど倍数か（ずれは 1.5Hz か 0.4% の広いほう。電源の周波数の揺れと、山の周波数の誤差の分）
bool isMainsHarmonic (double freq)
{
    if (freq > kMainsMaxHz)
        return false;
    const double tolerance = std::max (1.5, 0.004 * freq);
    for (const double base : { 50.0, 60.0 })
    {
        const double nearest = std::round (freq / base);
        if (nearest >= 1 && std::abs (freq - nearest * base) < tolerance)
            return true;
    }
    return false;
}
} // namespace

void TonalTracker::prepare (double sampleRate, int fftSize, double hop)
{
    sr = sampleRate;
    size = fftSize;
    hopSeconds = hop;
    binHz = sr / size;
    magDb.assign (static_cast<size_t> (size / 2 + 1), 0.0);
    prefix.assign (static_cast<size_t> (size / 2 + 2), 0.0);
    candidates.reserve (256);
    reset();
}

void TonalTracker::reset()
{
    for (auto& t : tracks)
        t = {};
}

double TonalTracker::widthFor (double freq, const TonalSettings& s) const
{
    if (freq < kVoiceLowHz)
        return s.lowWidth;
    // 2kHz での幅に比例。FIR（4096）で作れる細かさ（約 2 区画）より細くはしない
    return std::max (s.width2k * freq / 2000.0, 2.0 * binHz);
}

void TonalTracker::analyze (const float* power, double toAmplitude, bool voiced, const TonalSettings& s)
{
    const int half = size / 2;
    const double ampDb = 20.0 * std::log10 (toAmplitude);
    for (int k = 0; k <= half; ++k)
        magDb[static_cast<size_t> (k)] = 10.0 * std::log10 (static_cast<double> (power[k]) + 1.0e-24) + ampDb;
    prefix[0] = 0;
    for (int k = 0; k <= half; ++k)
        prefix[static_cast<size_t> (k + 1)] = prefix[static_cast<size_t> (k)] + magDb[static_cast<size_t> (k)];

    const int floorHalf = std::max (kFloorGap + 2, static_cast<int> (std::lround (s.neighborHz / binHz)));
    auto floorAt = [&] (int k)
    {
        const int a0 = std::max (0, k - floorHalf), a1 = std::max (0, k - kFloorGap);
        const int b0 = std::min (half + 1, k + kFloorGap + 1), b1 = std::min (half + 1, k + floorHalf + 1);
        const double sum = (prefix[static_cast<size_t> (a1)] - prefix[static_cast<size_t> (a0)])
                         + (prefix[static_cast<size_t> (b1)] - prefix[static_cast<size_t> (b0)]);
        const int count = (a1 - a0) + (b1 - b0);
        return count > 0 ? sum / count : magDb[static_cast<size_t> (k)];
    };

    // ---- 1. 突き出た山を探す ----
    candidates.clear();
    const int kLow = std::max (3, static_cast<int> (s.lowHz / binHz));
    const int kHigh = std::min (half - 3, static_cast<int> (std::min (s.highHz, sr * 0.45) / binHz));
    for (int k = kLow; k <= kHigh; ++k)
    {
        const double v = magDb[static_cast<size_t> (k)];
        if (v < s.quietDb)
            continue; // 聞こえないほど小さい山は追いかけない
        bool isMax = true;
        for (int d = 1; d <= 2 && isMax; ++d)
            isMax = v >= magDb[static_cast<size_t> (k - d)] && v > magDb[static_cast<size_t> (k + d)];
        if (! isMax)
            continue;
        const double prominence = v - floorAt (k);
        if (prominence < s.sensitivityDb)
            continue;
        // 周波数を、区画の間まで細かく求める（3 点を通る放物線）
        const double a = magDb[static_cast<size_t> (k - 1)], c = magDb[static_cast<size_t> (k + 1)];
        const double den = a - 2 * v + c;
        const double off = std::abs (den) < 1.0e-12 ? 0.0 : std::clamp (0.5 * (a - c) / den, -0.5, 0.5);
        if (candidates.size() < 255)
            candidates.push_back ({ (k + off) * binHz, prominence, false, isMainsHarmonic ((k + off) * binHz) });
    }

    // ---- 2. 声の倍音（等間隔に並んだ山）を見分ける（VoiChanAlgoBox と同じ） ----
    // 電源の倍数でない山が 2 本以上ある列だけを声とみなす。ハムの列（100, 200, 300, 400Hz …）が「基音 100Hz の声」に
    // 見えないように。声の倍音がたまたま電源の倍数に重なっても（基音 130Hz の 650Hz など）、その山は倍音として守る
    double bestF0 = 0;
    int bestCount = 0;
    for (const auto& c : candidates)
    {
        if (c.freq > 5000)
            break;
        for (int div = 1; div <= 6; ++div)
        {
            const double f0 = c.freq / div;
            if (f0 < 70 || f0 > 500)
                continue;
            int count = 0, notMains = 0;
            for (const auto& o : candidates)
            {
                if (o.freq > 5000)
                    break;
                const double ratio = o.freq / f0;
                const double nearest = std::round (ratio);
                if (nearest >= 1 && std::abs (ratio - nearest) < 0.08) // 基音の 8% 以内のずれ
                {
                    ++count;
                    if (! o.mains)
                        ++notMains;
                }
            }
            if (notMains >= 2 && count > bestCount)
            {
                bestCount = count;
                bestF0 = f0;
            }
        }
    }
    const bool harmonicVoice = bestCount >= 4;
    if (harmonicVoice)
        for (auto& c : candidates)
        {
            const double ratio = c.freq / bestF0;
            const double nearest = std::round (ratio);
            if (c.freq <= 5000 && nearest >= 1 && std::abs (ratio - nearest) < 0.08)
                c.harmonic = true;
        }

    updateTracks (voiced || harmonicVoice, s);
}

void TonalTracker::updateTracks (bool voiced, const TonalSettings& s)
{
    std::array<bool, kMaxTracks> matched {};

    for (const auto& c : candidates)
    {
        // 今追いかけている山に、近い候補（±1.5 区画か ±1.5% の広いほう）を当てはめる
        int best = -1;
        double bestDist = 1.0e9;
        for (int i = 0; i < static_cast<int> (tracks.size()); ++i)
        {
            const auto& t = tracks[static_cast<size_t> (i)];
            if (! t.used)
                continue;
            const double dist = std::abs (c.freq - t.freq);
            if (dist < std::max (1.5 * binHz, 0.015 * t.freq) && dist < bestDist)
            {
                bestDist = dist;
                best = i;
            }
        }
        if (best >= 0)
        {
            auto& t = tracks[static_cast<size_t> (best)];
            // 声の倍音と重なった候補でも、もう削っている山なら追いかけ続ける
            if (c.harmonic && ! t.active)
                continue;
            t.freq += 0.3 * (c.freq - t.freq); // 山の動きに合わせて、ゆっくり動かす
            if (! voiced)
                t.prominence += 0.3 * (c.prominence - t.prominence); // 声がある間は深さを固定する
            t.presentSec += hopSeconds;
            t.missingSec = 0;
            matched[static_cast<size_t> (best)] = true;
            continue;
        }
        if (voiced || c.harmonic)
            continue; // 新しい山は、声が出ていないフレームでだけ拾う

        for (auto& t : tracks)
            if (! t.used)
            {
                t = {};
                t.used = true;
                t.freq = c.freq;
                t.prominence = c.prominence;
                t.presentSec = hopSeconds;
                break;
            }
    }

    // 見えなかった山: 声が出ているフレームでは数えない（声に隠れて見えないだけ）
    for (int i = 0; i < static_cast<int> (tracks.size()); ++i)
    {
        auto& t = tracks[static_cast<size_t> (i)];
        if (! t.used || matched[static_cast<size_t> (i)])
            continue;
        if (! voiced)
            t.missingSec += hopSeconds;
        if (! t.active && t.fade <= 0 && t.missingSec > kForgetSeconds)
            t = {};
    }

    // 飛んだ先か: まだ削っていない、今見えている山の近くで、削っていた山が消えて間もないなら、同じ音が飛んだとみなしてすぐ削る。
    // 新しい山を見つけたときに 1 回だけ調べると、分析の窓（約 85ms）に飛ぶ前と後の両方が入るため、古い山がまだ見えていて
    // 判定できない（次のフレームからは、新しい山は追いかけている山になり、二度と調べない）。そのため、毎フレーム調べる
    if (! voiced)
        for (auto& n : tracks)
        {
            if (! n.used || n.missingSec > 0 || n.presentSec >= s.activateSec)
                continue;
            for (auto& t : tracks)
                if (&t != &n && t.used && t.fade >= 0.5 && ! t.jumpedFrom && t.missingSec > 0 && t.missingSec <= s.jumpSec
                    && std::abs (n.freq - t.freq) <= s.jumpRange * t.freq)
                {
                    n.presentSec = s.activateSec;
                    t.jumpedFrom = true;
                    break;
                }
        }

    // 削る山を選ぶ: 持続判定時間以上居続け、解除時間以上見えなくなっていないもののうち、突き出た量の大きい順に「本数」まで
    std::array<int, kMaxTracks> order {};
    int numEligible = 0;
    for (int i = 0; i < static_cast<int> (tracks.size()); ++i)
    {
        const auto& t = tracks[static_cast<size_t> (i)];
        if (t.used && t.presentSec >= s.activateSec && t.missingSec < s.releaseSec)
            order[static_cast<size_t> (numEligible++)] = i;
    }
    std::sort (order.begin(), order.begin() + numEligible, [&] (int x, int y)
               { return tracks[static_cast<size_t> (x)].prominence > tracks[static_cast<size_t> (y)].prominence; });
    for (auto& t : tracks)
        t.active = false;
    for (int j = 0; j < std::min (numEligible, std::clamp (s.count, 1, kMaxNotches)); ++j)
        tracks[static_cast<size_t> (order[static_cast<size_t> (j)])].active = true;

    // 効き: 削る山は 0.2 秒で深く、外す山（消えた・選ばれなくなった）は「解除時間」で浅くする
    for (auto& t : tracks)
    {
        if (! t.used)
            continue;
        if (t.active && t.missingSec == 0)
            t.fade = std::min (1.0, t.fade + hopSeconds / kAttackSeconds);
        else
            t.fade = std::max (0.0, t.fade - hopSeconds / std::max (0.05, s.releaseSec));
        if (! t.active && t.fade <= 0 && t.missingSec >= s.releaseSec)
            t = {};
    }
}

int TonalTracker::notches (Notch* out, const TonalSettings& s) const
{
    int count = 0;
    for (const auto& t : tracks)
    {
        if (! t.used || t.fade <= 0.0 || count >= kMaxNotches)
            continue;
        const double depth = std::min (t.prominence, s.maxDepthDb) * t.fade;
        if (depth <= 0.01)
            continue;
        out[count++] = { static_cast<float> (t.freq), static_cast<float> (depth), static_cast<float> (widthFor (t.freq, s)) };
    }
    return count;
}

void TonalTracker::addShape (const Notch* n, int count, const double* freqs, int points, double* db)
{
    // 谷は dB でガウス形（半値幅 = 幅）
    for (int j = 0; j < count; ++j)
    {
        const double sigma = n[j].widthHz / 2.3548;
        const double lo = n[j].freq - 4.0 * sigma, hi = n[j].freq + 4.0 * sigma;
        for (int i = 0; i < points; ++i)
        {
            const double f = freqs[i];
            if (f < lo || f > hi)
                continue;
            const double d = (f - n[j].freq) / sigma;
            db[i] -= n[j].depthDb * std::exp (-0.5 * d * d);
        }
    }
}

} // namespace vcab
