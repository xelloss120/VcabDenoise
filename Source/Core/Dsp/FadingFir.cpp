#include "FadingFir.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
 #include <xmmintrin.h>
 #define VCAB_FIR_SSE 1
#endif

namespace vcab
{

void FadingFir::prepare (int taps, int blockSize, int fade)
{
    // setFilter は length 個を読むので、taps は block の倍数にする（切り上げると、渡された配列の外を読む）
    assert (taps >= blockSize && taps % blockSize == 0);
    block = blockSize;
    length = std::max (block, (taps + block - 1) / block * block);
    parts = length / block - 1;
    fadeLength = std::max (1, fade);
    fft.prepare (2 * block);
    const auto fftSize = static_cast<size_t> (2 * block);
    fdl.clear();
    fdl.resize (static_cast<size_t> (parts + 1));
    for (auto& x : fdl)
        x.assign (fftSize);
    hist.assign (static_cast<size_t> (2 * block), 0.0f);
    blockIn.assign (static_cast<size_t> (block), 0.0f);
    for (auto* b : { &acc, &pad, &sumNow, &sumPrev })
        b->assign (fftSize);
    for (auto& s : slots)
    {
        s.headRev.assign (static_cast<size_t> (block), 0.0f);
        s.tail.clear();
        s.tail.resize (static_cast<size_t> (parts));
        for (auto& g : s.tail)
            g.assign (fftSize);
        s.outCur.assign (static_cast<size_t> (block), 0.0f);
        s.overlap.assign (static_cast<size_t> (block), 0.0f);
    }
    pending.assign (static_cast<size_t> (length), 0.0f);
    reset();
}

void FadingFir::reset()
{
    for (auto& x : fdl)
        x.clear();
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

    for (int i = 0; i < block; ++i)
        s.headRev[static_cast<size_t> (block - 1 - i)] = h[i];
    for (int j = 0; j < parts; ++j)
    {
        for (int i = 0; i < block; ++i)
            pad[static_cast<size_t> (i)] = h[(j + 1) * block + i];
        std::fill (pad.get() + block, pad.get() + 2 * block, 0.0f);
        fft.forwardRaw (pad.get(), s.tail[static_cast<size_t> (j)].get());
    }

    // 過去の入力に新しいフィルタをかけた尾の分（次の塊で出す分と、その次へ持ち越す後半）
    if (parts > 0)
    {
        const int older = (fdlNewest + parts) % (parts + 1); // 1 つ前の塊
        tailSum (s, fdlNewest, sumNow);
        tailSum (s, older, sumPrev);
        for (int i = 0; i < block; ++i)
        {
            s.outCur[static_cast<size_t> (i)] = sumNow[static_cast<size_t> (i)] + sumPrev[static_cast<size_t> (i + block)];
            s.overlap[static_cast<size_t> (i)] = sumNow[static_cast<size_t> (i + block)];
        }
    }
}

void FadingFir::tailSum (const Slot& s, int newest, AlignedFloats& out)
{
    acc.clear();
    const float scale = 1.0f / static_cast<float> (2 * block); // 逆 FFT の 1/N を、ここでかけておく
    for (int j = 1; j <= parts; ++j)
    {
        const auto& x = fdl[static_cast<size_t> (((newest - (j - 1)) % (parts + 1) + (parts + 1)) % (parts + 1))];
        fft.multiplyAdd (x.get(), s.tail[static_cast<size_t> (j - 1)].get(), acc.get(), scale);
    }
    fft.inverseRaw (acc.get(), out.get());
}

float FadingFir::headAt (const Slot& s) const noexcept
{
    // hist は 2 倍の長さで、histPos + block − 1 が今の入力（そこから古い向きに block 個）
    // 頭は逆順（headRev[j] = h[block − 1 − j]）に持ち、hist[histPos] から古い順に並ぶ入力と、前から掛けて足す
    const float* x = hist.data() + histPos;
    const float* h = s.headRev.data();
#ifdef VCAB_FIR_SSE
    __m128 a0 = _mm_setzero_ps(), a1 = _mm_setzero_ps(), a2 = _mm_setzero_ps(), a3 = _mm_setzero_ps();
    for (int j = 0; j < block; j += 16)
    {
        a0 = _mm_add_ps (a0, _mm_mul_ps (_mm_loadu_ps (h + j), _mm_loadu_ps (x + j)));
        a1 = _mm_add_ps (a1, _mm_mul_ps (_mm_loadu_ps (h + j + 4), _mm_loadu_ps (x + j + 4)));
        a2 = _mm_add_ps (a2, _mm_mul_ps (_mm_loadu_ps (h + j + 8), _mm_loadu_ps (x + j + 8)));
        a3 = _mm_add_ps (a3, _mm_mul_ps (_mm_loadu_ps (h + j + 12), _mm_loadu_ps (x + j + 12)));
    }
    const __m128 a = _mm_add_ps (_mm_add_ps (a0, a1), _mm_add_ps (a2, a3));
    alignas (16) float lanes[4];
    _mm_store_ps (lanes, a);
    return (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
#else
    float y = 0;
    for (int j = 0; j < block; ++j)
        y += h[j] * x[j];
    return y;
#endif
}

float FadingFir::process (float x) noexcept
{
    // 同じ値を 2 か所に書き、位置を進める。進めた後は、今の入力が hist[histPos + block − 1] にあり、
    // そこから古い向きに block 個が折り返さずに並ぶ（headAt はそう読む）
    hist[static_cast<size_t> (histPos)] = x;
    hist[static_cast<size_t> (histPos + block)] = x;
    if (++histPos == block)
        histPos = 0;

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
    std::fill (pad.get() + block, pad.get() + 2 * block, 0.0f);
    fft.forwardRaw (pad.get(), fdl[static_cast<size_t> (fdlNewest)].get());

    for (auto& s : slots)
    {
        if (! s.active || s.identity)
            continue;
        tailSum (s, fdlNewest, sumNow);
        for (int i = 0; i < block; ++i)
        {
            s.outCur[static_cast<size_t> (i)] = sumNow[static_cast<size_t> (i)] + s.overlap[static_cast<size_t> (i)];
            s.overlap[static_cast<size_t> (i)] = sumNow[static_cast<size_t> (i + block)];
        }
    }
}

} // namespace vcab
