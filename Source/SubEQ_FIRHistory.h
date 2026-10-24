/*
  ==============================================================================

    SubEQ_FIRHistory.h
    Single-writer / single-reader input history ring used by the FIR warm-up
    worker. The audio thread writes whole blocks and publishes the total sample
    count; the background thread copies a past range that lies behind the write
    frontier, so no lock is needed. No JUCE dependency.

  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace SubEQ
{

class FirHistoryRing
{
public:
    void prepare(int numChannels, int capacitySamples)
    {
        numCh = std::max(1, numChannels);
        cap = std::max(1, capacitySamples);

        // Allocate once. Never reallocate or clear while the background warm
        // builder may be reading: a new sample-rate session is delimited by
        // FFTProcessor::historyValidFrom instead of by resetting the ring.
        const size_t needed = static_cast<size_t>(numCh) * static_cast<size_t>(cap);
        if (data.size() != needed)
        {
            data.assign(needed, 0.0f);
            writeIndex = 0;
            written = 0;
            captured.store(0, std::memory_order_release);
            committed.store(0, std::memory_order_release);
        }
    }

    void reset() noexcept
    {
        std::fill(data.begin(), data.end(), 0.0f);
        writeIndex = 0;
        written = 0;
        captured.store(0, std::memory_order_release);
        committed.store(0, std::memory_order_release);
    }

    // Write one block for every channel. channelData may contain fewer channels
    // than prepared; missing channels are written as silence.
    // `commit=true` publishes both captured and committed frontiers (legacy
    // single-call use). `commit=false` exposes the block to the background
    // warm builder via `captured` while the audio thread keeps `committed` at
    // the block start; the caller then calls publish() at block end.
    void writeFrame(const float* const* channelData, int numChannelsIn, int numSamples,
                    bool commit = true) noexcept
    {
        if (cap <= 0 || data.empty() || numSamples <= 0)
            return;

        const int writeChannels = (channelData != nullptr)
                                      ? std::min(numCh, std::max(0, numChannelsIn))
                                      : numCh;

        for (int n = 0; n < numSamples; ++n)
        {
            for (int ch = 0; ch < writeChannels; ++ch)
            {
                const float x = (channelData != nullptr && channelData[ch] != nullptr)
                                    ? channelData[ch][n]
                                    : 0.0f;
                data[static_cast<size_t>(ch) * static_cast<size_t>(cap)
                     + static_cast<size_t>(writeIndex)] = x;
            }

            if (++writeIndex == cap)
                writeIndex = 0;
            ++written;
        }

        captured.store(written, std::memory_order_release);
        if (commit)
            committed.store(written, std::memory_order_release);
    }

    // Commit the currently captured frontier as processed. Called at the end
    // of the audio block, after convolution has advanced.
    void publish() noexcept
    {
        committed.store(captured.load(std::memory_order_acquire),
                        std::memory_order_release);
    }

    uint64_t publishedCount() const noexcept
    {
        return committed.load(std::memory_order_acquire);
    }

    uint64_t capturedCount() const noexcept
    {
        return captured.load(std::memory_order_acquire);
    }

    int capacity() const noexcept { return cap; }
    int numChannels() const noexcept { return numCh; }

    // Copy `count` samples ending at endExclusive (i.e. [end-count, end)).
    // Returns false when the range is not yet captured or has been overwritten.
    // `captured` includes the block currently being processed; that block is
    // fully written before the counter is published, so it is safe to read.
    bool snapshot(int channel, uint64_t endExclusive, int count, float* out) const noexcept
    {
        if (out == nullptr || count < 0 || channel < 0 || channel >= numCh || cap <= 0)
            return false;

        const uint64_t end = endExclusive;
        const uint64_t capNow = capturedCount();
        if (end > capNow || end < static_cast<uint64_t>(count))
            return false;

        const uint64_t start = end - static_cast<uint64_t>(count);
        if (capNow > static_cast<uint64_t>(cap)
            && start < capNow - static_cast<uint64_t>(cap))
            return false;

        size_t src = static_cast<size_t>(start % static_cast<uint64_t>(cap));
        const size_t base = static_cast<size_t>(channel) * static_cast<size_t>(cap);

        for (int i = 0; i < count; ++i)
        {
            out[i] = data[base + src];
            if (++src == static_cast<size_t>(cap))
                src = 0;
        }

        // A writer advancing far enough during the copy could have wrapped
        // over the oldest slots; reject rather than returning torn history.
        const uint64_t after = capturedCount();
        if (after > static_cast<uint64_t>(cap)
            && start < after - static_cast<uint64_t>(cap))
            return false;

        return true;
    }

    // Snapshot [start, start+count), zero-filling anything before validFrom.
    bool snapshotPadded(int channel, uint64_t start, int count, uint64_t validFrom,
                        float* out) const noexcept
    {
        if (out == nullptr || count < 0 || channel < 0 || channel >= numCh)
            return false;

        std::fill(out, out + count, 0.0f);

        const uint64_t end = start + static_cast<uint64_t>(count);
        if (end <= validFrom)
            return end <= capturedCount();

        const uint64_t readStart = std::max(start, validFrom);
        if (readStart >= end)
            return end <= capturedCount();

        const int prefix = static_cast<int>(readStart - start);
        const int readCount = count - prefix;
        return snapshot(channel, end, readCount, out + prefix);
    }

private:
    std::vector<float> data;
    int numCh = 0;
    int cap = 0;
    int writeIndex = 0;
    uint64_t written = 0;
    std::atomic<uint64_t> captured { 0 };
    std::atomic<uint64_t> committed { 0 };
};

} // namespace SubEQ
