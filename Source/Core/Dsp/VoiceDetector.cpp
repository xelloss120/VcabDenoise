#include "VoiceDetector.h"

#include "MathConstants.h"

#include <rnnoise.h>

#include <chrono>
#include <cmath>

namespace vcab
{


namespace
{
double besselI0 (double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 30; ++k)
    {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}
} // namespace


void StreamResampler::prepare (double inRate, double outRate)
{
    step = inRate / outRate;
    // 下げるときは、出力のナイキストより上を落とす（折り返しを防ぐ）
    const double cutoff = std::min (1.0, outRate / inRate) * 0.92;
    half = static_cast<int> (std::ceil (16.0 / cutoff));
    const int taps = half * 2;
    constexpr double beta = 8.0;
    const double i0beta = besselI0 (beta);
    table.assign (static_cast<size_t> ((kPhases + 1) * taps), 0.0f);
    for (int p = 0; p <= kPhases; ++p)
    {
        const double frac = static_cast<double> (p) / kPhases;
        for (int k = 0; k < taps; ++k)
        {
            const double d = k - half + 1 - frac; // 入力の位置 − 出力の位置
            const double x = cutoff * d;
            const double sinc = std::abs (x) < 1.0e-9 ? 1.0 : std::sin (kPi * x) / (kPi * x);
            const double r = d / half;
            const double w = std::abs (r) < 1.0 ? besselI0 (beta * std::sqrt (1.0 - r * r)) / i0beta : 0.0;
            table[static_cast<size_t> (p * taps + k)] = static_cast<float> (cutoff * sinc * w);
        }
    }
    buf.assign (16384, 0.0f);
    reset();
}


void StreamResampler::reset()
{
    // 始めに half 個の 0（入力の -half〜-1 番目）を置く
    std::fill (buf.begin(), buf.end(), 0.0f);
    count = half;
    base = -half;
    t = 0;
}


void StreamResampler::process (const float* in, int n, std::vector<float>& out)
{
    const int taps = half * 2;
    for (int done = 0; done < n;)
    {
        const int space = static_cast<int> (buf.size()) - count;
        if (space <= 0)
            break; // 起こらない大きさにしてある
        const int len = std::min (space, n - done);
        std::copy (in + done, in + done + len, buf.begin() + count);
        count += len;
        done += len;

        for (;;)
        {
            const double ft = std::floor (t);
            const long long first = static_cast<long long> (ft) - half + 1; // 使う最初の入力の番号
            const int start = static_cast<int> (first - base);
            if (start + taps > count)
                break;
            const double fracPos = (t - ft) * kPhases;
            const int p = static_cast<int> (fracPos);
            const float a = static_cast<float> (fracPos - p);
            const float* h0 = table.data() + p * taps;
            const float* h1 = h0 + taps;
            const float* x = buf.data() + start;
            float y = 0;
            for (int k = 0; k < taps; ++k)
                y += x[k] * (h0[k] + (h1[k] - h0[k]) * a);
            out.push_back (y);
            t += step;
        }

        // 使い終わった入力を詰める
        const long long keepFrom = static_cast<long long> (std::floor (t)) - half + 1;
        const int drop = static_cast<int> (std::max (0LL, keepFrom - base));
        if (drop > 0)
        {
            std::copy (buf.begin() + drop, buf.begin() + count, buf.begin());
            count -= drop;
            base += drop;
        }
    }
}


namespace
{
constexpr int kAiFrame = 480; // 1 回分（48kHz で 10ms）
} // namespace

VoiceDetector::~VoiceDetector()
{
    stopWorker.store (true);
    wake.notify_one();
    if (worker.joinable())
        worker.join();
    if (state != nullptr)
        rnnoise_destroy (static_cast<DenoiseState*> (state));
}


void VoiceDetector::prepare (double sampleRate, int maxBlockSize)
{
    // 作業スレッドを止めてから作り直す
    if (worker.joinable())
    {
        stopWorker.store (true);
        wake.notify_one();
        worker.join();
    }
    stopWorker.store (false);
    if (state == nullptr)
        state = rnnoise_create (nullptr);
    ratio = sampleRate / 48000.0;
    resample = std::abs (sampleRate - 48000.0) > 0.5;
    if (resample)
        up.prepare (sampleRate, 48000.0);
    in48.reserve (static_cast<size_t> (std::max (maxBlockSize, 4096) * 2 + 64));
    ring.assign (kRing, 0.0f);
    frameIn.assign (kAiFrame, 0.0f);
    frameOut.assign (kAiFrame, 0.0f);
    written.store (0);
    judged.store (0);
    lastVoice.store (0);
    restartAt.store (0);
    epoch.store (0);
    seenEpoch.store (-1);
    reset();
    worker = std::thread ([this] { workerLoop(); });
}


void VoiceDetector::reset()
{
    // 判定する側に、今の位置から始め直すように伝える（RNNoise の状態も消す）
    if (resample)
        up.reset();
    restartAt.store (written.load (std::memory_order_relaxed), std::memory_order_release);
    epoch.fetch_add (1, std::memory_order_acq_rel);
    if (! realtime.load())
    {
        std::lock_guard<std::mutex> lock (judgeMutex);
        processFrames();
    }
    wake.notify_one();
}


bool VoiceDetector::processFrames()
{
    if (state == nullptr)
        return false;
    const int e = epoch.load (std::memory_order_acquire);
    if (e != seenEpoch.load (std::memory_order_relaxed))
    {
        rnnoise_init (static_cast<DenoiseState*> (state), nullptr);
        const auto start = restartAt.load (std::memory_order_acquire);
        lastVoice.store (start, std::memory_order_relaxed);
        judged.store (start, std::memory_order_release);
        seenEpoch.store (e, std::memory_order_release);
    }
    auto pos = judged.load (std::memory_order_relaxed);
    const auto end = written.load (std::memory_order_acquire);
    if (end - pos > kRing / 2)
    {
        // 追いつけないほど遅れた（リングを上書きされる前に飛ばす。飛ばした所は「声あり」だったかもしれないとみなす）
        pos = end - kAiFrame;
        lastVoice.store (pos, std::memory_order_relaxed);
        judged.store (pos, std::memory_order_release);
    }
    bool did = false;
    while (pos + kAiFrame <= end)
    {
        for (int i = 0; i < kAiFrame; ++i)
            frameIn[static_cast<size_t> (i)] = ring[static_cast<size_t> ((pos + i) % kRing)] * 32768.0f; // RNNoise は 16 ビットの整数の大きさで扱う
        const float prob = rnnoise_process_frame (static_cast<DenoiseState*> (state), frameOut.data(), frameIn.data());
        pos += kAiFrame;
        if (prob > threshold.load (std::memory_order_relaxed))
            lastVoice.store (pos, std::memory_order_relaxed);
        judged.store (pos, std::memory_order_release); // lastVoice を書いた後に進める（読む側は judged を先に読む）
        did = true;
    }
    return did;
}


void VoiceDetector::workerLoop()
{
    while (! stopWorker.load())
    {
        bool did = false;
        if (realtime.load())
        {
            std::lock_guard<std::mutex> lock (judgeMutex);
            did = processFrames();
        }
        if (! did)
        {
            // 次の 480 サンプルがそろうまで待つ（音の処理のスレッドが起こす。取りこぼしても 20ms で見直す）
            std::unique_lock<std::mutex> lock (wakeMutex);
            wake.wait_for (lock, std::chrono::milliseconds (20));
        }
    }
}


int VoiceDetector::quietSamples() const noexcept
{
    if (seenEpoch.load (std::memory_order_acquire) != epoch.load (std::memory_order_acquire))
        return 0; // リセットを、判定する側がまだ受け取っていない
    const auto w = written.load (std::memory_order_relaxed);
    const auto j = judged.load (std::memory_order_acquire);
    const auto v = lastVoice.load (std::memory_order_relaxed);
    const auto backlog = w - j; // まだ判定していない分（「声あり」かもしれない）
    if (backlog > kMaxBacklog)
        return 0;
    const auto quiet = j - v - backlog;
    return quiet > 0 ? static_cast<int> (std::min (static_cast<double> (quiet) * ratio, 1.0e9)) : 0;
}


void VoiceDetector::process (const float* data, int n)
{
    if (state == nullptr)
        return;
    const float* x = data;
    int count = n;
    if (resample)
    {
        in48.clear();
        up.process (data, n, in48);
        x = in48.data();
        count = static_cast<int> (in48.size());
    }
    const auto w = written.load (std::memory_order_relaxed);
    for (int i = 0; i < count; ++i)
        ring[static_cast<size_t> ((w + i) % kRing)] = x[i];
    written.store (w + count, std::memory_order_release);
    if (realtime.load())
    {
        wake.notify_one();
    }
    else
    {
        std::lock_guard<std::mutex> lock (judgeMutex);
        processFrames();
    }
}

} // namespace vcab
