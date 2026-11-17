// 実数の FFT（高速フーリエ変換）。計算は pffft（単精度、SSE。https://github.com/marton78/pffft）に任せる。
// 前は倍精度・基数 2 の自作だったが、FFT がかかわる処理（FIR を作る、かける、分析）が全体の 4 割ほどを占めていたので替えた。
// 入出力は pffft と同じ単精度のまま受け渡す（2026-11-17。前は、使う側が倍精度の複素数で受け取り、変換のたびに詰め替えていた）。
// 大きさは 2 のべき乗で 32 以上（pffft の実数の FFT の条件）。準備（prepare）のときだけメモリを確保する。
//
// 周波数の側の並びは 2 通り:
//   順番どおり（forward / inverse）: [0Hz の実部, ナイキストの実部, 1 の実部, 1 の虚部, 2 の実部, 2 の虚部, …]（N 個）
//   pffft の中の並び（forwardRaw / inverseRaw / multiplyAdd）: 並べ替えを省いた形。中身は読めないが、
//     掛け算して足し込む（畳み込み）だけならこのまま計算でき、SIMD で速い
// FFT に渡す配列は、どれも AlignedFloats（SIMD 用にそろえた場所）にする。
#pragma once

#include <pffft.h>

#include <algorithm>
#include <cstddef>
#include <memory>

namespace vcab
{

// SIMD 用にそろえた場所に置く、単精度の配列（pffft に渡す配列はこれにする）
class AlignedFloats
{
public:
    void assign (size_t newSize)
    {
        data.reset (static_cast<float*> (pffft_aligned_malloc (newSize * sizeof (float))));
        count = newSize;
        clear();
    }
    void clear() noexcept { std::fill_n (data.get(), count, 0.0f); }
    float* get() noexcept { return data.get(); }
    const float* get() const noexcept { return data.get(); }
    size_t size() const noexcept { return count; }
    float& operator[] (size_t i) noexcept { return data.get()[i]; }
    float operator[] (size_t i) const noexcept { return data.get()[i]; }

private:
    struct Deleter { void operator() (float* p) const noexcept { pffft_aligned_free (p); } };
    std::unique_ptr<float, Deleter> data;
    size_t count = 0;
};

// 実数の FFT（大きさ N）。時間の側も周波数の側も N 個の単精度（AlignedFloats）。入力と出力に同じ配列を渡してもよい
class RealFft
{
public:
    void prepare (int newSize)
    {
        size = newSize;
        setup.reset (pffft_new_setup (size, PFFFT_REAL));
        work.assign (static_cast<size_t> (size));
    }

    // 時間 → 周波数（順番どおりの並び）
    void forward (const float* x, float* y) noexcept
    {
        pffft_transform_ordered (setup.get(), x, y, work.get(), PFFFT_FORWARD);
    }

    // 周波数（順番どおりの並び）→ 時間（1/N をかけて、forward と対になるようにする。N は 2 のべき乗なので、この掛け算で誤差は出ない）
    void inverse (const float* y, float* x) noexcept
    {
        pffft_transform_ordered (setup.get(), y, x, work.get(), PFFFT_BACKWARD);
        const float scale = 1.0f / static_cast<float> (size);
        for (int n = 0; n < size; ++n)
            x[n] *= scale;
    }

    // 時間 → 周波数（pffft の中の並び）
    void forwardRaw (const float* x, float* y) noexcept
    {
        pffft_transform (setup.get(), x, y, work.get(), PFFFT_FORWARD);
    }

    // 周波数（pffft の中の並び）→ 時間。1/N はかけない（multiplyAdd の scaling でかけておく）
    void inverseRaw (const float* y, float* x) noexcept
    {
        pffft_transform (setup.get(), y, x, work.get(), PFFFT_BACKWARD);
    }

    // acc += a × b × scaling（複素数の掛け算。どれも pffft の中の並び）
    void multiplyAdd (const float* a, const float* b, float* acc, float scaling) noexcept
    {
        pffft_zconvolve_accumulate (setup.get(), a, b, acc, scaling);
    }

private:
    struct SetupDeleter { void operator() (PFFFT_Setup* s) const noexcept { pffft_destroy_setup (s); } };

    int size = 0;
    std::unique_ptr<PFFFT_Setup, SetupDeleter> setup;
    AlignedFloats work;
};

} // namespace vcab
