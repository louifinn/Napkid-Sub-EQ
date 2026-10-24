/*
  ==============================================================================

    SubEQ_OlaBank.h
    JUCE-free helpers for exact OLA warm-state synthesis. The background warm
    builder and the offline regression tests share this implementation.

    Given a target OLA boundary T (= k*M), the accumulator at S=T-M depends
    only on the last N input samples before S. One overlap-save convolution at
    S produces that accumulator exactly; replaying the single block [S,T)
    rebuilds the pending output FIFO and advances the state to T.

  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "SubEQ_FFTConvolver.h"
#include "SubEQ_FIRHistory.h"

namespace SubEQ
{

// Fill `state.overlap` with the exact accumulator at boundaryS, and clear the
// pending output queue. `inputScratch` must hold at least firLength samples;
// `scratch` must hold at least convFFTSize complex samples.
inline bool olaSynthesizeOverlap(const FirHistoryRing& history, int channel,
                                 uint64_t boundaryS, uint64_t validFrom,
                                 int firLength, int convFFTSize, int M,
                                 const std::complex<double>* freqCoeffs,
                                 OlaChannelState& state,
                                 std::vector<float>& inputScratch,
                                 std::vector<std::complex<double>>& scratch)
{
    const int N = firLength;
    const int L = convFFTSize;
    if (N <= 0 || L < M || freqCoeffs == nullptr
        || static_cast<int>(inputScratch.size()) < N
        || static_cast<int>(scratch.size()) < L
        || static_cast<int>(state.overlap.size()) < L)
        return false;

    std::fill(inputScratch.begin(), inputScratch.begin() + N, 0.0f);
    if (boundaryS > 0)
    {
        const int count = static_cast<int>(
            std::min<uint64_t>(static_cast<uint64_t>(N), boundaryS));
        const uint64_t start = boundaryS - static_cast<uint64_t>(count);
        const int offset = N - count;
        if (!history.snapshotPadded(channel, start, count, validFrom,
                                    inputScratch.data() + offset))
            return false;
    }

    std::fill(scratch.begin(), scratch.begin() + L, std::complex<double>{});
    for (int i = 0; i < N; ++i)
        scratch[static_cast<size_t>(i)] = { inputScratch[static_cast<size_t>(i)], 0.0 };

    fftInPlace(scratch.data(), L);
    for (int i = 0; i < L; ++i)
        scratch[static_cast<size_t>(i)] *= freqCoeffs[static_cast<size_t>(i)];
    ifftInPlace(scratch.data(), L);

    std::fill(state.overlap.begin(), state.overlap.end(), 0.0);
    const int overlapLen = L - M;
    for (int m = 0; m < overlapLen; ++m)
    {
        const int64_t idx = static_cast<int64_t>(N) + m;
        if (idx < static_cast<int64_t>(2 * N - 1))
        {
            const double v = scratch[static_cast<size_t>(idx)].real();
            state.overlap[static_cast<size_t>(m)] = std::isfinite(v) ? v : 0.0;
        }
    }

    state.pos = 0;
    state.outQueue.clear();
    state.outRead = 0;
    return true;
}

// Replay exactly one OLA block [startS, startS+M) into an already synthesized
// state. This rebuilds the pending output FIFO and advances the accumulator
// from startS to startS+M (the target boundary).
inline bool olaReplayBlock(const FirHistoryRing& history, int channel,
                           uint64_t startS, uint64_t validFrom,
                           int convFFTSize, int M,
                           const std::complex<double>* freqCoeffs,
                           OlaChannelState& state,
                           std::vector<float>& inputScratch,
                           std::vector<float>& dummyOutput,
                           std::vector<std::complex<double>>& scratch)
{
    if (convFFTSize < M || freqCoeffs == nullptr
        || static_cast<int>(inputScratch.size()) < M
        || static_cast<int>(dummyOutput.size()) < M
        || static_cast<int>(scratch.size()) < convFFTSize
        || static_cast<int>(state.overlap.size()) < convFFTSize)
        return false;

    if (!history.snapshotPadded(channel, startS, M, validFrom, inputScratch.data()))
        return false;

    processOlaChannel(state, inputScratch.data(), dummyOutput.data(), M,
                      M, convFFTSize, freqCoeffs, scratch);
    return true;
}

// Synthesize the exact OLA state at an arbitrary host block start T.
// S = T-M is synthesized with overlap-save, then the single block [S,T) is
// replayed. This split lets the worker compute the overlap one block early;
// only the final replay depends on samples at T. The caller must have captured
// the history through T before calling.
inline bool olaSynthesizeAt(const FirHistoryRing& history, int channel,
                            uint64_t targetSample, uint64_t validFrom,
                            int firLength, int convFFTSize, int M,
                            const std::complex<double>* freqCoeffs,
                            OlaChannelState& state,
                            std::vector<float>& inputScratch,
                            std::vector<float>& dummyOutput,
                            std::vector<std::complex<double>>& scratch)
{
    if (M <= 0 || firLength <= 0
        || convFFTSize < M || freqCoeffs == nullptr
        || static_cast<int>(inputScratch.size()) < std::max(firLength, M)
        || static_cast<int>(dummyOutput.size()) < M
        || static_cast<int>(scratch.size()) < convFFTSize
        || static_cast<int>(state.overlap.size()) < convFFTSize
        || static_cast<int>(state.inputBlock.size()) < M)
        return false;

    const uint64_t boundary = targetSample
                            - (targetSample % static_cast<uint64_t>(M));
    const int prefix = static_cast<int>(targetSample - boundary);

    if (boundary == 0)
    {
        // Session start: zero state, then feed the first T samples.
        std::fill(state.overlap.begin(), state.overlap.end(), 0.0);
        state.pos = 0;
        state.outQueue.clear();
        state.outRead = 0;
    }
    else
    {
        const uint64_t S = boundary - static_cast<uint64_t>(M);
        if (!olaSynthesizeOverlap(history, channel, S, validFrom,
                                  firLength, convFFTSize, M, freqCoeffs,
                                  state, inputScratch, scratch))
            return false;

        if (!olaReplayBlock(history, channel, S, validFrom, convFFTSize, M,
                            freqCoeffs, state, inputScratch, dummyOutput, scratch))
            return false;
    }

    if (prefix > 0)
    {
        if (!history.snapshotPadded(channel, boundary, prefix, validFrom,
                                    inputScratch.data()))
            return false;
        processOlaChannel(state, inputScratch.data(), dummyOutput.data(), prefix,
                          M, convFFTSize, freqCoeffs, scratch);
    }

    return true;
}

} // namespace SubEQ
