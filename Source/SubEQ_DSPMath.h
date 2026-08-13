/*
  ==============================================================================

    SubEQ_DSPMath.h
    Shared standalone DSP math helpers (header-only, no JUCE dependency):
      - template radix-2 FFT / IFFT (float or double, in-place)
      - nextPowerOfTwo helpers for overlap-add sizing

    Shared by SubEQ_FFTProcessor.cpp (design + real-time convolution) and the
    standalone regression tests (Tests/subeq_fft_test.cpp) so the tested code
    is exactly the shipped code — no copy drift.

  ==============================================================================
*/

#pragma once

#include <complex>
#include <cmath>
#include <vector>

namespace SubEQ
{

namespace detail
{
    // Per-thread twiddle tables, one slot per FFT size: w[m] = exp(-2*pi*i*m/n),
    // m in [0, n/2). The butterfly loop does a table lookup instead of the
    // recurrence w *= wlen — this removes the twiddle rounding accumulation
    // that dominates float FFT error on long transforms.
    //
    // Multiple sizes coexist per thread: the audio thread interleaves
    // convolution FFTs (8192/32768/131072) with spectrum FFTs (4096/8192/
    // 16384), and a single-slot cache would rebuild the whole table on every
    // call whenever the two sizes differ (steady-state allocation + O(n)
    // trigonometry on the audio thread). Slots are indexed by log2(n), so
    // switching FIR length or spectrum size never triggers a rebuild once
    // each size has been seen.
    template <typename T>
    struct TwiddleCache
    {
        struct Entry
        {
            std::vector<std::complex<T>> w;
            int n = 0;
        };
        Entry slots[21];   // slot = log2(n); supports n up to 2^20
    };

    template <typename T>
    inline const std::complex<T>* getTwiddles(int n)
    {
        static thread_local TwiddleCache<T> cache;

        int slot = 0;
        while (slot < 20 && (1 << (slot + 1)) <= n)
            ++slot;

        auto& e = cache.slots[slot];
        if (e.n != n)
        {
            e.w.resize(static_cast<size_t>(n) / 2);
            for (int m = 0; m < n / 2; ++m)
            {
                const double ang = -2.0 * 3.14159265358979323846 * m / n;
                e.w[static_cast<size_t>(m)] = { static_cast<T>(std::cos(ang)),
                                                static_cast<T>(std::sin(ang)) };
            }
            e.n = n;
        }

        return e.w.data();
    }
}

// Builds the twiddle table for size n on the calling thread ahead of time, so
// the first real-time FFT does not allocate. Call from prepare()-like setup
// paths; effective only if called on the same thread that later runs the FFT
// (the cache is thread_local), which is the common JUCE prepareToPlay case.
// Safe to call repeatedly — subsequent calls are cheap lookups.
template <typename T>
inline void prewarmTwiddleTable(int n)
{
    detail::getTwiddles<T>(n);
}

// In-place iterative radix-2 FFT (no scaling). T = float or double, n must be
// a power of two (n >= 2).
//
// Optimizations vs. the plain recurrence version:
//   - stages len=2 and len=4 use twiddles +/-1 and +/-i -> pure add/sub and
//     sign/exchange, no multiplications;
//   - all remaining stages look up precomputed twiddle factors (no w *= wlen
//     recurrence, so no accumulating rounding and one less multiply per
//     butterfly).
template <typename T>
inline void fftInPlace(std::complex<T>* data, int n)
{
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;

        if (i < j)
            std::swap(data[i], data[j]);
    }

    // len = 2 stage: twiddles are +1 / -1 -> pure add/sub
    for (int i = 0; i < n; i += 2)
    {
        const std::complex<T> u = data[i];
        const std::complex<T> v = data[i + 1];
        data[i] = u + v;
        data[i + 1] = u - v;
    }

    if (n < 4)
        return;

    // len = 4 stage: twiddles are 1, -i, -1, i -> no multiplications
    for (int i = 0; i < n; i += 4)
    {
        const std::complex<T> u0 = data[i];
        const std::complex<T> v0 = data[i + 2];
        data[i] = u0 + v0;
        data[i + 2] = u0 - v0;

        const std::complex<T> u1 = data[i + 1];
        const std::complex<T> v1 = data[i + 3];
        const std::complex<T> v1r(v1.imag(), -v1.real());   // v1 * (-i)
        data[i + 1] = u1 + v1r;
        data[i + 3] = u1 - v1r;
    }

    if (n < 8)
        return;

    // Remaining stages with precomputed twiddle lookups (no recurrence error).
    // Stage of size `len` uses table entries k * (n / len) for k in [0, len/2).
    const std::complex<T>* tw = detail::getTwiddles<T>(n);

    for (int len = 8; len <= n; len <<= 1)
    {
        const int half = len / 2;
        const int stride = n / len;

        for (int i = 0; i < n; i += len)
        {
            for (int k = 0; k < half; ++k)
            {
                const std::complex<T> u = data[i + k];
                const std::complex<T> v = data[i + k + half] * tw[k * stride];
                data[i + k] = u + v;
                data[i + k + half] = u - v;
            }
        }
    }
}

// In-place inverse FFT (scales by 1/N)
template <typename T>
inline void ifftInPlace(std::complex<T>* data, int n)
{
    for (int i = 0; i < n; ++i)
        data[i] = std::conj(data[i]);

    fftInPlace(data, n);

    const T invN = static_cast<T>(1) / static_cast<T>(n);
    for (int i = 0; i < n; ++i)
        data[i] = std::conj(data[i]) * invN;
}

// Smallest power of two >= value (value must be >= 1)
inline int nextPowerOfTwo(int value)
{
    int p = 1;
    while (p < value)
        p <<= 1;
    return p;
}

// FFT size for a linear convolution of an FIR of `firLength` taps with blocks
// of `blockLength` samples: nextPow2(firLength + blockLength - 1).
inline int convolutionFftSize(int firLength, int blockLength)
{
    return nextPowerOfTwo(firLength + blockLength - 1);
}

} // namespace SubEQ
