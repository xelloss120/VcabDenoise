#include "MinPhase.h"

#include "MathConstants.h"

#include <cmath>

namespace vcab
{

void MinPhaseDesigner::prepare (int size)
{
    n = size;
    fft.prepare (n);
    spec.assign (static_cast<size_t> (n / 2 + 1), {});
    cep.assign (static_cast<size_t> (n), 0.0);
}

void MinPhaseDesigner::design (const double* logMag, float* h, int taps)
{
    const int half = n / 2;
    // 大きさの対数（実数で左右対称）→ 実ケプストラム
    for (int k = 0; k <= half; ++k)
        spec[static_cast<size_t> (k)] = { logMag[k], 0.0 };
    fft.inverse (spec.data(), cep.data());

    // 折り返し: 正の側を 2 倍、負の側を 0（0 とナイキストはそのまま）
    for (int i = 1; i < half; ++i)
    {
        cep[static_cast<size_t> (i)] *= 2.0;
        cep[static_cast<size_t> (n - i)] = 0.0;
    }
    fft.forward (cep.data(), spec.data());
    // 結果は実数の波形なので、半分（0〜ナイキスト）だけ計算すればよい。
    // 複素数の指数は展開して書く（std::exp の複素数版は、無限大や NaN の扱いのために遅い）
    for (auto& c : spec)
    {
        const double m = std::exp (c.real());
        c = { m * std::cos (c.imag()), m * std::sin (c.imag()) };
    }
    fft.inverse (spec.data(), cep.data());

    const int taper = taps / 4;
    for (int i = 0; i < taps; ++i)
    {
        double w = 1.0;
        if (i >= taps - taper)
            w = 0.5 + 0.5 * std::cos (kPi * (i - (taps - taper) + 1) / (taper + 1));
        h[i] = static_cast<float> (cep[static_cast<size_t> (i)] * w);
    }
}

} // namespace vcab
