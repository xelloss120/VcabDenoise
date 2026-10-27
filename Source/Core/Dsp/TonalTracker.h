// 狭帯域の持続音（「ふぃーん」「キーン」、低い機械の音など）を見つけて追いかける。長い側（4096 点の分析）。
// VoiChanAlgoBox のノッチの自動（AdaptiveNotch）を土台に、次を変えたもの:
//   - 探す範囲を 20Hz から（PC のファンなどの低い音のため）。パラメータは全部つまみ
//   - 削る量は、山が周りより突き出た分だけ（周りの高さにそろえる。上限は「深さ上限」）
//   - 声の判定（RNNoise）が「声あり」の間は、新しい山を拾わず、削る深さも固定する（周波数の追従だけ続ける）。
//     声の倍音が狭帯域ノイズに重なると、突き出た量が大きく見え、声の倍音まで深く削ってしまうため
//   - 周波数が飛んだとき: 削っている山が消えてから「跳躍判定時間」のうちに、「跳躍判定範囲」に新しい山が出たら、
//     同じ音が飛んだとみなし、持続判定時間を待たずにすぐ削る（古い位置は、解除時間をかけて戻す）
// 見つけ方（VoiChanAlgoBox と同じ）:
//   1. 区画ごとの大きさ（正弦波の振幅の dBFS）で、周り（前後「周囲比較幅」、山のすぐ隣の 3 区画は除く）の平均より
//      「感度」以上突き出た極大を探す。周波数は 3 点の放物線で区画の間まで求める
//   2. 等間隔に並んだ山（基音 70〜500Hz で 4 本以上）は、声の倍音とみなして対象外にする（RNNoise が見逃す歌声など）。
//      ただし、電源の 50 / 60Hz のちょうど倍数（2kHz まで）でない山が 2 本以上ある列だけ。ハムの列（100, 200, 300, 400Hz …）が
//      「基音 100Hz の声」に見えて、抑制しなくなるため
//   3. 「持続判定時間」以上居続けた山を、突き出た量の大きい順に「本数」まで削る。0.2 秒で深くし、消えたら「解除時間」で戻す
#pragma once

#include <array>
#include <vector>

namespace vcab
{

struct TonalSettings
{
    double sensitivityDb = 12;
    int count = 4;
    double maxDepthDb = 24;
    double lowHz = 20, highHz = 16000, neighborHz = 190;
    double activateSec = 1, releaseSec = 2;
    double jumpSec = 0.5, jumpRange = 0.3;
    double width2k = 70, lowWidth = 20;
    double quietDb = -96;
};

class TonalTracker
{
public:
    static constexpr int kMaxNotches = 8;  // 同時に削る谷の数の上限（本数上限の最大）
    static constexpr int kMaxTracks = 16;  // 同時に追いかける山の数の上限（削る前の候補を含む）
    struct Notch
    {
        float freq, depthDb, widthHz;
    };

    void prepare (double sampleRate, int fftSize, double hopSeconds);
    void reset();
    // 1 フレーム分の区画ごとの大きさ（2 乗）と、正弦波の振幅に直す倍率。voiced: 声の判定が「声あり」
    void analyze (const float* power, double toAmplitude, bool voiced, const TonalSettings& s);
    // 今削っている谷（効きを掛けた深さ）。戻り値は個数
    int notches (Notch* out, const TonalSettings& s) const;
    // 谷の形（dB、0 以下）を、freqs の各周波数で足し込む
    static void addShape (const Notch* n, int count, const double* freqs, int points, double* db);

private:
    struct Track
    {
        double freq = 0, prominence = 0;
        double presentSec = 0, missingSec = 0;
        double fade = 0;       // 効き（0〜1）
        bool active = false;   // 削る対象に選ばれている
        bool used = false;
        bool jumpedFrom = false; // この山から、飛んだ先の山を作った（同じ山から何度も作らない）
    };
    struct Candidate
    {
        double freq, prominence;
        bool harmonic;
        bool mains; // 電源の 50 / 60Hz のちょうど倍数
    };

    void updateTracks (bool voiced, const TonalSettings& s);
    double widthFor (double freq, const TonalSettings& s) const;

    double sr = 48000, hopSeconds = 0.02, binHz = 11.7;
    int size = 4096;
    std::vector<double> magDb, prefix;
    std::vector<Candidate> candidates;
    std::array<Track, kMaxTracks> tracks {};
};

} // namespace vcab
