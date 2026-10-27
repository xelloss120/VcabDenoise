// 広い帯域のノイズ推定（区画ごとの大きさの 2 乗）。短い側と中くらいの側で 1 つずつ使う。
// VoiChanAlgoBox の NoiseTracker のうち、使う見積もり方だけを残し、学習を「押してから、もう一度押すまで」に変えたもの。
//
//   自動追従（RNNoise の声の判定）: 声の判定（VAD）が「声なし」とした間だけ更新する。判定は少し遅れて出るので、分析した大きさを
//     60ms 遅らせ、その前後がずっと「声なし」のものだけを、約 0.4 秒でならして使う。
//     「声なし」の間でも、見積もりより 10dB 以上急に大きい区画は、ごく弱くしか使わない（ボタンや関節の音は「声なし」と
//     判定されるので、そのままだと一瞬の音で見積もりが持ち上がり、そのあと声が削れるため）。
//     4 秒以上「声あり」が続いたら、最小値の見積もりのほうが小さい区画だけ、そちらへゆっくり下げる（上げる向きには動かさない）。
//     まだ一度も「声なし」を確かめられていない（起動直後にいきなり話し始めた）ときは、最小値の見積もりを使う。
//   最小値: 区画ごとの、ならした大きさの最小値を、直近 1.6 秒で追いかける（Martin の minimum statistics の簡単な形）。
//     最小値は平均より小さく出るので、白色雑音で測った倍率で直す（prepare の後に calibrate で測る）。
//   学習固定: startLearning から stopLearning までの平均（押してから約 1 秒は覚えない。長い側が狭帯域ノイズを削り始めるまで待つ）。
//     学習した後は動かさない。まだ覚えていないときは、自動追従の値を使う。
// 完全な無音（デジタルの 0）の後は、分析の窓が実際の音で埋まるまで見積もりを更新しない（VoiChanAlgoBox と同じ）。
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace vcab
{

class NoiseEstimator
{
public:
    void prepare (int bins, double frameRate, double sampleRate, int settleSamples);
    void reset();          // 自動追従の状態だけ消す（学習した値は残す）

    void observe (float x) noexcept
    {
        if (x > -kSilentLevel && x < kSilentLevel)
        {
            if (++quietRun >= quietLength)
            {
                validRun = 0;
                return;
            }
        }
        else
        {
            quietRun = 0;
        }
        if (validRun < settleLength)
            ++validRun;
    }
    // 1 フレーム分の、区画ごとの大きさ（2 乗）。voiceQuiet: 声の判定で「声なし」と確かめられた長さ（サンプル数、今から数えて）
    void push (const float* power, int voiceQuiet);

    void startLearning();
    void stopLearning();     // それまでの平均を覚える（覚える前に止めたら、前の値のまま）
    void clearLearned();
    bool learning() const noexcept { return learnActive; }
    bool hasLearned() const noexcept { return learnedValid; }

    const float* estimate (bool useLearned) const noexcept
    {
        if (useLearned && learnedValid)
            return learned.data();
        return vadUpdates > 0 ? vadEst.data() : autoMin.data();
    }
    std::vector<float>& learnedValues() noexcept { return learned; }
    void setLearned (bool valid) noexcept { learnedValid = valid; }

    // 最小値を本来の大きさに戻す倍率を、白色雑音で測る（beginCalibration → calibrationFrame × 数 → finishCalibration）
    void beginCalibration();
    void calibrationFrame (const float* power);
    void finishCalibration();

private:
    static constexpr int kSubWindows = 8;
    static constexpr double kMinWindow = 1.6;           // 最小値を探す長さ（秒）
    static constexpr double kSmoothSeconds = 0.05;      // 最小値を追う前に、大きさをならす時間
    static constexpr double kVadDelaySeconds = 0.06;    // 声の判定の遅れ（RNNoise の 1 回分 10ms ＋ サンプルレートの変換）より十分長く
    static constexpr double kLearnWaitSeconds = 1.0;    // 学習を押してから覚え始めるまで
    static constexpr float kOutlier = 10.0f;            // 見積もりよりこれだけ（10dB）大きい区画は、ごく弱くしか使わない
    static constexpr float kSilentLevel = 1.0e-6f;      // -120dBFS より小さいサンプルが quietLength 続いたら、無音とみなす
    static constexpr float kTiny = 1.0e-20f;

    void trackMinimum (const float* power);

    int channels = 0;
    double frameRateHz = 100, sampleRateHz = 48000;
    int quietLength = 96, settleLength = 0, quietRun = 0, validRun = 0;

    // 最小値
    double smoothCoeff = 0.1;
    int subLength = 1, subCount = 0, subIndex = 0, filled = 0;
    bool first = true;
    std::vector<float> smooth, subMin, autoMin;
    std::vector<std::vector<float>> mins;
    float bias = 1.5f;
    bool calibrating = false;
    std::vector<std::vector<float>> calibFrames;

    // 自動追従（声の判定）
    std::vector<std::vector<float>> vadQueue;
    std::vector<float> vadEst;
    int vadDelay = 1, vadPos = 0, vadFilled = 0, vadUpdates = 0, vadSinceUpdate = 0, vadStuckFrames = 1, vadNeedSamples = 0;
    double vadSlowCoeff = 0.01, vadStuckCoeff = 0.01;

    // 学習
    bool learnActive = false, learnedValid = false;
    int learnWait = 0, learnCount = 0, learnWaitFrames = 1;
    std::vector<double> learnSum;
    std::vector<float> learned;
};

} // namespace vcab
