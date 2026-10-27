// 残す割合（大きさ）から、最小位相の FIR を作る（ケプストラムを折り返す方法）。
// 作り方は VoiChanAlgoBox の LowLatencyDenoiser::designFilter と同じ:
//   1. 大きさの対数を、逆 FFT で実ケプストラムにする
//   2. 正の側を 2 倍、負の側を 0 にする（折り返し）。こうすると、大きさが同じで位相が最小になる
//   3. FFT して指数をとり、逆 FFT で時間の波形に戻す
//   4. 先頭から taps 個を取り、最後の 1/4 をなめらかに 0 へ落とす（打ち切りの段差で、特性が波打たないように）
// 最小位相なので、エネルギーは先頭に集まり、決まった遅延はない。
#pragma once

#include "Fft.h"

#include <vector>

namespace vcab
{

class MinPhaseDesigner
{
public:
    // size: 計算に使う FFT の大きさ（作る FIR の長さの 2 倍以上）
    void prepare (int size);
    int size() const noexcept { return n; }
    // logMag: 0Hz〜ナイキストの size / 2 + 1 個の、大きさの自然対数。h に taps 個を書く
    void design (const double* logMag, float* h, int taps);

private:
    int n = 0;
    RealFft fft;
    std::vector<Fft::Complex> spec;
    std::vector<double> cep;
};

} // namespace vcab
