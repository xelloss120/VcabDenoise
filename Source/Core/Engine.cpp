#include "Engine.h"

#include "Dsp/MathConstants.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vcab
{

namespace
{
constexpr double kCrossLowHz = 350, kCrossHighHz = 700; // 中くらいの側と短い側をつなぐ範囲

// 倍率を測るための白色雑音（毎回同じ並び）
struct WhiteNoise
{
    uint32_t state = 0x12345678u;
    float next() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (static_cast<float> (state) / 4294967296.0f * 2.0f - 1.0f) * 0.1f;
    }
};

// 区画の値（dB）を、区画の間の位置 pos で直線補間する
double interpDb (const std::vector<float>& db, double pos)
{
    const int last = static_cast<int> (db.size()) - 1;
    pos = std::clamp (pos, 0.0, static_cast<double> (last));
    const int i = std::min (static_cast<int> (pos), last - 1);
    const double t = pos - i;
    const double a = db[static_cast<size_t> (i)], b = db[static_cast<size_t> (i + 1)];
    return a + (b - a) * t;
}

GainSettings gainSettings (const ParamValues& p)
{
    GainSettings s;
    s.maxReductionDb = p.get (P::maxReduction);
    s.strengthDb = p.get (P::strength);
    s.attackMs = p.get (P::attackMs);
    s.releaseMs = p.get (P::releaseMs);
    s.presence = p.on (P::presenceOn);
    s.cepstrum = p.on (P::cepstrumOn);
    s.onsetProtect = p.on (P::onsetProtect);
    return s;
}

TonalSettings tonalSettings (const ParamValues& p)
{
    TonalSettings s;
    s.sensitivityDb = p.get (P::tonalSensitivity);
    s.count = p.index (P::tonalCount);
    s.maxDepthDb = p.get (P::tonalMaxDepth);
    s.lowHz = p.get (P::tonalLowHz);
    s.highHz = p.get (P::tonalHighHz);
    s.neighborHz = p.get (P::tonalNeighborHz);
    s.activateSec = p.get (P::tonalActivateSec);
    s.releaseSec = p.get (P::tonalReleaseSec);
    s.jumpSec = p.get (P::tonalJumpSec);
    s.jumpRange = p.get (P::tonalJumpRange) * 0.01;
    s.width2k = p.get (P::tonalWidth);
    s.lowWidth = p.get (P::tonalLowWidth);
    s.quietDb = p.get (P::tonalQuietDb);
    return s;
}
} // namespace

Engine::Engine()
{
    for (auto& v : meters.noiseCurveDb)
        v.store (-200.0f);
}

double Engine::crossoverLow (double hz)
{
    if (hz <= kCrossLowHz)
        return 1.0;
    if (hz >= kCrossHighHz)
        return 0.0;
    // 対数の周波数で、なめらかに 1 → 0
    const double t = std::log (hz / kCrossLowHz) / std::log (kCrossHighHz / kCrossLowHz);
    return 0.5 + 0.5 * std::cos (kPi * t);
}

void Engine::prepareStage (Stage& st, int size, bool lowSide)
{
    const int hop = size / 4;
    st.analyzer.prepare (size, hop, FrameAnalyzer::Window::recentHeavy);
    st.noise.prepare (size / 2 + 1, sr / hop, sr, size);
    st.rule.prepare (size, sr, hop);
    st.designer.prepare (2 * size);
    st.fir.prepare (size, blockSize, hop);
    st.gainDb.assign (static_cast<size_t> (size / 2 + 1), 0.0f);
    st.h.assign (static_cast<size_t> (size), 0.0f);
    st.logMag.assign (static_cast<size_t> (size + 1), 0.0);
    st.weight.assign (static_cast<size_t> (size + 1), 0.0);
    for (int k = 0; k <= size; ++k)
    {
        const double hz = k * sr / (2.0 * size);
        st.weight[static_cast<size_t> (k)] = lowSide ? crossoverLow (hz) : 1.0 - crossoverLow (hz);
    }
    st.identity = true;
}

