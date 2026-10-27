// 広い帯域の、区画ごとの残す割合（下げる量）を決める。短い側と中くらいの側で 1 つずつ使う。
//
// 順番:
//   1. log-MMSE（Ephraim・Malah 1985）: 対数振幅の平均二乗誤差を最小にする。声の SNR（前もっての SNR ξ）は判定指向型で、
//      直前のフレームの結果と今のフレームの結果の重み付きの平均で見積もる。
//   2. 音声存在確率（OM-LSA、Cohen 2001。入り切りできる）: 区画ごとの「声がある確率」p で、残す割合 G を
//      G^p × 下限^(1−p) にする。声がない確率は、ξ を時間でならし、近く・広く・フレーム全体の平均から決める（Cohen・Berdugo 2001）。
//   3. ならす: 時間（アタック・リリース。dB で、開けるのは速く閉じるのは遅く）、またはケプストラム（Breithaupt ら 2007。
//      入り切りできる。切ったときは時間）。どちらの後も、周波数を ERB（聴覚の帯域幅）の尺度でならす（高い周波数ほど広く）。
//   4. 立ち上がり保護（入り切りできる）: 区画の大きさが、ノイズより 10dB 以上大きく、ゆっくりの大きさ（40ms）より
//      3dB 以上急に大きくなったら、30ms の間その区画を開ける（開ける量は SNR +9〜+15dB でなだらかに、その後 20ms で戻す。
//      VoiChanAlgoBox の帯域分割の「立ち上がりの保護」と同じ値）。
// 係数（判定指向型の 0.98、Cohen の 0.7、ケプストラムの強さ）は、論文の分析の間隔での値を、今の間隔に換算して使う。
// 計算は、残す割合の自然対数のまま進める（対数と指数の計算を、区画ごとに数回に減らすため）。結果は dB で返す。
// 1〜3 は VoiChanAlgoBox の SpectralGain と GainSmoother から、使う部分だけを移したもの。
#pragma once

#include "Fft.h"

#include <vector>

namespace vcab
{

struct GainSettings
{
    double maxReductionDb = 24;
    double strengthDb = 6;
    double attackMs = 10, releaseMs = 100;
    bool presence = false;
    bool cepstrum = false;
    bool onsetProtect = true;
};

class GainRule
{
public:
    void prepare (int fftSize, double sampleRate, int hopSize);
    // power と noise（区画ごとの 2 乗）から、gainDb（残す割合の dB。−抑制量上限〜0）を出す
    void compute (const float* power, const float* noise, const GainSettings& s, float* gainDb);

private:
    void reset();
    // lg: 残す割合の自然対数（区画ごと）
    void logMmse (const float* power, const float* noise, const GainSettings& s);
    void applyPresence (double lnFloor);
    void smoothTime (const GainSettings& s);
    void smoothCepstrum();
    void smoothErb();
    void protectOnset (const float* power, const float* noise);

    int size = 512, bins = 257;
    double sr = 48000, hopSeconds = 0.0027, binHz = 93.75;
    double ddAlpha = 0.98, zetaAlpha = 0.7;
    bool first = true;
    std::vector<float> prevClean, xiNow, vNow, zeta, prevLn;
    std::vector<double> lg, work;
    // ケプストラム
    RealFft fft;
    std::vector<Fft::Complex> spec;
    std::vector<double> cep, cepSmooth, beta;
    // ERB でならす範囲（区画ごとの、前後の端）
    std::vector<int> erbLo, erbHi;
    std::vector<double> logWork;
    // 立ち上がり保護
    std::vector<float> slowPower, opened;
    std::vector<int> holdLeft;
    float slowCoef = 0.1f, openRelease = 0.1f;
    int holdFrames = 10;
};

} // namespace vcab
