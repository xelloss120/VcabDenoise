#include "GainRule.h"

#include "MathConstants.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vcab
{

namespace
{
// 級数の係数 (-1)^(k+1) / (k・k!)（k = 1〜16）。x < 1 では、17 項目からの和は 1e-16 より小さい
constexpr int kE1Terms = 16;
constexpr std::array<double, kE1Terms> e1Coefficients()
{
    std::array<double, kE1Terms> c {};
    double factorial = 1;
    for (int k = 1; k <= kE1Terms; ++k)
    {
        factorial *= k;
        c[static_cast<size_t> (k - 1)] = (k % 2 == 1 ? 1.0 : -1.0) / (k * factorial);
    }
    return c;
}
constexpr auto kE1Coef = e1Coefficients();

// 指数積分 E1(x)（log-MMSE に使う）。式は Abramowitz と Stegun の「Handbook of Mathematical Functions」から
double expIntE1 (double x)
{
    x = std::max (x, 1.0e-10);
    if (x < 1.0)
    {
        // 級数（5.1.11）: E1(x) = -γ - ln x - Σ (-x)^k / (k・k!)。係数を前もって計算し、ホーナー法で求める（割り算をなくす。
        // ノイズが勝っている区画はほとんどここを通るので、下げる量の計算で一番重いところだった）
        double sum = 0;
        for (int k = kE1Terms - 1; k >= 0; --k)
            sum = sum * x + kE1Coef[static_cast<size_t> (k)];
        return -0.5772156649015329 - std::log (x) + sum * x;
    }
    // 有理式の近似（5.1.56）: x e^x E1(x) ≒ P(x) / Q(x)。1 以上で誤差 2e-8 未満
    const double p = (((x + 8.5733287401) * x + 18.0590169730) * x + 8.6347608925) * x + 0.2677737343;
    const double q = (((x + 9.5733223454) * x + 25.6329561486) * x + 21.0996530827) * x + 3.9584969228;
    return p / (q * x) * std::exp (-x);
}

double erbHz (double f) { return 24.7 * (4.37 * f / 1000.0 + 1.0); } // Glasberg・Moore 1990

} // namespace

void GainRule::prepare (int fftSize, double sampleRate, int hopSize)
{
    size = fftSize;
    bins = size / 2 + 1;
    sr = sampleRate;
    hopSeconds = hopSize / sampleRate;
    binHz = sampleRate / size;
    // 判定指向型の重み。もとの論文の 0.98 を、区切りの間隔 8ms のときの値とみなして、間隔に合わせる（VoiChanAlgoBox と同じ）
    ddAlpha = std::pow (0.98, hopSeconds / 0.008);
    // Cohen の声がない確率: ξ をならす重み（論文の 0.7 を、間隔 16ms のときの値とみなす）
    zetaAlpha = std::pow (0.7, hopSeconds / 0.016);
    const auto b = static_cast<size_t> (bins);
    prevClean.assign (b, 0.0f);
    xiNow.assign (b, 0.0f);
    vNow.assign (b, 0.0f);
    zeta.assign (b, 0.0f);
    prevLn.assign (b, 0.0f);
    lg.assign (b, 0.0);
    work.assign (b + 1, 0.0);
    fft.prepare (size);
    spec.assign (b, {});
    cep.assign (static_cast<size_t> (size), 0.0);
    cepSmooth.assign (static_cast<size_t> (size / 2 + 1), 0.0);
    beta.assign (static_cast<size_t> (size / 2 + 1), 0.0);
    // ERB でならす範囲: 中心の前後に、ERB の半分ずつ
    erbLo.assign (b, 0);
    erbHi.assign (b, 0);
    for (int k = 0; k < bins; ++k)
    {
        const double halfWidth = 0.5 * erbHz (k * binHz) / binHz;
        erbLo[static_cast<size_t> (k)] = std::max (0, static_cast<int> (std::floor (k - halfWidth + 0.5)));
        erbHi[static_cast<size_t> (k)] = std::min (bins - 1, static_cast<int> (std::floor (k + halfWidth + 0.5)));
    }
    logWork.assign (b + 1, 0.0);
    // 立ち上がり保護の時間（フレームの数に直す）
    const auto coef = [this] (double seconds) { return static_cast<float> (1.0 - std::exp (-hopSeconds / seconds)); };
    slowCoef = coef (0.040);
    openRelease = coef (0.020);
    holdFrames = std::max (1, static_cast<int> (std::lround (0.030 / hopSeconds)));
    slowPower.assign (b, 0.0f);
    opened.assign (b, 0.0f);
    holdLeft.assign (b, 0);
    reset();
}

void GainRule::reset()
{
    std::fill (prevClean.begin(), prevClean.end(), 0.0f);
    std::fill (zeta.begin(), zeta.end(), 0.0f);
    std::fill (prevLn.begin(), prevLn.end(), 0.0f);
    std::fill (cepSmooth.begin(), cepSmooth.end(), 0.0);
    std::fill (beta.begin(), beta.end(), 0.0);
    std::fill (slowPower.begin(), slowPower.end(), 0.0f);
    std::fill (opened.begin(), opened.end(), 0.0f);
    std::fill (holdLeft.begin(), holdLeft.end(), 0);
    first = true;
}

void GainRule::compute (const float* power, const float* noise, const GainSettings& s, float* gainDb)
{
    const double lnFloor = -s.maxReductionDb / kLnToDb;
    logMmse (power, noise, s);
    for (auto& v : lg)
        v = std::max (v, lnFloor);
    if (s.presence)
        applyPresence (lnFloor);
    if (s.cepstrum)
        smoothCepstrum();
    else
        smoothTime (s);
    smoothErb();
    if (s.onsetProtect)
        protectOnset (power, noise);
    for (int k = 0; k < bins; ++k)
        gainDb[k] = static_cast<float> (std::clamp (lg[static_cast<size_t> (k)], lnFloor, 0.0) * kLnToDb);
    first = false;
}

void GainRule::logMmse (const float* power, const float* noise, const GainSettings& s)
{
    const double strength = std::pow (10.0, s.strengthDb / 10.0);
    for (int k = 0; k < bins; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const double gamma = std::max (power[k] / std::max (noise[k] * strength, 1.0e-30), 1.0e-6);
        // 判定指向型: 声の SNR を、直前のフレームの結果と、今のフレームの結果の重み付きの平均で見積もる
        const double xi = std::max (ddAlpha * prevClean[i] + (1.0 - ddAlpha) * std::max (gamma - 1.0, 0.0), 1.0e-6);
        const double v = xi / (1.0 + xi) * gamma;
        // 対数振幅の平均二乗誤差を最小にする（Ephraim・Malah 1985）: G = ξ / (1 + ξ) × exp(E1(v) / 2)
        const double ln = std::min (std::log (xi / (1.0 + xi)) + 0.5 * expIntE1 (v), 0.0);
        prevClean[i] = static_cast<float> (std::exp (2.0 * ln) * gamma);
        xiNow[i] = static_cast<float> (xi);
        vNow[i] = static_cast<float> (v);
        lg[i] = ln;
    }
}

void GainRule::applyPresence (double lnFloor)
{
    // 声がある確率 p = 1 / (1 + q / (1 − q) × (1 + ξ) e^(−v))。q は声がない確率（前もって）。
    // q は、ξ を時間でならし、近く（3 区画）、広く（31 区画）、フレーム全体の 3 つの平均が −10dB〜−5dB のどこにあるかで、
    // 音声存在確率を 0〜1 にし、その積から決める（Cohen・Berdugo 2001）。G^p × 下限^(1−p) は、対数では p ln G + (1 − p) ln 下限
    for (int k = 0; k < bins; ++k)
        zeta[static_cast<size_t> (k)] = static_cast<float> (zetaAlpha * zeta[static_cast<size_t> (k)] + (1.0 - zetaAlpha) * xiNow[static_cast<size_t> (k)]);
    work[0] = 0;
    for (int k = 0; k < bins; ++k)
        work[static_cast<size_t> (k + 1)] = work[static_cast<size_t> (k)] + zeta[static_cast<size_t> (k)];
    auto average = [&] (int k, int half)
    {
        const int lo = std::max (0, k - half), hi = std::min (bins - 1, k + half);
        return (work[static_cast<size_t> (hi + 1)] - work[static_cast<size_t> (lo)]) / (hi - lo + 1);
    };
    static constexpr double zetaMin = 0.1, zetaMax = 0.316; // −10dB、−5dB
    const double lnRange = std::log (zetaMax / zetaMin);
    auto likelihood = [lnRange] (double z)
    {
        if (z <= zetaMin) return 0.0;
        if (z >= zetaMax) return 1.0;
        return std::log (z / zetaMin) / lnRange;
    };
    const double pFrame = likelihood (work[static_cast<size_t> (bins)] / bins);
    for (int k = 0; k < bins; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const double q = pFrame <= 0.0 ? 0.95 : std::min (1.0 - likelihood (average (k, 1)) * likelihood (average (k, 15)) * pFrame, 0.95);
        const double ratio = q / (1.0 - q);
        const double p = 1.0 / (1.0 + ratio * (1.0 + xiNow[i]) * std::exp (-std::min (static_cast<double> (vNow[i]), 700.0)));
        lg[i] = p * lg[i] + (1.0 - p) * lnFloor;
    }
}

void GainRule::smoothTime (const GainSettings& s)
{
    // 残す割合（対数）の変化を、上がる（開ける）ときはアタック、下がるときはリリースでならす
    const auto attack = static_cast<float> (1.0 - std::exp (-hopSeconds / (std::max (0.1, s.attackMs) * 0.001)));
    const auto release = static_cast<float> (1.0 - std::exp (-hopSeconds / (std::max (1.0, s.releaseMs) * 0.001)));
    for (int k = 0; k < bins; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const auto target = static_cast<float> (lg[i]);
        if (first)
            prevLn[i] = target;
        prevLn[i] += (target - prevLn[i]) * (target > prevLn[i] ? attack : release);
        lg[i] = prevLn[i];
    }
}

void GainRule::smoothCepstrum()
{
    // 残す割合の対数を、ケプストラム（ケフレンシー＝時間の軸）に変える
    for (int k = 0; k < bins; ++k)
        spec[static_cast<size_t> (k)] = { lg[static_cast<size_t> (k)], 0.0 };
    fft.inverse (spec.data(), cep.data());

    // ケフレンシーごとに、時間方向にならす強さを決める（VoiChanAlgoBox と同じ値）:
    //   包絡（0.5ms まで）→ ならさない、声の高さの山（70〜500Hz の周期のあたりで一番大きい山の前後）→ 弱く、それ以外 → 強く。
    //   強さは 16ms 間隔のときの値で 0 / 0.2 / 0.9 とし、今の間隔に合わせる
    const int quarter = size / 2;
    const int envEnd = std::max (1, static_cast<int> (sr * 0.0005));
    const int pitchLo = static_cast<int> (sr / 500.0), pitchHi = std::min (quarter, static_cast<int> (sr / 70.0));
    int pitchQ = -1;
    double pitchVal = 0.2; // これより小さい山は、声の高さとみなさない
    for (int q = pitchLo; q <= pitchHi; ++q)
        if (cep[static_cast<size_t> (q)] > pitchVal)
        {
            pitchVal = cep[static_cast<size_t> (q)];
            pitchQ = q;
        }
    const double scale = hopSeconds / 0.016;
    const double betaPitch = std::pow (0.2, scale), betaConst = std::pow (0.9, scale);
    const double betaSmooth = std::pow (0.8, scale);
    for (int q = 0; q <= quarter; ++q)
    {
        double b = betaConst;
        if (q <= envEnd)
            b = 0.0;
        else if (pitchQ > 0 && std::abs (q - pitchQ) <= 2)
            b = betaPitch;
        auto& bs = beta[static_cast<size_t> (q)];
        bs = first ? b : betaSmooth * bs + (1.0 - betaSmooth) * b;
        auto& cs = cepSmooth[static_cast<size_t> (q)];
        const double c = cep[static_cast<size_t> (q)];
        cs = first ? c : bs * cs + (1.0 - bs) * c;
    }

    // 周波数に戻す（ケプストラムは左右対称なので、ならした片側を写す）
    for (int q = 0; q <= quarter; ++q)
    {
        cep[static_cast<size_t> (q)] = cepSmooth[static_cast<size_t> (q)];
        if (q > 0 && q < quarter)
            cep[static_cast<size_t> (size - q)] = cepSmooth[static_cast<size_t> (q)];
    }
    fft.forward (cep.data(), spec.data());
    for (int k = 0; k < bins; ++k)
        lg[static_cast<size_t> (k)] = spec[static_cast<size_t> (k)].real();
}

void GainRule::smoothErb()
{
    // 残す割合の対数を、前後 ERB の半分ずつの範囲で平均する
    logWork[0] = 0;
    for (int k = 0; k < bins; ++k)
        logWork[static_cast<size_t> (k + 1)] = logWork[static_cast<size_t> (k)] + lg[static_cast<size_t> (k)];
    for (int k = 0; k < bins; ++k)
    {
        const int lo = erbLo[static_cast<size_t> (k)], hi = erbHi[static_cast<size_t> (k)];
        if (hi > lo)
            lg[static_cast<size_t> (k)] = (logWork[static_cast<size_t> (hi + 1)] - logWork[static_cast<size_t> (lo)]) / (hi - lo + 1);
    }
}

void GainRule::protectOnset (const float* power, const float* noise)
{
    for (int k = 0; k < bins; ++k)
    {
        const auto i = static_cast<size_t> (k);
        const float p = power[k];
        const float nz = noise[k] + 1.0e-20f;
        if (p > nz * 10.0f && p > slowPower[i] * 2.0f)
            holdLeft[i] = holdFrames;
        slowPower[i] += slowCoef * (p - slowPower[i]);
        if (holdLeft[i] > 0)
        {
            --holdLeft[i];
            opened[i] = 1.0f;
        }
        else
        {
            opened[i] -= openRelease * opened[i];
        }
        // 開ける量は、SNR が +9dB（8 倍）〜 +15dB（32 倍）の間でなだらかに。開けるときだけ、大きさに戻して計算する
        const float amount = opened[i] * std::clamp ((p / nz - 8.0f) / 24.0f, 0.0f, 1.0f);
        if (amount > 1.0e-4f)
        {
            const double g = std::exp (lg[i]);
            lg[i] = std::log (g + amount * (1.0 - g));
        }
    }
}

} // namespace vcab
