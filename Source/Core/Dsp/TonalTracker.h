// 狭帯域の持続音（「ふぃーん」「キーン」、低い機械の音など）を見つけて追いかける。長い側（4096 点の分析）。
// VoiChanAlgoBox のノッチの自動（AdaptiveNotch）を土台に、次を変えたもの:
//   - 探す範囲を 20Hz から（PC のファンなどの低い音のため）
//   - 削る量は、山が周りより突き出た分だけ（周りの高さにそろえる。上限は「深さ上限」）
//   - 声の判定（RNNoise）が「声あり」の間は、新しい山を拾わず、削る深さも固定する（周波数の追従だけ続ける）。
//     声の倍音が狭帯域ノイズに重なると、突き出た量が大きく見え、声の倍音まで深く削ってしまうため
//   - 周波数が飛んだとき: 削っている山が消えて 0.5 秒のうちに、その ±30% に新しい山が出たら、同じ音が飛んだとみなし、
//     持続判定時間を待たずにすぐ削る。元の山は、声のない間にまた見えるまで、声がある間は追いかけず、削る山に選ばず、0.2 秒で外す
//     （声の倍音に当てはまって残り続けないように）
// 見つけ方（VoiChanAlgoBox と同じ）:
//   1. 区画ごとの大きさ（正弦波の振幅の dBFS）で、周り（前後 190Hz、山のすぐ隣の 3 区画は除く）の平均より
//      「感度」以上突き出た極大を探す。周波数は 3 点の放物線で区画の間まで求める
//   2. 等間隔に並んだ山（基音 70〜500Hz で 4 本以上）は、声の倍音とみなして対象外にする（RNNoise が見逃す歌声など）。
//      ただし、電源の 50 / 60Hz のちょうど倍数（2kHz まで）でない山が 2 本以上ある列だけ。ハムの列（100, 200, 300, 400Hz …）が
//      「基音 100Hz の声」に見えて、抑制しなくなるため
//   3. 「持続判定時間」以上居続け、声のない間の約 0.5 秒で 7 割以上見えている山を、突き出た量の大きい順に「本数上限」まで削る。
//      0.2 秒で深くし、消えたら 2 秒で戻す
#pragma once

#include <array>
#include <vector>

namespace vcab
{

// 調整項目（ほかの値は TonalTracker.cpp の定数）
struct TonalSettings
{
    double sensitivityDb = 12; // 感度
    int count = 4;             // 本数上限
    double maxDepthDb = 24;    // 深さ上限
    double activateSec = 1;    // 持続判定時間
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
        double hitRate = 0;    // 見えている割合（声が出ていないフレームの移動平均）
        bool confirmed = false; // 持続音とみなした（一度みなしたら、消えて忘れるまで続ける）
        bool jumpedFrom = false; // この山から、飛んだ先の山を作った（同じ山から何度も作らない）
        bool left = false; // 飛んだ後、まだ元の位置に見えていない（声がある間は追いかけず、削る山に選ばず、速く外す）
    };
    struct Candidate
    {
        double freq, prominence;
        bool harmonic;
        bool mains; // 電源の 50 / 60Hz のちょうど倍数
    };

    void updateTracks (bool voiced, const TonalSettings& s);
    double widthFor (double freq) const;

    double sr = 48000, hopSeconds = 0.02, binHz = 11.7;
    int size = 4096;
    std::vector<double> magDb, prefix;
    std::vector<Candidate> candidates;
    std::array<Track, kMaxTracks> tracks {};
};

} // namespace vcab