void Engine::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    scale = sr >= 176400.0 ? 4 : (sr >= 88200.0 ? 2 : 1);
    blockSize = 128 * scale;

    prepareStage (shortSide, 512 * scale, false);
    prepareStage (midSide, 2048 * scale, true);

    const int tonalSize = 4096 * scale;
    tonalAnalyzer.prepare (tonalSize, tonalSize / 4, FrameAnalyzer::Window::hann);
    tonal.prepare (sr, tonalSize, (tonalSize / 4) / sr);
    tonalDesigner.prepare (2 * tonalSize);
    tonalFir.prepare (tonalSize, blockSize, tonalSize / 4);
    tonalH.assign (static_cast<size_t> (tonalSize), 0.0f);
    tonalLogMag.assign (static_cast<size_t> (tonalSize + 1), 0.0);
    tonalFreqs.assign (static_cast<size_t> (tonalSize + 1), 0.0);
    for (int k = 0; k <= tonalSize; ++k)
        tonalFreqs[static_cast<size_t> (k)] = k * sr / (2.0 * tonalSize);
    tonalIdentity = true;
    notchCount = 0;

    // 分析（と FIR の作り直し）の時期を段ごとにずらす。短い側は毎塊、中くらいの側は 4 塊ごと、長い側は 8 塊ごとなので、
    // そのままだと 8 塊に 1 回、3 段の重い処理が同じ塊に重なる。中くらいの側を 2 塊、長い側を 4 塊早めて、
    // 中くらいの側は 1・5 番目、長い側は 3 番目の塊に分ける（1 塊の処理が一番重いときを軽くする。平均の負荷は変わらない）
    midSide.analyzer.setFirstHopEarlier (2 * blockSize);
    tonalAnalyzer.setFirstHopEarlier (4 * blockSize);

    monitorDesigner.prepare (2 * tonalSize);
    monitorFir.prepare (tonalSize, blockSize, tonalSize / 4);
    monitorH.assign (static_cast<size_t> (tonalSize), 0.0f);
    monitorLogMag.assign (static_cast<size_t> (tonalSize + 1), 0.0);
    monitorOn = false;

    vad.prepare (sr, std::max (maxBlockSize, blockSize));
    voicedQuiet = static_cast<int> (0.05 * sr);
    chunkIn.assign (static_cast<size_t> (blockSize), 0.0f);
    chunkTonal.assign (static_cast<size_t> (blockSize), 0.0f);
    mixStep = static_cast<float> (1.0 / (0.01 * sr));
    meterCoef = 1.0 - std::exp (-1.0 / (0.3 * sr));

    // 最小値の見積もりを本来の大きさに戻す倍率を、白色雑音で測る（6 秒）
    {
        WhiteNoise noise;
        for (auto* st : { &shortSide, &midSide })
            st->noise.beginCalibration();
        const int total = static_cast<int> (6.0 * sr);
        for (int i = 0; i < total; ++i)
        {
            const float x = noise.next();
            for (auto* st : { &shortSide, &midSide })
                if (st->analyzer.push (x))
                    st->noise.push (st->analyzer.powers(), 0);
        }
        for (auto* st : { &shortSide, &midSide })
        {
            st->noise.finishCalibration();
            st->analyzer.reset();
        }
    }

    // 学習した値の曲線を作る場所（区画の数から大きさが決まる）
    {
        std::lock_guard<std::mutex> lock (curveMutex);
        const size_t points = static_cast<size_t> (midSide.analyzer.bins() + shortSide.analyzer.bins());
        curveScratch.reserve (2 + points * 2);
        learnedCurve.reserve (2 + points * 2);
        if (! learnedCurve.empty())
            applyLearnedCurve (learnedCurve); // サンプルレートが変わっても、覚えた値を使い続ける
    }
    hasPendingCurve.store (false);

    phase = 0;
    bypassMix = noiseOnlyMix = 0;
    inPower = outPower = 0;
    wasLearning = false;
    meters.noiseCurveValid.store (false);
}

void Engine::handleRequests()
{
    switch (learnRequest.exchange (LearnRequest::none))
    {
        case LearnRequest::start:
            shortSide.noise.startLearning();
            midSide.noise.startLearning();
            break;
        case LearnRequest::stop:
            shortSide.noise.stopLearning();
            midSide.noise.stopLearning();
            curveDirty.store (true);
            break;
        case LearnRequest::clear:
            shortSide.noise.clearLearned();
            midSide.noise.clearLearned();
            curveDirty.store (true);
            break;
        case LearnRequest::none:
            break;
    }

    // 画面のスレッドとは try_lock だけで受け渡す（音の処理のスレッドを待たせない。取れなければ次の塊で）
    if (hasPendingCurve.load() || curveDirty.load())
    {
        std::unique_lock<std::mutex> lock (curveMutex, std::try_to_lock);
        if (lock.owns_lock())
        {
            if (hasPendingCurve.exchange (false))
                applyLearnedCurve (pendingCurve);
            if (curveDirty.exchange (false))
                storeLearnedCurve();
        }
    }

    meters.learning.store (shortSide.noise.learning());
    meters.learned.store (shortSide.noise.hasLearned());
}

