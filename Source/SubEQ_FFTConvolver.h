/*
  ==============================================================================

    SubEQ_FFTConvolver.h
    Per-channel overlap-add convolution primitive (blocked, partition-free),
    with no JUCE dependency. Consumes a frequency-domain filter and a shared
    scratch buffer; owns the per-channel accumulator/FIFO state.

    Extracted from FFTProcessor::process so the shipped convolution core (not
    a hand-copied mirror) is what the standalone regression tests exercise.
    The version-switch flush and NaN-poisoning rules live here too, because
    they are exactly the bug-prone runtime logic the tests must cover.

  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

#include "SubEQ_DSPMath.h"

namespace SubEQ
{

// Per-channel overlap-add state. `overlap` is the convolution tail accumulator
// (size = conv FFT size L); `outQueue` is the output FIFO (one sample drained
// per input sample, M samples enqueued per completed block).
struct OlaChannelState
{
    std::vector<std::complex<double>> inputBlock;   // size M (block length)
    int pos = 0;
    std::vector<double> overlap;                    // capacity = max L (never resized on audio thread)
    std::vector<double> outQueue;                   // fixed-capacity output FIFO (reserve 2M)
    int outRead = 0;
};

// Allocate/reset one OLA channel for the lifetime of a prepared processor.
// `overlap` is deliberately sized to the largest supported convolution FFT so
// switching to a longer FIR can never write past the accumulator; the logical
// L is passed to processOlaChannel and must be <= this capacity.
inline void prepareOlaChannelState(OlaChannelState& st, int M, int maxFftSize)
{
    st.inputBlock.assign(static_cast<size_t>(std::max(1, M)), {});
    st.pos = 0;
    st.overlap.assign(static_cast<size_t>(std::max(1, maxFftSize)), 0.0);
    st.outQueue.clear();
    st.outQueue.reserve(static_cast<size_t>(std::max(1, M)) * 2);
    st.outRead = 0;
}

// Build the frequency-domain filter used by processOlaChannel. The tap count
// is authoritative: callers may pass a Type-I linear-phase design whose size
// is one less than the nominal FIR-length parameter. Taps beyond the FFT size
// are ignored rather than read out of bounds.
inline std::vector<std::complex<double>> buildFirFrequencyCoeffs(
    const std::vector<double>& taps, int convFFTSize)
{
    if (convFFTSize <= 0)
        return {};

    std::vector<std::complex<double>> spec(static_cast<size_t>(convFFTSize));
    const size_t count = std::min(taps.size(), spec.size());
    for (size_t i = 0; i < count; ++i)
        spec[i] = { taps[i], 0.0 };

    fftInPlace(spec.data(), convFFTSize);
    return spec;
}

// Process `numSamples` through one channel. `scratch` is a caller-owned work
// buffer of size L shared across channels to bound memory (one scratch per
// processor, not per channel). `freqCoeffs` is the frequency-domain filter
// (size L). `M` is the block length; `L` the convolution FFT size.
// Templated on the sample type so the audio path (float) and the regression
// tests (double, for exact comparison against a reference convolution) share
// the same implementation; the accumulator always runs in double.
//
// The convolution is partition-free: every M input samples, the block is FFT'd,
// multiplied by the filter, inverse-FFT'd, and the first M samples (plus the
// accumulated tail) are pushed onto the output FIFO. A NaN/Inf anywhere in a
// block flushes the accumulator so one poisoned block cannot mute the output
// until a manual reset.
template <typename T>
inline void processOlaChannel(OlaChannelState& st,
                              const T* input, T* output, int numSamples,
                              int M, int L,
                              const std::complex<double>* freqCoeffs,
                              std::vector<std::complex<double>>& scratch)
{
    // Fail closed: a state prepared for a shorter design must never be driven
    // with a larger L. This is the invariant that makes zero-state fallback
    // and warm-wrapper reuse safe; Debug/ASan builds surface it immediately.
    const bool valid = M > 0 && L >= M && L <= static_cast<int>(st.overlap.size())
                    && static_cast<int>(st.inputBlock.size()) >= M
                    && st.pos >= 0 && st.pos < M
                    && static_cast<int>(scratch.size()) >= L
                    && freqCoeffs != nullptr;
    if (!valid)
    {
        for (int n = 0; n < numSamples; ++n)
            output[n] = static_cast<T>(0);
        return;
    }

    for (int n = 0; n < numSamples; ++n)
    {
        st.inputBlock[st.pos] = { static_cast<double>(input[n]), 0.0 };
        ++st.pos;

        if (st.pos == M)
        {
            if (st.outQueue.size() + static_cast<size_t>(M) > st.outQueue.capacity())
            {
                // Should be unreachable: the queue drains one sample per input
                // and peaks at 2M-1. Fail closed rather than allocate/UB.
                st.outQueue.clear();
                st.outRead = 0;
                for (int n2 = n; n2 < numSamples; ++n2)
                    output[n2] = static_cast<T>(0);
                return;
            }

            for (int i = 0; i < M; ++i)
                scratch[i] = st.inputBlock[i];
            for (int i = M; i < L; ++i)
                scratch[i] = { 0.0, 0.0 };

            fftInPlace(scratch.data(), L);
            for (int i = 0; i < L; ++i)
                scratch[i] *= freqCoeffs[i];
            ifftInPlace(scratch.data(), L);

            bool poisoned = false;
            // F6：卷积累加始终是 double；即使结果有限，也可能超出输出类型
            // T（音频路径为 float）的可表示范围——此时 static_cast<T> 会变成
            // ±inf。输出类型无法表示的有限值按污染处理（该块输出置零并冲刷
            // 累加器），与 NaN/Inf 同一路径，保证音频输出永不为 inf。
            const double maxRepresentable = static_cast<double>(std::numeric_limits<T>::max());
            for (int i = 0; i < M; ++i)
            {
                double y = scratch[i].real() + st.overlap[i];
                if (!std::isfinite(y) || std::abs(y) > maxRepresentable)
                {
                    y = 0.0;
                    poisoned = true;
                }
                st.outQueue.push_back(y);
            }

            if (poisoned)
            {
                std::fill(st.overlap.begin(), st.overlap.end(), 0.0);
            }
            else
            {
                // Carry the tail into the next block: shift the accumulator and
                // ADD this block's tail (a convolution spans multiple blocks,
                // so older tails must accumulate). Check the tail too: a
                // poisoned future block must not hide until it reaches y.
                bool tailPoisoned = false;
                for (int i = 0; i < L - M; ++i)
                {
                    if (!std::isfinite (scratch[M + i].real())
                        || !std::isfinite (scratch[M + i].imag()))
                    {
                        tailPoisoned = true;
                        st.overlap[i] = 0.0;
                        continue;
                    }

                    const double v = st.overlap[i + M] + scratch[M + i].real();
                    if (!std::isfinite(v))
                    {
                        tailPoisoned = true;
                        st.overlap[i] = 0.0;
                    }
                    else
                    {
                        st.overlap[i] = v;
                    }
                }
                for (int i = L - M; i < L; ++i)
                    st.overlap[i] = 0.0;

                if (tailPoisoned)
                    std::fill(st.overlap.begin(), st.overlap.end(), 0.0);
            }

            st.pos = 0;
        }

        // Drain one output sample (zero during the initial block latency)
        if (st.outRead < static_cast<int>(st.outQueue.size()))
            output[n] = static_cast<T>(st.outQueue[st.outRead++]);
        else
            output[n] = static_cast<T>(0);

        if (st.outRead == static_cast<int>(st.outQueue.size()))
        {
            st.outQueue.clear();
            st.outRead = 0;
        }
    }
}

} // namespace SubEQ
