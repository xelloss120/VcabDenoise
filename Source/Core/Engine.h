// 全体の処理。分析は FFT、音には最小位相の FIR をかける（音は FFT に通さない。遅延 0）。
//
//   入力 ─┬→ [長い FIR 4096] ─┬────→ [中くらいの FIR 2048] ──→ [短い FIR 512] → 出力
//         │      ↑ 狭帯域ノイズの谷   │            ↑ 500Hz より下          ↑ 500Hz より上
//         │                     ├→ [FFT 2048] ─┐
//         └→ [FFT 4096]         ├→ [FFT 512] ──┴→ ノイズ推定 → 下げる量
//            狭帯域ノイズを探す       └→ RNNoise の声の判定
//
// - 先に長い FIR で狭帯域ノイズを消し、消した後の音を短い側・中くらいの側で分析する（同じ場所を二重に削らないように）。
// - 中くらいの側と短い側は、受け持つ帯域を分ける（350〜700Hz で、下げる量 dB を周波数に応じた割合で分け合う）。
//   512 点の区画（約 94Hz）では、200Hz より下で、ファンの低い音と声の基本の周波数を分けられないため。
// - 分析と FIR は、同じ大きさで対にする（48kHz で 512 / 2048 / 4096。88.2・96kHz では 2 倍、176.4・192kHz では 4 倍）。
//   分析の間隔は大きさの 1/4、FIR を作り直す間隔とクロスフェードの長さも同じ。FIR の塊の長さは 128（同じ倍率）。
// - 抑制ノイズ試聴: 3 段を合わせた残す割合 G から、削った分の大きさ √(1 − G²) を特性にした FIR を作り、入力にかける
//   （入力 − 出力 にすると、最小位相の FIR の位相のずれの分が、削っていない声として聞こえてしまうため）。
// - バイパスと抑制ノイズ試聴の切り替えは、10ms でなめらかに移る。中の分析は、バイパスの間も続ける。
#pragma once

#include "Params.h"
#include "Dsp/FadingFir.h"
#include "Dsp/FrameAnalyzer.h"
#include "Dsp/GainRule.h"
#include "Dsp/MinPhase.h"
#include "Dsp/NoiseEstimator.h"
#include "Dsp/TonalTracker.h"
#include "Dsp/VoiceDetector.h"

#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

namespace vcab
{

// 画面に出す値（音の処理のスレッドが書き、画面のスレッドが読む）
struct EngineMeters
{
    std::atomic<float> inputDb { -120 }, outputDb { -120 }, reductionDb { 0 };
    std::atomic<bool> voice { false }, learning { false }, learned { false };
    std::atomic<int> notchCount { 0 };
    std::array<std::atomic<float>, TonalTracker::kMaxNotches> notchHz {}, notchDepthDb {}, notchWidthHz {};

    // ノイズ推定（白色雑音に直したときの 1 サンプルあたりの 2 乗を dB で）。20Hz〜20kHz の対数の等間隔
    static constexpr int kNoiseCurvePoints = 160;
    static float noiseCurveHz (int i) { return 20.0f * std::pow (1000.0f, static_cast<float> (i) / (kNoiseCurvePoints - 1)); }
    std::array<std::atomic<float>, kNoiseCurvePoints> noiseCurveDb {};
    std::atomic<bool> noiseCurveValid { false };
};

class Engine
{
public:
    Engine();

    void prepare (double sampleRate, int maxBlockSize);
    void process (float* data, int n, const ParamValues& p);
    void setRealtime (bool isRealtime) noexcept { vad.setRealtime (isRealtime); }

    // 画面から（どのスレッドからでもよい。音の処理のスレッドが次の塊で受け取る）
    void requestLearning (bool start) noexcept { learnRequest.store (start ? LearnRequest::start : LearnRequest::stop); }
    void requestClearLearned() noexcept { learnRequest.store (LearnRequest::clear); }

    // 学習した値の書き出しと読み込み（保存のため）。形式: 版（1）、点の数、（周波数 Hz、白色雑音に直した大きさ dB）× 点の数
    std::vector<float> exportLearned();
    void importLearned (std::vector<float> curve);

    EngineMeters meters;

private:
    enum class LearnRequest
    {
        none,
        start,
        stop,
        clear
    };

    struct Stage
    {
        FrameAnalyzer analyzer;
        NoiseEstimator noise;
        GainRule rule;
        MinPhaseDesigner designer;
        FadingFir fir;
        std::vector<float> gainDb, h; // gainDb: 区画ごとの残す割合（dB、0 以下）
        std::vector<double> logMag, weight; // weight: 設計の区画ごとの、この段が受け持つ割合（クロスオーバー）
        bool identity = true;
    };

    void prepareStage (Stage& st, int size, bool lowSide);
    void onBroadbandFrame (Stage& st, const ParamValues& p, bool useLearned);
    void onTonalFrame (const ParamValues& p);
    void updateMonitor();
    void updateNoiseCurve (bool useLearned);
    void handleRequests();
    void storeLearnedCurve();
    void applyLearnedCurve (const std::vector<float>& curve);
    double totalGainDb (double hz) const; // 3 段を合わせた下げる量（dB、0 以下）
    static double crossoverLow (double hz); // 中くらいの側が受け持つ割合（350Hz 以下で 1、700Hz 以上で 0）

    double sr = 48000;
    int scale = 1, blockSize = 128, phase = 0;
    Stage shortSide, midSide;

    // 狭帯域ノイズ
    FrameAnalyzer tonalAnalyzer;
    TonalTracker tonal;
    MinPhaseDesigner tonalDesigner;
    FadingFir tonalFir;
    std::vector<float> tonalH;
    std::vector<double> tonalLogMag, tonalFreqs;
    std::array<TonalTracker::Notch, TonalTracker::kMaxNotches> notchList {};
    int notchCount = 0;
    bool tonalIdentity = true;

    // 抑制ノイズ試聴
    MinPhaseDesigner monitorDesigner;
    FadingFir monitorFir;
    std::vector<float> monitorH;
    std::vector<double> monitorLogMag;
    bool monitorOn = false;

    VoiceDetector vad;
    int voicedQuiet = 2400, quietNow = 0;
    bool voiced = false;
    std::vector<float> chunkIn, chunkTonal;

    // 切り替えのなめらかさ
    float bypassMix = 0, noiseOnlyMix = 0, mixStep = 0.002f;
    // メーター
    double inPower = 0, outPower = 0, meterCoef = 0.001;

    // 学習
    std::atomic<LearnRequest> learnRequest { LearnRequest::none };
    bool wasLearning = false;
    std::mutex curveMutex;          // learnedCurve（音の処理のスレッドは try_lock だけ）
    std::vector<float> learnedCurve; // 最後に覚えた値（周波数の曲線）。サンプルレートが変わっても使えるように、この形で持つ
    std::vector<float> curveScratch; // 音の処理のスレッドで曲線を作る場所（learnedCurve と入れ替える。prepare で大きさを確保しておく）
    std::vector<float> pendingCurve;
    std::atomic<bool> hasPendingCurve { false }, curveDirty { false };
};

} // namespace vcab