void Engine::storeLearnedCurve()
{
    // 中くらいの側の 1kHz まで、その上は短い側の区画を、周波数と「白色雑音に直した大きさ」の組にする
    curveScratch.clear();
    if (shortSide.noise.hasLearned() && midSide.noise.hasLearned())
    {
        curveScratch.push_back (1.0f);
        curveScratch.push_back (0.0f);
        int points = 0;
        auto add = [&] (Stage& st, double fromHz, double toHz)
        {
            const double binHz = sr / st.analyzer.fftSize();
            const auto& v = st.noise.learnedValues();
            for (int k = 0; k < st.analyzer.bins(); ++k)
            {
                const double hz = k * binHz;
                if (hz < fromHz || hz >= toHz)
                    continue;
                curveScratch.push_back (static_cast<float> (hz));
                curveScratch.push_back (static_cast<float> (10.0 * std::log10 (v[static_cast<size_t> (k)] * st.analyzer.toDensity() + 1.0e-30)));
                ++points;
            }
        };
        add (midSide, 0.0, 1000.0);
        add (shortSide, 1000.0, 1.0e9);
        curveScratch[1] = static_cast<float> (points);
    }
    std::swap (curveScratch, learnedCurve);
}

void Engine::applyLearnedCurve (const std::vector<float>& curve)
{
    if (curve.size() < 6 || curve[0] != 1.0f)
        return;
    const int points = static_cast<int> (curve[1]);
    if (points < 2 || static_cast<int> (curve.size()) < 2 + points * 2)
        return;
    auto hzAt = [&] (int i) { return static_cast<double> (curve[static_cast<size_t> (2 + i * 2)]); };
    auto dbAt = [&] (int i) { return static_cast<double> (curve[static_cast<size_t> (3 + i * 2)]); };
    for (auto* st : { &shortSide, &midSide })
    {
        const double binHz = sr / st->analyzer.fftSize();
        auto& v = st->noise.learnedValues();
        int j = 0;
        for (int k = 0; k < st->analyzer.bins(); ++k)
        {
            const double hz = k * binHz;
            while (j + 2 < points && hzAt (j + 1) < hz)
                ++j;
            const double h0 = hzAt (j), h1 = hzAt (j + 1);
            const double t = h1 > h0 ? std::clamp ((hz - h0) / (h1 - h0), 0.0, 1.0) : 0.0;
            const double db = dbAt (j) + (dbAt (j + 1) - dbAt (j)) * t;
            v[static_cast<size_t> (k)] = static_cast<float> (std::pow (10.0, db / 10.0) / st->analyzer.toDensity());
        }
        st->noise.setLearned (true);
    }
}

std::vector<float> Engine::exportLearned()
{
    std::lock_guard<std::mutex> lock (curveMutex);
    return learnedCurve;
}

void Engine::importLearned (std::vector<float> curve)
{
    std::lock_guard<std::mutex> lock (curveMutex);
    learnedCurve = curve;
    learnedCurve.reserve (curveScratch.capacity()); // 音の処理のスレッドで入れ替えた後も、作り直しで確保が起きないように
    pendingCurve = std::move (curve);
    hasPendingCurve.store (true);
}

double Engine::totalGainDb (double hz) const
{
    double db = 0;
    TonalTracker::addShape (notchList.data(), notchCount, &hz, 1, &db);
    const double w = crossoverLow (hz);
    if (w > 0)
        db += w * interpDb (midSide.gainDb, hz / (sr / midSide.analyzer.fftSize()));
    if (w < 1)
        db += (1.0 - w) * interpDb (shortSide.gainDb, hz / (sr / shortSide.analyzer.fftSize()));
    return db;
}

