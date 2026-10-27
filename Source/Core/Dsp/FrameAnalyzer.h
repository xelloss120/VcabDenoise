// 分析だけの FFT（音は通さない）。直近の size サンプルに窓をかけて、hop サンプルごとに区画ごとの大きさ（2 乗）を出す。
#pragma once

#include "Fft.h"
#include "MathConstants.h"

#include <cmath>
#include <vector>

namespace vcab
{

class FrameAnalyzer
{
public:
    enum class Window
    {
        hann,      // 左右対称（狭帯域ノイズの周波数を正確に求めるため）
        recentHeavy // 今に近い側を重くした非対称の形（3/4 で上がって 1/4 で下がる。声の出だしへの反応が早い。VoiChanAlgoBox の FFT＋FIR と同じ）
    };

    void prepare (int fftSize, int hopSize, Window kind)
    {
        size = fftSize;
        hop = hopSize;
        fft.prepare (size);
        frame.assign (static_cast<size_t> (size), 0.0);
        window.assign (static_cast<size_t> (size), 0.0);
        if (kind == Window::hann)
        {
            for (int i = 0; i < size; ++i)
                window[static_cast<size_t> (i)] = 0.5 - 0.5 * std::cos (2 * kPi * i / size);
        }
        else
        {
            const int rise = size * 3 / 4, fall = size - rise;
            for (int i = 0; i < size; ++i)
                window[static_cast<size_t> (i)] = i < rise ? 0.5 - 0.5 * std::cos (kPi * i / rise)
                                                           : 0.5 + 0.5 * std::cos (kPi * (i - rise) / fall);
        }
        windowEnergy = 0;
        windowSum = 0;
        for (double w : window)
        {
            windowEnergy += w * w;
            windowSum += w;
        }
        ring.assign (static_cast<size_t> (size), 0.0f);
        spec.assign (static_cast<size_t> (size / 2 + 1), {});
        power.assign (static_cast<size_t> (size / 2 + 1), 0.0f);
        reset();
    }

    void reset()
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        pos = 0;
        sinceHop = firstHopHead;
    }

    // 最初の分析を、ホップの長さより samples 早める（reset の後も同じ）。段ごとに分析の時期をずらし、重い処理が同じ塊に重ならないようにする
    void setFirstHopEarlier (int samples)
    {
        firstHopHead = samples;
        reset();
    }

    // 1 サンプル入れる。分析した（power が新しくなった）ら true
    bool push (float x)
    {
        ring[static_cast<size_t> (pos)] = x;
        pos = (pos + 1) % size;
        if (++sinceHop < hop)
            return false;
        sinceHop = 0;
        // リングを古い順に並べ直しながら窓をかける（pos が一番古い）
        const int first = size - pos;
        for (int i = 0; i < first; ++i)
            frame[static_cast<size_t> (i)] = ring[static_cast<size_t> (pos + i)] * window[static_cast<size_t> (i)];
        for (int i = first; i < size; ++i)
            frame[static_cast<size_t> (i)] = ring[static_cast<size_t> (i - first)] * window[static_cast<size_t> (i)];
        fft.forward (frame.data(), spec.data());
        for (int k = 0; k <= size / 2; ++k)
            power[static_cast<size_t> (k)] = static_cast<float> (std::norm (spec[static_cast<size_t> (k)]));
        return true;
    }

    int fftSize() const noexcept { return size; }
    int bins() const noexcept { return size / 2 + 1; }
    const float* powers() const noexcept { return power.data(); }
    // 区画の大きさ（2 乗）を、白色雑音の 1 サンプルあたりの分散に直す倍率（分散 σ² の白色雑音は、区画ごとに σ² × Σw² になる）
    double toDensity() const noexcept { return 1.0 / windowEnergy; }
    // 正弦波の振幅に直す倍率（振幅 A の正弦波は、区画の大きさが A × Σw / 2 になる）
    double toAmplitude() const noexcept { return 2.0 / windowSum; }

private:
    int size = 512, hop = 128, pos = 0, sinceHop = 0, firstHopHead = 0;
    RealFft fft;
    std::vector<double> window, frame;
    double windowEnergy = 1, windowSum = 1;
    std::vector<float> ring, power;
    std::vector<Fft::Complex> spec;
};

} // namespace vcab
