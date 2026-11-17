#include "MinPhase.h"

#include "MathConstants.h"

#include <cmath>

namespace vcab
{

void MinPhaseDesigner::prepare (int size)
{
    n = size;
    fft.prepare (n);
    spec.assign (static_cast<size_t> (n));
    cep.assign (static_cast<size_t> (n));
}

void MinPhaseDesigner::design (const double* logMag, float* h, int taps)
{
    const int half = n / 2;
    // 大きさの対数（実数で左右対称）→ 実ケプストラム
    // 周波数の側の並びは [0Hz, ナイキスト, 1 の実部, 1 の虚部, …]（Fft.h）
    spec[0] = static_cast<float> (logMag[0]);
    spec[1] = static_cast<float> (logMag[half]);
    for (int k = 1; k < half; ++k)
    {
        spec[static_cast<size_t> (2 * k)] = static_cast<float> (logMag[k]);
        spec[static_cast<size_t> (2 * k + 1)] = 0.0f;
    }
    fft.inverse (spec.get(), cep.get());

    // 折り返し: 正の側を 2 倍、負の側を 0（0 とナイキストはそのまま）
    for (int i = 1; i < half; ++i)
    {
        cep[static_cast<size_t> (i)] *= 2.0f;
        cep[static_cast<size_t> (n - i)] = 0.0f;
    }
    fft.forward (cep.get(), spec.get());
    // 結果は実数の波形なので、半分（0〜ナイキスト）だけ計算すればよい。
    // 複素数の指数は展開して書く（std::exp の複素数版は、無限大や NaN の扱いのために遅い）。指数と三角関数は倍精度で計算する
    spec[0] = static_cast<float> (std::exp (static_cast<double> (spec[0])));
    spec[1] = static_cast<float> (std::exp (static_cast<double> (spec[1])));
    for (int k = 1; k < half; ++k)
    {
        auto& re = spec[static_cast<size_t> (2 * k)];
        auto& im = spec[static_cast<size_t> (2 * k + 1)];
        const double m = std::exp (static_cast<double> (re));
        const double angle = im;
        re = static_cast<float> (m * std::cos (angle));
        im = static_cast<float> (m * std::sin (angle));
    }
    fft.inverse (spec.get(), cep.get());

    const int taper = taps / 4;
    for (int i = 0; i < taps; ++i)
    {
        double w = 1.0;
        if (i >= taps - taper)
            w = 0.5 + 0.5 * std::cos (kPi * (i - (taps - taper) + 1) / (taper + 1));
        h[i] = static_cast<float> (static_cast<double> (cep[static_cast<size_t> (i)]) * w);
    }
}

} // namespace vcab
