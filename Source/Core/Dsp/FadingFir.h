// 遅延 0 の FIR（分割して FFT で畳み込む）と、フィルタを替えるときのクロスフェード。
//
// 分け方（一様な分割。先頭だけそのまま計算する）:
//   FIR h（長さ L）を、長さ B ずつに分ける。先頭の B 個（頭）は、1 サンプルごとにそのまま畳み込む（遅延 0）。
//   残り（尾。分割 j = 1〜P）は、入力を B 個ずつの塊にして FFT（大きさ 2B）でまとめて畳み込む。
//   塊 k を分割 j にかけた結果は、時刻 (k + j)B から出始めるので、j ≥ 1 なら塊がそろった後に計算しても間に合う。
//   塊 k が終わった時点で、次の塊の間に出す尾の分 = IFFT(Σ_j X_{k+1−j} G_j) の前半 ＋ 前回の後半（重ね合わせ）。
//   （X は入力の塊の FFT、G_j は分割 j の FFT）
// クロスフェード:
//   フィルタは 2 つ（今と前）を持ち、替えた後 fadeLength サンプルの間は両方を計算して、出力を直線で混ぜる
//   （出力を直線で混ぜるのは、係数を 1 サンプルごとに直線で補間するのと同じ）。フェードの途中で次のフィルタが来たら、
//   フェードが終わるまで待ってからかける（途中で切り替えると段差ができるため）。
//   新しいフィルタの尾は、過去の入力の FFT（X）から、その場で計算し直す（過去の入力に新しいフィルタをかけた結果から始める）。
// 素通し（h = δ）のフィルタは、畳み込みを省く（狭帯域ノイズがないときの長い FIR など）。
#pragma once

#include "Fft.h"

#include <vector>

namespace vcab
{

class FadingFir
{
public:
    // taps: FIR の長さ（block の倍数にする）。block: 塊の長さ（2 のべき乗）。fade: クロスフェードの長さ（サンプル）
    void prepare (int taps, int block, int fade);
    void reset(); // 素通しに戻す
    // 新しいフィルタ（長さ taps）。フェードの途中なら、終わってからかける
    void setFilter (const float* h);
    void setIdentity();
    float process (float x) noexcept;

private:
    struct Slot
    {
        std::vector<float> head;                       // 先頭の block 個
        std::vector<std::vector<Fft::Complex>> tail;   // 分割ごとの FFT（大きさ 2 × block の実数の FFT。block + 1 個）
        std::vector<float> outCur, overlap;            // 今の塊で出す尾の分と、次の塊へ持ち越す後半
        bool identity = true;
        bool active = false;
    };

    void load (int slotIndex, const float* h);
    void endBlock();
    void tailSum (const Slot& s, int newest, std::vector<double>& out); // Σ_j X_{newest+1−j} G_j を IFFT した結果（2 × block 個）
    float headAt (const Slot& s) const noexcept;

    int length = 0, block = 128, parts = 0, fadeLength = 128;
    int pos = 0, histPos = 0, fdlNewest = 0, fadeLeft = 0, cur = 0;
    RealFft fft;
    std::vector<std::vector<Fft::Complex>> fdl; // 入力の塊の FFT（parts + 1 個分のリング。fdlNewest が一番新しい）
    std::vector<float> hist;                    // 頭の畳み込み用の直近の入力（2 倍の長さで、折り返さずに読めるように）
    std::vector<float> blockIn;
    std::vector<Fft::Complex> acc;
    std::vector<double> pad, sumNow, sumPrev;
    Slot slots[2];
    std::vector<float> pending;
    bool hasPending = false;
};

} // namespace vcab