void Engine::onBroadbandFrame (Stage& st, const ParamValues& p, bool useLearned)
{
    st.noise.push (st.analyzer.powers(), quietNow);
    st.rule.compute (st.analyzer.powers(), st.noise.estimate (useLearned), gainSettings (p), st.gainDb.data());

    // 設計の区画は、分析の区画の 2 倍の細かさ（区画の間は dB で直線補間）。受け持たない帯域は 0dB（素通し）
    const int designBins = static_cast<int> (st.logMag.size());
    double largest = 0;
    for (int k = 0; k < designBins; ++k)
    {
        const double w = st.weight[static_cast<size_t> (k)];
        const double db = w > 0 ? w * interpDb (st.gainDb, k * 0.5) : 0.0;
        st.logMag[static_cast<size_t> (k)] = db * kDbToLn;
        largest = std::max (largest, -db);
    }
    if (largest < 0.01)
    {
        if (! st.identity)
            st.fir.setIdentity();
        st.identity = true;
    }
    else
    {
        st.designer.design (st.logMag.data(), st.h.data(), static_cast<int> (st.h.size()));
        st.fir.setFilter (st.h.data());
        st.identity = false;
    }

    if (&st == &midSide)
        updateNoiseCurve (useLearned);
}

void Engine::onTonalFrame (const ParamValues& p)
{
    const auto s = tonalSettings (p);
    if (p.on (P::tonalOn))
    {
        // 分析の窓（85ms）に声が残っている間も「声あり」とみなす（声が止まった直後に、窓の中の声の倍音を、狭帯域ノイズとして拾わないように）
        const bool voicedInWindow = quietNow < voicedQuiet + tonalAnalyzer.fftSize();
        tonal.analyze (tonalAnalyzer.powers(), tonalAnalyzer.toAmplitude(), voicedInWindow, s);
        notchCount = tonal.notches (notchList.data(), s);
    }
    else
    {
        tonal.reset();
        notchCount = 0;
    }
    meters.notchCount.store (notchCount);
    for (int i = 0; i < notchCount; ++i)
    {
        meters.notchHz[static_cast<size_t> (i)].store (notchList[static_cast<size_t> (i)].freq);
        meters.notchDepthDb[static_cast<size_t> (i)].store (notchList[static_cast<size_t> (i)].depthDb);
        meters.notchWidthHz[static_cast<size_t> (i)].store (notchList[static_cast<size_t> (i)].widthHz);
    }

    if (notchCount == 0)
    {
        if (! tonalIdentity)
            tonalFir.setIdentity();
        tonalIdentity = true;
    }
    else
    {
        std::vector<double>& db = tonalLogMag;
        std::fill (db.begin(), db.end(), 0.0);
        TonalTracker::addShape (notchList.data(), notchCount, tonalFreqs.data(), static_cast<int> (tonalFreqs.size()), db.data());
        for (auto& v : db)
            v *= kDbToLn;
        tonalDesigner.design (db.data(), tonalH.data(), static_cast<int> (tonalH.size()));
        tonalFir.setFilter (tonalH.data());
        tonalIdentity = false;
    }

    if (monitorOn)
        updateMonitor();
}

void Engine::updateMonitor()
{
    // 削った分の大きさ √(1 − G²)（G は 3 段を合わせた残す割合）
    for (size_t k = 0; k < monitorLogMag.size(); ++k)
    {
        const double g = std::pow (10.0, totalGainDb (tonalFreqs[k]) / 20.0);
        monitorLogMag[k] = 0.5 * std::log (std::max (1.0 - g * g, 1.0e-6));
    }
    monitorDesigner.design (monitorLogMag.data(), monitorH.data(), static_cast<int> (monitorH.size()));
    monitorFir.setFilter (monitorH.data());
}

void Engine::updateNoiseCurve (bool useLearned)
{
    // 500Hz より下は中くらいの側、上は短い側の見積もり
    for (int i = 0; i < EngineMeters::kNoiseCurvePoints; ++i)
    {
        const double hz = EngineMeters::noiseCurveHz (i);
        const Stage& st = hz < 500.0 ? midSide : shortSide;
        const double binHz = sr / st.analyzer.fftSize();
        const float* est = st.noise.estimate (useLearned);
        const double pos = std::clamp (hz / binHz, 0.0, static_cast<double> (st.analyzer.bins() - 1));
        const int k = std::min (static_cast<int> (pos), st.analyzer.bins() - 2);
        const double t = pos - k;
        const double v = est[k] + (est[k + 1] - est[k]) * t;
        meters.noiseCurveDb[static_cast<size_t> (i)].store (static_cast<float> (10.0 * std::log10 (v * st.analyzer.toDensity() + 1.0e-30)));
    }
    meters.noiseCurveValid.store (true);
}

