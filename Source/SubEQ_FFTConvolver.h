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
    std::vector<double> overlap;                    // size L (conv FFT size)
    std::vector<double> outQueue;                   // output FIFO
    int outRead = 0;
};

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
    for (int n = 0; n < numSamples; ++n)
    {
        st.inputBlock[st.pos] = { static_cast<double>(input[n]), 0.0 };
        ++st.pos;

        if (st.pos == M)
        {
            for (int i = 0; i < M; ++i)
                scratch[i] = st.inputBlock[i];
            for (int i = M; i < L; ++i)
                scratch[i] = { 0.0, 0.0 };

            fftInPlace(scratch.data(), L);
            for (int i = 0; i < L; ++i)
                scratch[i] *= freqCoeffs[i];
            ifftInPlace(scratch.data(), L);

            bool poisoned = false;
            for (int i = 0; i < M; ++i)
            {
                double y = scratch[i].real() + st.overlap[i];
                if (std::isnan(y) || std::isinf(y))
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
                // so older tails must accumulate).
                for (int i = 0; i < L - M; ++i)
                    st.overlap[i] = st.overlap[i + M] + scratch[M + i].real();
                for (int i = L - M; i < L; ++i)
                    st.overlap[i] = 0.0;
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
