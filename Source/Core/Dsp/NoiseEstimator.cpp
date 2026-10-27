#include "NoiseEstimator.h"

#include <limits>

namespace vcab
{

void NoiseEstimator::prepare (int bins, double frameRate, double sampleRate, int settleSamples)
{
    channels = bins;
    frameRateHz = frameRate;
    sampleRateHz = sampleRate;
    const auto c = static_cast<size_t> (channels);

    quietLength = std::max (1, static_cast<int> (std::lround (0.002 * sampleRate))); // 2ms
    settleLength = std::max (0, settleSamples);

    smoothCoeff = 1.0 - std::exp (-1.0 / (frameRate * kSmoothSeconds));
    subLength = std::max (1, static_cast<int> (std::lround (kMinWindow / kSubWindows * frameRate)));
    smooth.assign (c, 0.0f);
    subMin.assign (c, 0.0f);
    autoMin.assign (c, 0.0f);
    mins.assign (kSubWindows, std::vector<float> (c, 0.0f));

    vadDelay = std::max (1, static_cast<int> (std::ceil (kVadDelaySeconds * frameRate)));
    vadQueue.assign (static_cast<size_t> (vadDelay), std::vector<float> (c, 0.0f));
    vadEst.assign (c, 0.0f);
    vadSlowCoeff = 1.0 - std::exp (-1.0 / (frameRate * 0.4));  // 声のない間は、約 0.4 秒でならして追う
    vadStuckFrames = static_cast<int> (std::lround (4.0 * frameRate));
    vadStuckCoeff = 1.0 - std::exp (-1.0 / (frameRate * 2.0));
    // 遅らせたフレームの分析の窓（settleLength）の前 50ms から、今（判定が出ている所）までが、ずっと「声なし」なら使う
    vadNeedSamples = static_cast<int> (std::lround ((kVadDelaySeconds + 0.05) * sampleRate)) + settleLength;

    learnWaitFrames = std::max (1, static_cast<int> (std::lround (kLearnWaitSeconds * frameRate)));
    learnSum.assign (c, 0.0);
    learned.assign (c, 0.0f);
    learnedValid = learnActive = false;
    reset();
}

void NoiseEstimator::reset()
{
    first = true;
    subCount = subIndex = filled = 0;
    quietRun = validRun = 0;
    std::fill (subMin.begin(), subMin.end(), std::numeric_limits<float>::max());
    std::fill (autoMin.begin(), autoMin.end(), 0.0f); // 最初の見積もりができるまでは下げない
    vadPos = vadFilled = vadUpdates = vadSinceUpdate = 0;
    std::fill (vadEst.begin(), vadEst.end(), 0.0f);
    learnActive = false;
}

void NoiseEstimator::trackMinimum (const float* power)
{
    for (int k = 0; k < channels; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const float p = std::max (power[k], kTiny);
        smooth[i] = first ? p : smooth[i] + static_cast<float> (smoothCoeff) * (p - smooth[i]);
        subMin[i] = std::min (subMin[i], smooth[i]);
        float m = subMin[i];
        for (int u = 0; u < filled; ++u)
            m = std::min (m, mins[static_cast<size_t> (u)][i]);
        autoMin[i] = m * bias;
    }
    first = false;
    if (++subCount >= subLength)
    {
        mins[static_cast<size_t> (subIndex)] = subMin;
        subIndex = (subIndex + 1) % kSubWindows;
        filled = std::min (filled + 1, kSubWindows);
        std::fill (subMin.begin(), subMin.end(), std::numeric_limits<float>::max());
        subCount = 0;
    }
}

void NoiseEstimator::push (const float* power, int voiceQuiet)
{
    if (calibrating)
    {
        calibrationFrame (power);
        return;
    }
    if (validRun < settleLength)
        return; // 無音の後で、分析の窓がまだ実際の音で埋まっていない

    trackMinimum (power);

    // 自動（声の判定）: vadDelay フレーム前の大きさを、その前後がずっと「声なし」と確かめられたときだけ使う
    {
        auto& slot = vadQueue[static_cast<size_t> (vadPos)];
        if (vadFilled >= vadDelay && voiceQuiet >= vadNeedSamples)
        {
            // 最初は平均（早く落ち着くように）、その後は約 0.4 秒でならす
            const double a = std::max (1.0 / (vadUpdates + 1), vadSlowCoeff);
            for (int k = 0; k < channels; ++k)
            {
                const auto i = static_cast<size_t> (k);
                double ak = a;
                // 見積もりより急に大きい区画（一瞬の音）は、ごく弱くしか使わない（落ち着いた後だけ）
                if (vadUpdates > vadDelay && slot[i] > vadEst[i] * kOutlier)
                    ak *= 0.02;
                vadEst[i] += static_cast<float> (ak) * (slot[i] - vadEst[i]);
            }
            ++vadUpdates;
            vadSinceUpdate = 0;
        }
        else if (vadUpdates > 0 && ++vadSinceUpdate > vadStuckFrames)
        {
            // 長く「声あり」が続くときは、最小値の見積もりの方が小さい区画だけ、そちらへゆっくり下げる
            for (int k = 0; k < channels; ++k)
            {
                auto& v = vadEst[static_cast<size_t> (k)];
                v += static_cast<float> (vadStuckCoeff) * std::min (0.0f, autoMin[static_cast<size_t> (k)] - v);
            }
        }
        for (int k = 0; k < channels; ++k)
            slot[static_cast<size_t> (k)] = std::max (power[k], kTiny);
        vadPos = (vadPos + 1) % vadDelay;
        vadFilled = std::min (vadFilled + 1, vadDelay);
    }

    if (learnActive)
    {
        if (learnWait > 0)
        {
            --learnWait;
        }
        else
        {
            for (int k = 0; k < channels; ++k)
                learnSum[static_cast<size_t> (k)] += std::max (power[k], kTiny);
            ++learnCount;
        }
    }
}

void NoiseEstimator::startLearning()
{
    learnActive = true;
    learnWait = learnWaitFrames;
    learnCount = 0;
    std::fill (learnSum.begin(), learnSum.end(), 0.0);
}

void NoiseEstimator::stopLearning()
{
    if (! learnActive)
        return;
    learnActive = false;
    if (learnCount <= 0)
        return;
    for (int k = 0; k < channels; ++k)
        learned[static_cast<size_t> (k)] = static_cast<float> (learnSum[static_cast<size_t> (k)] / learnCount);
    learnedValid = true;
}

void NoiseEstimator::clearLearned()
{
    learnedValid = false;
    learnActive = false;
}

void NoiseEstimator::beginCalibration()
{
    calibrating = true;
    bias = 1.0f;
    calibFrames.clear();
}

void NoiseEstimator::calibrationFrame (const float* power)
{
    calibFrames.emplace_back (power, power + channels);
}

void NoiseEstimator::finishCalibration()
{
    calibrating = false;
    // 測った大きさを push と同じ手順で最小値を追い直し、2 秒の後の「平均 / 最小値」を倍率にする（全区画で同じ値）
    reset();
    const int warmup = static_cast<int> (std::lround (2.0 * frameRateHz));
    double sumP = 0, sumM = 0;
    for (size_t f = 0; f < calibFrames.size(); ++f)
    {
        trackMinimum (calibFrames[f].data());
        if (static_cast<int> (f) < warmup)
            continue;
        // 端の区画（直流とナイキスト）は、ほかと統計が違うので入れない
        for (int k = 1; k + 1 < channels; ++k)
        {
            sumP += calibFrames[f][static_cast<size_t> (k)];
            sumM += autoMin[static_cast<size_t> (k)];
        }
    }
    if (sumM > 0)
        bias = static_cast<float> (sumP / sumM);
    calibFrames.clear();
    calibFrames.shrink_to_fit();
    reset();
}

} // namespace vcab
