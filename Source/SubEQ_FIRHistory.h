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
            published.store(0, std::memory_order_release);
        }
    }

    void reset() noexcept
    {
        std::fill(data.begin(), data.end(), 0.0f);
        writeIndex = 0;
        written = 0;
        published.store(0, std::memory_order_release);
    }

    // Write one block for every channel. channelData may contain fewer channels
    // than prepared; missing channels are written as silence. publishNow=false
    // lets the caller write the current block before processing but expose it
    // only after the convolution has advanced (so warm states always align
    // with the start of the next block).
    void writeFrame(const float* const* channelData, int numChannelsIn, int numSamples,
                    bool publishNow = true) noexcept
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

        if (publishNow)
            published.store(written, std::memory_order_release);
    }

    void publish() noexcept
    {
        published.store(written, std::memory_order_release);
    }

    uint64_t publishedCount() const noexcept
    {
        return published.load(std::memory_order_acquire);
    }

    int capacity() const noexcept { return cap; }
    int numChannels() const noexcept { return numCh; }

    // Copy `count` samples ending at endExclusive (i.e. [end-count, end)).
    // Returns false when the range is not yet published or has been overwritten.
    bool snapshot(int channel, uint64_t endExclusive, int count, float* out) const noexcept
    {
        if (out == nullptr || count < 0 || channel < 0 || channel >= numCh || cap <= 0)
            return false;

        const uint64_t end = endExclusive;
        if (end > publishedCount() || end < static_cast<uint64_t>(count))
            return false;

        const uint64_t start = end - static_cast<uint64_t>(count);
        const uint64_t pub = publishedCount();
        if (pub > static_cast<uint64_t>(cap)
            && start < pub - static_cast<uint64_t>(cap))
            return false;

        size_t src = static_cast<size_t>(start % static_cast<uint64_t>(cap));
        const size_t base = static_cast<size_t>(channel) * static_cast<size_t>(cap);

        for (int i = 0; i < count; ++i)
        {
            out[i] = data[base + src];
            if (++src == static_cast<size_t>(cap))
                src = 0;
        }

        return true;
    }

private:
    std::vector<float> data;
    int numCh = 0;
    int cap = 0;
    int writeIndex = 0;
    uint64_t written = 0;
    std::atomic<uint64_t> published { 0 };
};

} // namespace SubEQ
