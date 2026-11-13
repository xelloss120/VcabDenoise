#include "GainRule.h"

#include "MathConstants.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vcab
{

namespace
{
// 級数の係数 (-1)^(k+1) / (k・k!)（k = 1〜16）。x < 1 では、17 項目からの和は 1e-16 より小さい
constexpr int kE1Terms = 16;
constexpr std::array<double, kE1Terms> e1Coefficients()
{
    std::array<double, kE1Terms> c {};
    double factorial = 1;
    for (int k = 1; k <= kE1Terms; ++k)
    {
        factorial *= k;
        c[static_cast<size_t> (k - 1)] = (k % 2 == 1 ? 1.0 : -1.0) / (k * factorial);
    }
    return c;
}
constexpr auto kE1Coef = e1Coefficients();

// 指数積分 E1(x)（log-MMSE に使う）。式は Abramowitz と Stegun の「Handbook of Mathematical Functions」から
double expIntE1 (double x)
{
    x = std::max (x, 1.0e-10);
    if (x < 1.0)
    {
        // 級数（5.1.11）: E1(x) = -γ - ln x - Σ (-x)^k / (k・k!)。係数を前もって計算し、ホーナー法で求める（割り算をなくす。
        // ノイズが勝っている区画はほとんどここを通るので、下げる量の計算で一番重いところだった）
        double sum = 0;
        for (int k = kE1Terms - 1; k >= 0; --k)
            sum = sum * x + kE1Coef[static_cast<size_t> (k)];
        return -0.5772156649015329 - std::log (x) + sum * x;
    }
    // 有理式の近似（5.1.56）: x e^x E1(x) ≒ P(x) / Q(x)。1 以上で誤差 2e-8 未満
    const double p = (((x + 8.5733287401) * x + 18.0590169730) * x + 8.6347608925) * x + 0.2677737343;
    const double q = (((x + 9.5733223454) * x + 25.6329561486) * x + 21.0996530827) * x + 3.9584969228;
    return p / (q * x) * std::exp (-x);
}

double erbHz (double f) { return 24.7 * (4.37 * f / 1000.0 + 1.0); } // Glasberg・Moore 1990

// ERB の数（ERB-number、Glasberg・Moore 1990）と、その逆
double erbNumber (double f) { return 21.4 * std::log10 (1.0 + 0.00437 * f); }
double erbNumberToHz (double e) { return (std::pow (10.0, e / 21.4) - 1.0) / 0.00437; }

constexpr double kBandSpacingErb = 2.0; // 帯域でまとめるときの、帯域の中心の間隔（RNNoise の 600Hz より上とほぼ同じ）
constexpr int kBandMinBins = 2;         // 帯域の中心の間隔の下限（区画の数。1 区画だけの帯域を作らない。PercepNet と同じ考え方）
// 帯域でまとめるときの、音声存在確率の「近く」「広く」の範囲（帯域の数。区画ごとのときの 3 区画・31 区画に当たる）
constexpr int kBandLocalHalf = 1, kBandGlobalHalf = 4;
// 帯域ごとの値の割合（残りは区画ごとの値）と、区画に戻した後のならしの幅（ERB）。2026-11-10 に聞き比べと数値で決め、11-12 に固定した
constexpr double kBandMix = 0.5, kBandSmoothErb = 2.0;

} // namespace

void GainRule::prepare (int fftSize, double sampleRate, int hopSize)
{
    size = fftSize;
    bins = size / 2 + 1;
    hopSeconds = hopSize / sampleRate;
    binHz = sampleRate / size;
    // 判定指向型の重み。もとの論文の 0.98 を、区切りの間隔 8ms のときの値とみなして、間隔に合わせる（VoiChanAlgoBox と同じ）
    ddAlpha = std::pow (0.98, hopSeconds / 0.008);
    // Cohen の声がない確率: ξ をならす重み（論文の 0.7 を、間隔 16ms のときの値とみなす）
    zetaAlpha = std::pow (0.7, hopSeconds / 0.016);
    const auto b = static_cast<size_t> (bins);
    for (auto* t : { &binTrack, &bandTrack })
    {
        t->prevClean.assign (b, 0.0f);
        t->xiNow.assign (b, 0.0f);
        t->vNow.assign (b, 0.0f);
        t->zeta.assign (b, 0.0f);
        t->prevLn.assign (b, 0.0f);
    }
    lg.assign (b, 0.0);
    mixWork.assign (b, 0.0);
    work.assign (b + 1, 0.0);
    makeErbRanges (1.0, erbLo, erbHi);
    logWork.assign (b + 1, 0.0);
    // 帯域でまとめるときの、帯域の中心（区画の位置）: 0Hz から、約 2 ERB ごと（2 区画以上空ける）。最後はナイキスト周波数
    std::vector<double> centers { 0.0 };
    for (;;)
    {
        const double c = centers.back();
        const double next = std::max (erbNumberToHz (erbNumber (c * binHz) + kBandSpacingErb) / binHz, c + kBandMinBins);
        if (next >= bins - 1 - 0.5 * kBandMinBins)
            break;
        centers.push_back (next);
    }
    centers.push_back (static_cast<double> (bins - 1));
    numBands = static_cast<int> (centers.size());
    bandLo.assign (b, 0);
    bandT.assign (b, 0.0f);
    for (int k = 0, j = 0; k < bins; ++k)
    {
        while (j < numBands - 2 && k >= centers[static_cast<size_t> (j + 1)])
            ++j;
        const double lo = centers[static_cast<size_t> (j)], hi = centers[static_cast<size_t> (j + 1)];
        bandLo[static_cast<size_t> (k)] = j;
        bandT[static_cast<size_t> (k)] = static_cast<float> (std::clamp ((k - lo) / (hi - lo), 0.0, 1.0));
    }
    bandPower.assign (static_cast<size_t> (numBands), 0.0f);
    bandNoise.assign (static_cast<size_t> (numBands), 0.0f);
    bandLg.assign (static_cast<size_t> (numBands), 0.0);
    makeErbRanges (kBandSmoothErb, bandSmoothLo, bandSmoothHi);
    // 立ち上がり保護の時間（フレームの数に直す）
    const auto coef = [this] (double seconds) { return static_cast<float> (1.0 - std::exp (-hopSeconds / seconds)); };
    slowCoef = coef (0.040);
    openAttack = coef (0.005);
    openRelease = coef (0.040);
    holdFrames = std::max (1, static_cast<int> (std::lround (0.030 / hopSeconds)));
    slowPower.assign (b, 0.0f);
    opened.assign (b, 0.0f);
    holdLeft.assign (b, 0);
    reset();
}

void GainRule::makeErbRanges (double widthErb, std::vector<int>& lowEdge, std::vector<int>& highEdge) const
{
    // 区画 k の中心の前後に、widthErb 個分の ERB の半分ずつ（端で切る）
    lowEdge.assign (static_cast<size_t> (bins), 0);
    highEdge.assign (static_cast<size_t> (bins), 0);
    for (int k = 0; k < bins; ++k)
    {
        const double half = 0.5 * widthErb * erbHz (k * binHz) / binHz;
        lowEdge[static_cast<size_t> (k)] = std::max (0, static_cast<int> (std::floor (k - half + 0.5)));
        highEdge[static_cast<size_t> (k)] = std::min (bins - 1, static_cast<int> (std::floor (k + half + 0.5)));
    }
}

void GainRule::Track::reset()
{
    std::fill (prevClean.begin(), prevClean.end(), 0.0f);
    std::fill (zeta.begin(), zeta.end(), 0.0f);
    std::fill (prevLn.begin(), prevLn.end(), 0.0f);
    first = true;
}

void GainRule::reset()
{
    binTrack.reset();
    bandTrack.reset();
    std::fill (slowPower.begin(), slowPower.end(), 0.0f);
    std::fill (opened.begin(), opened.end(), 0.0f);
    std::fill (holdLeft.begin(), holdLeft.end(), 0);
}

void GainRule::compute (const float* power, const float* noise, const GainSettings& s, float* gainDb)
{
    const double lnFloor = -s.maxReductionDb / kLnToDb;
    if (s.bands)
    {
        // 帯域ごとの計算と区画ごとの計算を行って、残す割合の対数を混ぜる
        computeBands (power, noise, s, lnFloor);
        std::copy (lg.begin(), lg.end(), mixWork.begin());
        computeBins (power, noise, s, lnFloor);
        for (int k = 0; k < bins; ++k)
        {
            const auto i = static_cast<size_t> (k);
            lg[i] = kBandMix * mixWork[i] + (1.0 - kBandMix) * lg[i];
        }
    }
    else
    {
        computeBins (power, noise, s, lnFloor);
    }
    if (s.onsetProtect)
        protectOnset (power, noise);
    for (int k = 0; k < bins; ++k)
        gainDb[k] = static_cast<float> (std::clamp (lg[static_cast<size_t> (k)], lnFloor, 0.0) * kLnToDb);
}

void GainRule::computeBins (const float* power, const float* noise, const GainSettings& s, double lnFloor)
{
    logMmse (binTrack, power, noise, s, bins);
    for (auto& v : lg)
        v = std::max (v, lnFloor);
    applyPresence (binTrack, lnFloor, bins, 1, 15);
    smoothTime (binTrack, s, bins);
    boxcar (erbLo, erbHi); // 前後 ERB の半分ずつの範囲で平均する
    binTrack.first = false;
}

void GainRule::logMmse (Track& t, const float* power, const float* noise, const GainSettings& s, int n)
{
    auto& prevClean = t.prevClean;
    auto& xiNow = t.xiNow;
    auto& vNow = t.vNow;
    const double strength = std::pow (10.0, s.strengthDb / 10.0);
    for (int k = 0; k < n; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const double gamma = std::max (power[k] / std::max (noise[k] * strength, 1.0e-30), 1.0e-6);
        // 判定指向型: 声の SNR を、直前のフレームの結果と、今のフレームの結果の重み付きの平均で見積もる
        const double xi = std::max (ddAlpha * prevClean[i] + (1.0 - ddAlpha) * std::max (gamma - 1.0, 0.0), 1.0e-6);
        const double v = xi / (1.0 + xi) * gamma;
        // 対数振幅の平均二乗誤差を最小にする（Ephraim・Malah 1985）: G = ξ / (1 + ξ) × exp(E1(v) / 2)
        const double ln = std::min (std::log (xi / (1.0 + xi)) + 0.5 * expIntE1 (v), 0.0);
        prevClean[i] = static_cast<float> (std::exp (2.0 * ln) * gamma);
        xiNow[i] = static_cast<float> (xi);
        vNow[i] = static_cast<float> (v);
        lg[i] = ln;
    }
}

void GainRule::applyPresence (Track& t, double lnFloor, int n, int localHalf, int globalHalf)
{
    auto& zeta = t.zeta;
    const auto& xiNow = t.xiNow;
    const auto& vNow = t.vNow;
    // 声がある確率 p = 1 / (1 + q / (1 − q) × (1 + ξ) e^(−v))。q は声がない確率（前もって）。
    // q は、ξ を時間でならし、近く（区画ごとなら 3 区画）、広く（同じく 31 区画）、フレーム全体の 3 つの平均が −10dB〜−5dB のどこにあるかで、
    // 音声存在確率を 0〜1 にし、その積から決める（Cohen・Berdugo 2001）。G^p × 下限^(1−p) は、対数では p ln G + (1 − p) ln 下限
    for (int k = 0; k < n; ++k)
        zeta[static_cast<size_t> (k)] = static_cast<float> (zetaAlpha * zeta[static_cast<size_t> (k)] + (1.0 - zetaAlpha) * xiNow[static_cast<size_t> (k)]);
    work[0] = 0;
    for (int k = 0; k < n; ++k)
        work[static_cast<size_t> (k + 1)] = work[static_cast<size_t> (k)] + zeta[static_cast<size_t> (k)];
    auto average = [&] (int k, int half)
    {
        const int lo = std::max (0, k - half), hi = std::min (n - 1, k + half);
        return (work[static_cast<size_t> (hi + 1)] - work[static_cast<size_t> (lo)]) / (hi - lo + 1);
    };
    static constexpr double zetaMin = 0.1, zetaMax = 0.316; // −10dB、−5dB
    const double lnRange = std::log (zetaMax / zetaMin);
    auto likelihood = [lnRange] (double z)
    {
        if (z <= zetaMin) return 0.0;
        if (z >= zetaMax) return 1.0;
        return std::log (z / zetaMin) / lnRange;
    };
    const double pFrame = likelihood (work[static_cast<size_t> (n)] / n);
    for (int k = 0; k < n; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const double q = pFrame <= 0.0 ? 0.95 : std::min (1.0 - likelihood (average (k, localHalf)) * likelihood (average (k, globalHalf)) * pFrame, 0.95);
        const double ratio = q / (1.0 - q);
        const double p = 1.0 / (1.0 + ratio * (1.0 + xiNow[i]) * std::exp (-std::min (static_cast<double> (vNow[i]), 700.0)));
        lg[i] = p * lg[i] + (1.0 - p) * lnFloor;
    }
}

void GainRule::computeBands (const float* power, const float* noise, const GainSettings& s, double lnFloor)
{
    // 区画の大きさとノイズ推定を、三角形を重ねた帯域でまとめる（区画 k は、両隣の帯域に 1 − t と t の重みで入る）
    std::fill (bandPower.begin(), bandPower.end(), 0.0f);
    std::fill (bandNoise.begin(), bandNoise.end(), 0.0f);
    for (int k = 0; k < bins; ++k)
    {
        const auto j = static_cast<size_t> (bandLo[static_cast<size_t> (k)]);
        const float t = bandT[static_cast<size_t> (k)];
        bandPower[j] += (1.0f - t) * power[k];
        bandPower[j + 1] += t * power[k];
        bandNoise[j] += (1.0f - t) * noise[k];
        bandNoise[j + 1] += t * noise[k];
    }
    // 帯域ごとに、log-MMSE → 音声存在確率 → 時間のならし（lg の先頭 numBands 個を使う）
    logMmse (bandTrack, bandPower.data(), bandNoise.data(), s, numBands);
    for (int j = 0; j < numBands; ++j)
        lg[static_cast<size_t> (j)] = std::max (lg[static_cast<size_t> (j)], lnFloor);
    applyPresence (bandTrack, lnFloor, numBands, kBandLocalHalf, kBandGlobalHalf);
    smoothTime (bandTrack, s, numBands);
    bandTrack.first = false;
    // 区画に戻す: 隣の帯域の中心の間を、残す割合の対数で直線につなぐ
    std::copy (lg.begin(), lg.begin() + numBands, bandLg.begin());
    for (int k = 0; k < bins; ++k)
    {
        const auto j = static_cast<size_t> (bandLo[static_cast<size_t> (k)]);
        const double t = bandT[static_cast<size_t> (k)];
        lg[static_cast<size_t> (k)] = (1.0 - t) * bandLg[j] + t * bandLg[j + 1];
    }
    // 帯域のならし（帯域の中心の折れ目が、一つの帯域だけが開いたときに三角形の頂点になり、粒になるのを和らげる）
    boxcar (bandSmoothLo, bandSmoothHi);
}

void GainRule::smoothTime (Track& t, const GainSettings& s, int n)
{
    auto& prevLn = t.prevLn;
    const bool trackFirst = t.first;
    // 残す割合（対数）の変化を、上がる（開ける）ときはアタック、下がるときはリリースでならす
    const auto attack = static_cast<float> (1.0 - std::exp (-hopSeconds / (std::max (0.1, s.attackMs) * 0.001)));
    const auto release = static_cast<float> (1.0 - std::exp (-hopSeconds / (std::max (1.0, s.releaseMs) * 0.001)));
    for (int k = 0; k < n; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const auto target = static_cast<float> (lg[i]);
        if (trackFirst)
            prevLn[i] = target;
        prevLn[i] += (target - prevLn[i]) * (target > prevLn[i] ? attack : release);
        lg[i] = prevLn[i];
    }
}

void GainRule::boxcar (const std::vector<int>& lowEdge, const std::vector<int>& highEdge)
{
    logWork[0] = 0;
    for (int k = 0; k < bins; ++k)
        logWork[static_cast<size_t> (k + 1)] = logWork[static_cast<size_t> (k)] + lg[static_cast<size_t> (k)];
    for (int k = 0; k < bins; ++k)
    {
        const int lo = lowEdge[static_cast<size_t> (k)], hi = highEdge[static_cast<size_t> (k)];
        if (hi > lo)
            lg[static_cast<size_t> (k)] = (logWork[static_cast<size_t> (hi + 1)] - logWork[static_cast<size_t> (lo)]) / (hi - lo + 1);
    }
}

void GainRule::protectOnset (const float* power, const float* noise)
{
    for (int k = 0; k < bins; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const float p = power[k];
        const float nz = noise[k] + 1.0e-20f;
        if (p > nz * 10.0f && p > slowPower[i] * 2.0f)
            holdLeft[i] = holdFrames;
        slowPower[i] += slowCoef * (p - slowPower[i]);
        if (holdLeft[i] > 0)
        {
            --holdLeft[i];
            opened[i] += openAttack * (1.0f - opened[i]); // 一気に開けると粒（ピロピロ）のもとになるので、なめらかに開ける
        }
        else
        {
            opened[i] -= openRelease * opened[i];
        }
        // 開ける量は、SNR が +9dB（8 倍）〜 +15dB（32 倍）の間でなだらかに。開けるときだけ、大きさに戻して計算する
        const float amount = opened[i] * std::clamp ((p / nz - 8.0f) / 24.0f, 0.0f, 1.0f);
        if (amount > 1.0e-4f)
        {
            const double g = std::exp (lg[i]);
            lg[i] = std::log (g + amount * (1.0 - g));
        }
    }
}

} // namespace vcab
