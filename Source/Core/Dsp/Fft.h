// 実数の FFT（高速フーリエ変換）。計算は pffft（単精度、SSE。https://github.com/marton78/pffft）に任せる。
// 前は倍精度・基数 2 の自作だったが、FFT がかかわる処理（FIR を作る、かける、分析）が全体の 4 割ほどを占めていたので替えた。
// 入出力は前と同じ形（実数の時間の波形と、0Hz〜ナイキストの倍精度の複素数）にして、使う側は変えずに済むようにしている。
// 大きさは 2 のべき乗で 32 以上（pffft の実数の FFT の条件）。準備（prepare）のときだけメモリを確保する。
#pragma once

#include <pffft.h>

#include <complex>
#include <memory>

namespace vcab
{

namespace Fft
{
using Complex = std::complex<double>;
}

// 実数の FFT（大きさ N）。周波数の側は 0〜N/2 の N/2 + 1 個（残りは複素共役で決まる）
class RealFft
{
public:
    using Complex = Fft::Complex;

    void prepare (int newSize)
    {
        size = newSize;
        setup.reset (pffft_new_setup (size, PFFFT_REAL));
        const auto bytes = static_cast<size_t> (size) * sizeof (float);
        for (auto* b : { &in, &out, &work })
            b->reset (static_cast<float*> (pffft_aligned_malloc (bytes)));
    }

    // 時間（実数 N 個）→ 周波数（N/2 + 1 個）
    template <typename Real>
    void forward (const Real* x, Complex* y) noexcept
    {
        float* a = in.get();
        for (int n = 0; n < size; ++n)
            a[n] = static_cast<float> (x[n]);
        pffft_transform_ordered (setup.get(), a, out.get(), work.get(), PFFFT_FORWARD);
        // pffft の並び: [0Hz, ナイキスト, 1 の実部, 1 の虚部, 2 の実部, …]
        const float* b = out.get();
        const int m = size / 2;
        y[0] = { b[0], 0.0 };
        y[m] = { b[1], 0.0 };
        for (int k = 1; k < m; ++k)
            y[k] = { b[2 * k], b[2 * k + 1] };
    }

    // 周波数（N/2 + 1 個）→ 時間（実数 N 個。1/N をかけて、forward と対になるようにする）
    template <typename Real>
    void inverse (const Complex* y, Real* x) noexcept
    {
        float* a = in.get();
        const int m = size / 2;
        a[0] = static_cast<float> (y[0].real());
        a[1] = static_cast<float> (y[m].real());
        for (int k = 1; k < m; ++k)
        {
            a[2 * k] = static_cast<float> (y[k].real());
            a[2 * k + 1] = static_cast<float> (y[k].imag());
        }
        pffft_transform_ordered (setup.get(), a, out.get(), work.get(), PFFFT_BACKWARD);
        const float* b = out.get();
        const double scale = 1.0 / size;
        for (int n = 0; n < size; ++n)
            x[n] = static_cast<Real> (b[n] * scale);
    }

private:
    struct SetupDeleter { void operator() (PFFFT_Setup* s) const noexcept { pffft_destroy_setup (s); } };
    struct BufferDeleter { void operator() (float* p) const noexcept { pffft_aligned_free (p); } };

    int size = 0;
    std::unique_ptr<PFFFT_Setup, SetupDeleter> setup;
    std::unique_ptr<float, BufferDeleter> in, out, work;
};

} // namespace vcab
