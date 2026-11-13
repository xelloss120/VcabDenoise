// 広い帯域の、区画ごとの残す割合（下げる量）を決める。短い側と中くらいの側で 1 つずつ使う。
//
// 順番:
//   1. log-MMSE（Ephraim・Malah 1985）: 対数振幅の平均二乗誤差を最小にする。声の SNR（前もっての SNR ξ）は判定指向型で、
//      直前のフレームの結果と今のフレームの結果の重み付きの平均で見積もる。
//   2. 音声存在確率（OM-LSA、Cohen 2001。常に行う。2026-11-12 までは入り切りできた）: 区画ごとの「声がある確率」p で、残す割合 G を
//      G^p × 下限^(1−p) にする。声がない確率は、ξ を時間でならし、近く・広く・フレーム全体の平均から決める（Cohen・Berdugo 2001）。
//   3. ならす: 時間（アタック・リリース。dB で、開けるのは速く閉じるのは遅く）。その後、周波数を ERB（聴覚の帯域幅）の尺度でならす
//      （高い周波数ほど広く）。ケプストラム平滑化（Breithaupt ら 2007）は、聞いて差が分からなかったので 2026-11-12 に外した。
//   帯域でまとめるとき（bands。短い側だけで使う）: 1〜3 の前に、区画の大きさとノイズ推定を、三角形を重ねた帯域
//      （中心の間隔は約 2 ERB、ただし 2 区画以上）でまとめ、1〜3 を帯域ごとに行う。
//      帯域ごとの結果は、隣の帯域の中心の間を直線でつないで区画に戻し、2 ERB でならす（帯域の中心の折れ目が粒になるのを和らげる）。
//      RNNoise（Valin 2017）と同じ考え方で、推定のばらつきを、非線形の計算の前に減らす（粒と上下を減らす）。
//      区画ごとの計算（1〜3 と ERB のならし）も行い、残す割合の対数を半分ずつ混ぜる（帯域の粗さによる、倍音の間のノイズの
//      開け閉め（うねり）を減らす）。2 つの計算は、前のフレームから引き継ぐ状態を別々に持つ。混ぜた後に 4 を行う。
//      割合とならしの幅は、2026-11-10 まで調整項目だったもの（11-12 に固定）。
//   4. 立ち上がり保護（入り切りできる）: 区画の大きさが、ノイズより 10dB 以上大きく、ゆっくりの大きさ（40ms）より
//      3dB 以上急に大きくなったら、30ms の間その区画を 5ms で開ける（開ける量は SNR +9〜+15dB でなだらかに、その後 40ms で戻す。
//      判定は VoiChanAlgoBox の帯域分割の「立ち上がりの保護」と同じ値）。
// 係数（判定指向型の 0.98、Cohen の 0.7）は、論文の分析の間隔での値を、今の間隔に換算して使う。
// 計算は、残す割合の自然対数のまま進める（対数と指数の計算を、区画ごとに数回に減らすため）。結果は dB で返す。
// 1〜3 は VoiChanAlgoBox の SpectralGain と GainSmoother から、使う部分だけを移したもの。
#pragma once

#include <vector>

namespace vcab
{

struct GainSettings
{
    double maxReductionDb = 24;
    double strengthDb = 6;
    double attackMs = 10, releaseMs = 100;
    bool onsetProtect = true;
    bool bands = false; // 帯域でまとめる（短い側だけ。途中で切り替えない）
};

class GainRule
{
public:
    void prepare (int fftSize, double sampleRate, int hopSize);
    // power と noise（区画ごとの 2 乗）から、gainDb（残す割合の dB。−抑制量上限〜0）を出す
    void compute (const float* power, const float* noise, const GainSettings& s, float* gainDb);
    void reset(); // ならしなどの、前のフレームから引き継ぐ状態を消す

private:
    // 前のフレームから引き継ぐ状態（区画ごとの計算と、帯域ごとの計算で別々に持つ）
    struct Track
    {
        std::vector<float> prevClean, xiNow, vNow, zeta, prevLn;
        bool first = true;
        void reset();
    };

    // lg: 残す割合の自然対数（区画ごと。帯域でまとめるときは、先頭の n 個が帯域ごと）
    void logMmse (Track& t, const float* power, const float* noise, const GainSettings& s, int n);
    void applyPresence (Track& t, double lnFloor, int n, int localHalf, int globalHalf);
    void smoothTime (Track& t, const GainSettings& s, int n);
    void computeBins (const float* power, const float* noise, const GainSettings& s, double lnFloor);
    void computeBands (const float* power, const float* noise, const GainSettings& s, double lnFloor);
    void boxcar (const std::vector<int>& lo, const std::vector<int>& hi); // lg を、区画ごとの範囲 lo〜hi で平均する
    void makeErbRanges (double widthErb, std::vector<int>& lo, std::vector<int>& hi) const; // boxcar の範囲（幅 widthErb 個分の ERB）
    void protectOnset (const float* power, const float* noise);

    int size = 512, bins = 257;
    double hopSeconds = 0.0027, binHz = 93.75;
    double ddAlpha = 0.98, zetaAlpha = 0.7;
    Track binTrack, bandTrack;
    std::vector<double> lg, work, mixWork;
    // ERB でならす範囲（区画ごとの、前後の端）
    std::vector<int> erbLo, erbHi;
    std::vector<double> logWork;
    // 帯域でまとめるとき: 区画 k は、帯域 bandLo[k] と bandLo[k] + 1 の中心の間にあり、重みは 1 − bandT[k] と bandT[k]
    int numBands = 0;
    std::vector<int> bandLo;
    std::vector<float> bandT, bandPower, bandNoise;
    std::vector<double> bandLg;
    std::vector<int> bandSmoothLo, bandSmoothHi; // 帯域のならしの範囲
    // 立ち上がり保護
    std::vector<float> slowPower, opened;
    std::vector<int> holdLeft;
    float slowCoef = 0.1f, openAttack = 0.1f, openRelease = 0.1f;
    int holdFrames = 10;
};

} // namespace vcab