void Engine::process (float* data, int n, const ParamValues& p)
{
    handleRequests();
    vad.setThreshold (p.get (P::rnnThreshold) * 0.01f);
    const bool useLearned = static_cast<NoiseMode> (p.index (P::noiseMode)) == NoiseMode::learned;

    // 抑制ノイズ試聴: 入れたらすぐ（次の塊から）特性を作る。切ったら素通しに戻す
    if (p.on (P::noiseOnly) != monitorOn)
    {
        monitorOn = p.on (P::noiseOnly);
        if (monitorOn)
            updateMonitor();
        else
            monitorFir.setIdentity();
    }
    const float bypassTarget = p.on (P::bypass) ? 1.0f : 0.0f;
    const float noiseOnlyTarget = p.on (P::noiseOnly) ? 1.0f : 0.0f;

    double sumIn = 0, sumOut = 0;
    for (int done = 0; done < n;)
    {
        // FIR の塊と分析の間隔（どちらも blockSize の倍数）にそろえて、区切って処理する
        const int len = std::min (n - done, blockSize - phase);

        // 1. 長い FIR（狭帯域ノイズ）と、その分析（入力で）
        for (int i = 0; i < len; ++i)
        {
            const float x = data[done + i];
            chunkIn[static_cast<size_t> (i)] = x;
            chunkTonal[static_cast<size_t> (i)] = tonalFir.process (x);
            if (tonalAnalyzer.push (x))
                onTonalFrame (p);
        }

        // 2. 声の判定（狭帯域ノイズを消した後の音で）
        vad.process (chunkTonal.data(), len);
        quietNow = vad.quietSamples();
        voiced = quietNow < voicedQuiet;

        // 3. 中くらいの FIR と短い FIR、それぞれの分析（狭帯域ノイズを消した後の音で）
        for (int i = 0; i < len; ++i)
        {
            const float x = chunkIn[static_cast<size_t> (i)];
            const float y1 = chunkTonal[static_cast<size_t> (i)];
            const float y2 = midSide.fir.process (y1);
            const float y3 = shortSide.fir.process (y2);
            const float removed = monitorFir.process (x);

            for (auto* st : { &midSide, &shortSide })
            {
                st->noise.observe (y1);
                if (st->analyzer.push (y1))
                    onBroadbandFrame (*st, p, useLearned);
            }

            noiseOnlyMix += std::clamp (noiseOnlyTarget - noiseOnlyMix, -mixStep, mixStep);
            bypassMix += std::clamp (bypassTarget - bypassMix, -mixStep, mixStep);
            float out = y3 + (removed - y3) * noiseOnlyMix;
            out += (x - out) * bypassMix;
            data[done + i] = out;

            inPower += meterCoef * (static_cast<double> (x) * x - inPower);
            outPower += meterCoef * (static_cast<double> (y3) * y3 - outPower);
            sumIn += static_cast<double> (x) * x;
            sumOut += static_cast<double> (out) * out;
        }

        done += len;
        phase = (phase + len) % blockSize;
    }

    meters.voice.store (voiced);
    meters.reductionDb.store (static_cast<float> (std::max (0.0, 10.0 * std::log10 ((inPower + 1.0e-20) / (outPower + 1.0e-20)))));
    auto toDb = [] (double power) { return static_cast<float> (10.0 * std::log10 (power + 1.0e-12)); };
    if (n > 0)
    {
        // 入出力の音量は、塊の RMS を約 0.3 秒でならす（画面のメーター用）
        const float a = static_cast<float> (1.0 - std::exp (-n / (0.3 * sr)));
        const float inDb = toDb (sumIn / n), outDb = toDb (sumOut / n);
        meters.inputDb.store (meters.inputDb.load() + a * (inDb - meters.inputDb.load()));
        meters.outputDb.store (meters.outputDb.load() + a * (outDb - meters.outputDb.load()));
    }
}

} // namespace vcab
