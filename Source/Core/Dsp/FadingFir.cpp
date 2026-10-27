#include "FadingFir.h"

#include <algorithm>
#include <cmath>

namespace vcab
{

void FadingFir::prepare (int taps, int blockSize, int fade)
{
    block = blockSize;
    length = std::max (block, (taps + block - 1) / block * block);
    parts = length / block - 1;
    fadeLength = std::max (1, fade);
    fft.prepare (2 * block);
    const auto bins = static_cast<size_t> (block + 1);
    fdl.assign (static_cast<size_t> (parts + 1), std::vector<Fft::Complex> (bins));
    hist.assign (static_cast<size_t> (2 * block), 0.0f);
    blockIn.assign (static_cast<size_t> (block), 0.0f);
    acc.assign (bins, {});
    pad.assign (static_cast<size_t> (2 * block), 0.0);
    sumNow.assign (static_cast<size_t> (2 * block), 0.0);
    sumPrev.assign (static_cast<size_t> (2 * block), 0.0);
    for (auto& s : slots)
    {
        s.head.assign (static_cast<size_t> (block), 0.0f);
        s.tail.assign (static_cast<size_t> (parts), std::vector<Fft::Complex> (bins));
        s.outCur.assign (static_cast<size_t> (block), 0.0f);
        s.overlap.assign (static_cast<size_t> (block), 0.0f);
    }
    pending.assign (static_cast<size_t> (length), 0.0f);
    reset();
}

void FadingFir::reset()
{
    for (auto& x : fdl)
        std::fill (x.begin(), x.end(), Fft::Complex {});
    std::fill (hist.begin(), hist.end(), 0.0f);
    std::fill (blockIn.begin(), blockIn.end(), 0.0f);
    pos = histPos = fdlNewest = fadeLeft = 0;
    cur = 0;
    for (auto& s : slots)
    {
        s.identity = true;
        s.active = false;
        std::fill (s.outCur.begin(), s.outCur.end(), 0.0f);
        std::fill (s.overlap.begin(), s.overlap.end(), 0.0f);
    }
    slots[0].active = true;
    hasPending = false;
}

void FadingFir::setIdentity()
{
    std::fill (pending.begin(), pending.end(), 0.0f);
    pending[0] = 1.0f;
    setFilter (pending.data());
}

void FadingFir::setFilter (const float* h)
{
    if (h != pending.data())
        std::copy (h, h + length, pending.begin());
    hasPending = true;
    if (fadeLeft == 0 && pos == 0)
    {
        hasPending = false;
        const int next = 1 - cur;
        load (next, pending.data());
        // 素通しから素通しへは、フェードしない
        if (slots[next].identity && slots[cur].identity)
            return;
        slots[next].active = true;
        cur = next;
        fadeLeft = fadeLength;
    }
}

void FadingFir::load (int slotIndex, const float* h)
{
    auto& s = slots[static_cast<size_t> (slotIndex)];
    bool identity = std::abs (h[0] - 1.0f) < 1.0e-7f;
    for (int i = 1; i < length && identity; ++i)
        identity = std::abs (h[i]) < 1.0e-7f;
    s.identity = identity;
    if (identity)
        return;

    std::copy (h, h + block, s.head.begin());
    for (int j = 0; j < parts; ++j)
    {
        for (int i = 0; i < block; ++i)
            pad[static_cast<size_t> (i)] = h[(j + 1) * block + i];
        std::fill (pad.begin() + block, pad.end(), 0.0);
        fft.forward (pad.data(), s.tail[static_cast<size_t> (j)].data());
    }

    // 過去の入力に新しいフィルタをかけた尾の分（次の塊で出す分と、その次へ持ち越す後半）
    if (parts > 0)
    {
        const int older = (fdlNewest + parts) % (parts + 1); // 1 つ前の塊
        tailSum (s, fdlNewest, sumNow);
        tailSum (s, older, sumPrev);
        for (int i = 0; i < block; ++i)
        {
            s.outCur[static_cast<size_t> (i)] = static_cast<float> (sumNow[static_cast<size_t> (i)] + sumPrev[static_cast<size_t> (i + block)]);
            s.overlap[static_cast<size_t> (i)] = static_cast<float> (sumNow[static_cast<size_t> (i + block)]);
        }
    }
}

void FadingFir::tailSum (const Slot& s, int newest, std::vector<double>& out)
{
    std::fill (acc.begin(), acc.end(), Fft::Complex {});
    const int bins = block + 1;
    for (int j = 1; j <= parts; ++j)
    {
        const auto* x = fdl[static_cast<size_t> (((newest - (j - 1)) % (parts + 1) + (parts + 1)) % (parts + 1))].data();
        const auto* g = s.tail[static_cast<size_t> (j - 1)].data();
        auto* a = acc.data();
        for (int i = 0; i < bins; ++i)
        {
            // 複素数の掛け算を展開して書く（Fft.h と同じ理由）
            const double re = x[i].real() * g[i].real() - x[i].imag() * g[i].imag();
            const double im = x[i].real() * g[i].imag() + x[i].imag() * g[i].real();
            a[i] = { a[i].real() + re, a[i].imag() + im };
        }
    }
    fft.inverse (acc.data(), out.data());
}

float FadingFir::headAt (const Slot& s) const noexcept
{
    // hist は 2 倍の長さで、histPos + block − 1 が今の入力（そこから古い向きに block 個）
    const float* x = hist.data() + histPos + block - 1;
    const float* h = s.head.data();
    float y = 0;
    for (int i = 0; i < block; ++i)
        y += h[i] * x[-i];
    return y;
}

float FadingFir::process (float x) noexcept
{
    hist[static_cast<size_t> (histPos)] = x;
    hist[static_cast<size_t> (histPos + block)] = x;
    // headAt は histPos + block − 1 を今の位置として読むので、書いた後に進める前の位置を合わせる
    ++histPos;
    if (histPos == block)
        histPos = 0;
    // 今の入力は hist[histPos − 1 + block]（= histPos + block − 1）

    auto outOf = [&] (const Slot& s)
    {
        if (s.identity)
            return x;
        return headAt (s) + s.outCur[static_cast<size_t> (pos)];
    };

    float y;
    if (fadeLeft > 0)
    {
        const float w = static_cast<float> (fadeLength - fadeLeft + 1) / static_cast<float> (fadeLength);
        y = outOf (slots[cur]) * w + outOf (slots[1 - cur]) * (1.0f - w);
        if (--fadeLeft == 0)
            slots[1 - cur].active = false;
    }
    else
    {
        y = outOf (slots[cur]);
    }

    blockIn[static_cast<size_t> (pos)] = x;
    if (++pos == block)
    {
        endBlock();
        pos = 0;
        if (hasPending && fadeLeft == 0)
            setFilter (pending.data());
    }
    return y;
}

void FadingFir::endBlock()
{
    if (parts == 0)
        return;
    // 入力の塊の FFT を、リングの次の位置に入れる
    fdlNewest = (fdlNewest + 1) % (parts + 1);
    for (int i = 0; i < block; ++i)
        pad[static_cast<size_t> (i)] = blockIn[static_cast<size_t> (i)];
    std::fill (pad.begin() + block, pad.end(), 0.0);
    fft.forward (pad.data(), fdl[static_cast<size_t> (fdlNewest)].data());

    for (auto& s : slots)
    {
        if (! s.active || s.identity)
            continue;
        tailSum (s, fdlNewest, sumNow);
        for (int i = 0; i < block; ++i)
        {
            s.outCur[static_cast<size_t> (i)] = static_cast<float> (sumNow[static_cast<size_t> (i)]) + s.overlap[static_cast<size_t> (i)];
            s.overlap[static_cast<size_t> (i)] = static_cast<float> (sumNow[static_cast<size_t> (i + block)]);
        }
    }
}

} // namespace vcab
