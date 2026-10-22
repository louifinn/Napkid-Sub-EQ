/*
  ==============================================================================

    SubEQ_FFTProcessor.cpp
    FIR coefficient design (Linear Phase + Minimum Phase) and FFT overlap-add
    convolution. Coefficient design runs on a background thread with the shared
    radix-2 FFT (SubEQ_DSPMath.h) — JUCE FFT wrapper is unreliable here.

    v0.5.0 T2 switching:
      - raw input history ring (audio writer / background reader)
      - background shadow OLA convolution builds a pre-warmed state exactly at
        the latest completed OLA block boundary
      - audio swaps the warm state in and crossfades only when the design's
        group delay is unchanged; latency-changing designs flush at a block
        boundary (PDC handshake is layered on top in PluginProcessor)
      - no shared_ptr/vector of a large object is ever destroyed on the audio
        thread; superseded states go through the retirement rings

  ==============================================================================
*/

#include "SubEQ_FFTProcessor.h"
#include "SubEQ_DSPMath.h"
#include "SubEQ_Parameters.h"
#include "SubEQ_FIRDesign.h"
#include "SubEQ_FIRSwitchState.h"

namespace SubEQ
{

//==============================================================================
FFTProcessor::FFTProcessor()  : juce::Thread ("SubEQ FIR Designer")
{
    // The background design engine must always evaluate exact target
    // coefficients, so disable smoothing on it.
    designEngine.setSmoothingEnabled(false);
}

FFTProcessor::~FFTProcessor()
{
    // Generous join timeout: the worker loop polls threadShouldExit every
    // 20 ms and a single design (longest FIR) completes in well under a
    // second, so 5 s leaves ample margin against scheduler stalls.
    if (!stopThread (5000))
    {
        jassertfalse;   // worker did not exit in time — would be a lifetime bug
        // Release 兜底（F-005）：继续阻塞等待线程退出。相比 std::terminate
        // 拖垮宿主，宁可延长退出时间，也绝不让 run() 在 this 销毁后继续
        // 访问成员（use-after-free）。
        while (isThreadRunning())
            wait (50);
    }
}

//==============================================================================
void FFTProcessor::prepare(double sr, int maxBlockSize, int channels)
{
    sampleRate.store(sr, std::memory_order_release);
    preparedChannels.store(std::max(1, channels), std::memory_order_release);

    const int xfadeSamples = std::max(1, maxBlockSize);
    if (xfadeNew.getNumSamples() < xfadeSamples)
    {
        xfadeOld.setSize(std::max(1, numChannels), xfadeSamples);
        xfadeNew.setSize(std::max(1, numChannels), xfadeSamples);
    }

    // Same session parameters: keep history, published design and warm state.
    // Hosts re-call prepareToPlay on transport stop/start.
    if (sr == preparedSampleRate && channels == numChannels && !fftWork.empty())
        return;

    designEpoch.fetch_add(1, std::memory_order_release);

    preparedSampleRate = sr;
    numChannels = std::max(1, channels);

    const int maxL = convolutionFftSize(MaxFirLength, ConvBlockLen);

    activeStates.resize(static_cast<size_t>(numChannels));
    oldStates.resize(static_cast<size_t>(numChannels));
    for (auto* list : { &activeStates, &oldStates })
    {
        for (auto& ch : *list)
        {
            ch.inputBlock.assign(static_cast<size_t>(ConvBlockLen), {});
            ch.pos = 0;
            ch.overlap.assign(static_cast<size_t>(maxL), 0.0);
            ch.outQueue.clear();
            ch.outQueue.reserve(static_cast<size_t>(ConvBlockLen) * 2);
            ch.outRead = 0;
        }
    }

    xfadeOld.setSize(numChannels, xfadeSamples);
    xfadeNew.setSize(numChannels, xfadeSamples);

    fftWork.assign(static_cast<size_t>(maxL), {});
    if (static_cast<int>(warmScratch.size()) < maxL)
        warmScratch.assign(static_cast<size_t>(maxL), {});
    prefixScratch.assign(static_cast<size_t>(ConvBlockLen), 0.0f);
    catchUpInput.assign(static_cast<size_t>(ConvBlockLen), 0.0f);
    catchUpOutput.assign(static_cast<size_t>(ConvBlockLen), 0.0f);
    if (static_cast<int>(catchUpScratch.size()) < maxL)
        catchUpScratch.assign(static_cast<size_t>(maxL), {});

    // Enough history to warm the longest FIR with two extra OLA blocks of
    // margin; power-of-two capacity keeps modulo cheap.
    const int warmBlocks = (MaxFirLength + ConvBlockLen - 1) / ConvBlockLen + 2;
    warmFeedSamples = warmBlocks * ConvBlockLen;
    if (static_cast<int>(warmInput.size()) < warmFeedSamples)
        warmInput.assign(static_cast<size_t>(warmFeedSamples), 0.0f);
    if (static_cast<int>(warmOutput.size()) < warmFeedSamples)
        warmOutput.assign(static_cast<size_t>(warmFeedSamples), 0.0f);

    // Allocate the history ring for the maximum channel count ONCE. The
    // background warm builder may read it concurrently, so prepare() must not
    // reallocate or clear it on ordinary sample-rate/channel-count changes.
    // Samples before historyValidFrom belong to a previous session and are
    // never used for warm-up.
    const int historyCapacity = nextPowerOfTwo(MaxFirLength + ConvBlockLen);
    if (history.capacity() != historyCapacity
        || history.numChannels() < SubEQ::EQNode::MaxChannels)
        history.prepare(SubEQ::EQNode::MaxChannels, historyCapacity);
    historyValidFrom.store(history.publishedCount(), std::memory_order_release);

    activeConvFFTSize = 0;
    activeGroupDelay = 0;
    processedVersion = 0;
    currentOlaBlockIndex = 0;
    reportedActiveLatency.store(0, std::memory_order_release);
    publishedLatency.store(0, std::memory_order_release);
    latencyAckVersion.store(0, std::memory_order_release);
    latencyGuardVersion = 0;
    latencyGuardSamples = 0;
    latencyWaitVersion = 0;
    latencyWaitSamples = 0;
    firstDesignWaitSamples = 0;
    firstDesignWaitVersion = -1;
    startupFadeRemaining = 0;
    crossfadeRemaining = 0;
    fadeConvFFTSize = 0;
    twiddlePrewarmedSize = 0;
    audioTwiddlesPrewarmed = false;

    // Drop all published/active designs and warm states. Moves are O(1);
    // destruction happens on the background thread.
    {
        std::lock_guard<std::mutex> lock(stateMutex);

        auto dropDesign = [&](std::shared_ptr<const FIRState>& p)
        {
            if (p == nullptr)
                return;
            auto& slot = pendingDestroy[pendingDestroyHead];
            if (slot == nullptr)
            {
                slot = std::move(p);
                pendingDestroyHead = (pendingDestroyHead + 1) % NumRetireSlots;
            }
        };
        auto dropWarm = [&](std::shared_ptr<WarmState>& w)
        {
            if (w == nullptr)
                return;
            auto& slot = pendingWarmDestroy[pendingWarmDestroyHead];
            if (slot == nullptr)
            {
                slot = std::move(w);
                pendingWarmDestroyHead = (pendingWarmDestroyHead + 1) % NumWarmRetireSlots;
            }
        };

        dropDesign(currentState);
        dropDesign(localState);
        dropDesign(activeState);
        dropDesign(oldState);
        dropDesign(deferredDesignRetire);
        dropWarm(currentWarm);
        dropWarm(localWarm);
        dropWarm(deferredWarmRetire);
    }

    lastSeenVersion = 0;
    lastSeenWarmVersion = 0;
    stateVersion.fetch_add(1, std::memory_order_release);
    warmVersion.fetch_add(1, std::memory_order_release);

    for (int i = 0; i < NumFirLengthChoices; ++i)
        prewarmTwiddleTable<double>(convolutionFftSize(FirLengthChoices[i], ConvBlockLen));

    requestRedesign();
}

void FFTProcessor::resetConvolution()
{
    crossfadeRemaining = 0;
    fadeConvFFTSize = 0;
    activeConvFFTSize = 0;
    activeGroupDelay = 0;
    processedVersion = 0;
    currentOlaBlockIndex = 0;
    reportedActiveLatency.store(0, std::memory_order_release);
    publishedLatency.store(0, std::memory_order_release);
    latencyGuardVersion = 0;
    latencyGuardSamples = 0;
    latencyWaitVersion = 0;
    latencyWaitSamples = 0;
    firstDesignWaitSamples = 0;
    firstDesignWaitVersion = -1;
    startupFadeRemaining = 0;

    // Invalidate any design that is currently being computed. Without this,
    // a design started before a mode/length change could still publish and be
    // adopted after the clear.
    designEpoch.fetch_add(1, std::memory_order_release);

    auto disposeDesign = [&](std::shared_ptr<const FIRState>& p) -> bool
    {
        if (p == nullptr)
            return true;
        if (retireState(p))
            return true;
        if (deferredDesignRetire == nullptr)
        {
            deferredDesignRetire = std::move(p);
            return true;
        }
        return false;
    };

    // Retire active/old designs. The OLA vectors are swapped out when a warm
    // state is adopted, so there is no large fill here.
    disposeDesign(activeState);
    disposeDesign(oldState);

    if (localState != nullptr && retireState(localState))
        localState.reset();
    if (localWarm != nullptr && retireWarm(localWarm))
        localWarm.reset();

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (currentWarm != nullptr)
        {
            auto& slot = pendingWarmDestroy[pendingWarmDestroyHead];
            if (slot == nullptr)
            {
                slot = std::move(currentWarm);
                pendingWarmDestroyHead = (pendingWarmDestroyHead + 1) % NumWarmRetireSlots;
            }
        }
    }

    stateVersion.fetch_add(1, std::memory_order_release);
    warmVersion.fetch_add(1, std::memory_order_release);
}

void FFTProcessor::clearPublishedState()
{
    resetConvolution();

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (currentState != nullptr)
        {
            auto& slot = pendingDestroy[pendingDestroyHead];
            if (slot == nullptr)
            {
                slot = std::move(currentState);
                pendingDestroyHead = (pendingDestroyHead + 1) % NumRetireSlots;
            }
        }
    }

    stateVersion.fetch_add(1, std::memory_order_release);
}

void FFTProcessor::captureHistory(const float* const* channels, int numChannelsIn,
                                  int numSamples, bool publishNow)
{
    history.writeFrame(channels, numChannelsIn, numSamples, publishNow);
}

void FFTProcessor::setParameterSource(juce::AudioProcessorValueTreeState* source)
{
    parameterSource.store(source, std::memory_order_release);
    startThread();
}

void FFTProcessor::requestRedesign()
{
    // A new request invalidates any design currently being computed. The
    // worker captures this epoch after consuming the request, so its own
    // generation is not invalidated by the request that woke it.
    designEpoch.fetch_add(1, std::memory_order_release);
    redesignRequested.store(true, std::memory_order_release);
    redesignEvent.signal();
}

//==============================================================================
// Background thread: design FIR coefficients, publish, then build a pre-warmed
// OLA state from the input history (shadow convolution).
void FFTProcessor::run()
{
    std::shared_ptr<FIRState> lastDesign;
    int lastDesignEpoch = -1;

    while (!threadShouldExit())
    {
        // Drain retirement rings outside the lock: no large object is ever
        // destroyed while holding stateMutex or on the audio thread.
        std::shared_ptr<const FIRState> toFree[NumRetireSlots];
        std::shared_ptr<WarmState> warmToFree[NumWarmRetireSlots];
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            for (int i = 0; i < NumRetireSlots; ++i)
                toFree[i] = std::move (pendingDestroy[i]);
            for (int i = 0; i < NumWarmRetireSlots; ++i)
                warmToFree[i] = std::move (pendingWarmDestroy[i]);
        }
        for (auto& p : toFree) p.reset();
        for (auto& w : warmToFree) w.reset();

        redesignEvent.wait(20);

        auto* source = parameterSource.load(std::memory_order_acquire);
        if (source == nullptr)
            continue;

        const auto mode = static_cast<EQMode> (
            static_cast<int> (source->getRawParameterValue ("eq_mode")->load()));

        if (warmRebuildRequested.exchange(false, std::memory_order_acquire)
            && lastDesign != nullptr && mode != EQMode::ZeroLatency)
        {
            auto warm = std::make_shared<WarmState>();
            if (buildWarmState(*lastDesign, warm))
            {
                std::shared_ptr<const FIRState> replacedState;
                std::shared_ptr<WarmState> replacedWarm;
                bool publish = false;

                {
                    std::lock_guard<std::mutex> lock (stateMutex);
                    if (lastDesignEpoch != designEpoch.load(std::memory_order_acquire))
                    {
                        // Stale design; a newer request or prepare wins.
                    }
                    else if (lastDesign->version == 0)
                    {
                        // The design was built but deliberately not published
                        // because its warm state was not ready. Publish the
                        // complete package now.
                        const int v = stateVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
                        lastDesign->version = v;
                        warm->version = v;
                        replacedState = std::move (currentState);
                        replacedWarm = std::move (currentWarm);
                        currentState = lastDesign;
                        currentWarm = warm;
                        publishedLatency.store(lastDesign->groupDelay + (ConvBlockLen - 1),
                                               std::memory_order_release);
                        publish = true;
                    }
                    else if (lastDesign->version == stateVersion.load(std::memory_order_acquire))
                    {
                        // Same design, newer warm snapshot.
                        warm->version = lastDesign->version;
                        replacedWarm = std::move (currentWarm);
                        currentWarm = warm;
                        publish = true;
                    }
                }

                if (publish)
                    warmVersion.fetch_add(1, std::memory_order_release);
                replacedState.reset();
                replacedWarm.reset();
            }
            else
            {
                // History not filled yet (or a transient snapshot failure).
                // Retry without rebuilding the FIR coefficients.
                warmRebuildRequested.store(true, std::memory_order_release);
            }
        }

        if (!redesignRequested.exchange(false, std::memory_order_acquire))
            continue;

        if (mode == EQMode::ZeroLatency)
            continue;

        const int firChoice = static_cast<int> (
            source->getRawParameterValue ("fir_length")->load());
        const int firLength = (firChoice >= 0 && firChoice < NumFirLengthChoices)
                                  ? FirLengthChoices[firChoice]
                                  : DefaultFirLength;

        const int epoch = designEpoch.load(std::memory_order_acquire);

        const double sr = sampleRate.load(std::memory_order_acquire);
        if (sr != designSampleRate)
        {
            designEngine.prepare(sr);
            designSampleRate = sr;
        }
        applyNodesToEngine(*source, designEngine);

        auto newState = std::make_shared<FIRState>();
        newState->firLength = firLength;

        // Master gain and bypass are applied after convolution by OutputStage;
        // baking them into the FIR caused a full redesign per gain automation
        // step and delayed bypass.
        auto magnitude = [&](double w) { return designEngine.getNodesMagnitudeLinear(w); };
        if (mode == EQMode::LinearPhase)
        {
            newState->coeffs = designLinearPhaseFIR(magnitude, firLength, newState->groupDelay);
        }
        else
        {
            // 解析统计设计目标的 DC / Nyquist 传输零点阶数，供最小相位设计
            // 剥离奇点：高通在 DC、低通在 Nyquist 各 2 阶，带通各 1 阶，其余
            // 类型无传输零点。Bypass 不再进入 FIR 设计，因此无需旁路归零。
            int dcZeroOrder = 0;
            int nyquistZeroOrder = 0;
            for (int i = 0; i < EQEngine::MaxNodes; ++i)
            {
                const auto& node = designEngine.getNode(i);
                if (!node.isEnabled())
                    continue;

                switch (node.getType())
                {
                    case FilterType::HighPass: dcZeroOrder += 2;      break;
                    case FilterType::LowPass:  nyquistZeroOrder += 2; break;
                    case FilterType::BandPass: ++dcZeroOrder;
                                               ++nyquistZeroOrder;    break;
                    default: break;
                }
            }

            newState->coeffs = designMinimumPhaseFIR(magnitude, firLength, newState->groupDelay,
                                                     dcZeroOrder, nyquistZeroOrder, sr);
        }

        // Type-I linear phase returns nominal-1 taps; use the actual count for
        // convolution sizing, frequency-response construction and tail
        // reporting. Never index coeffs with the nominal parameter value.
        newState->firLength = static_cast<int>(newState->coeffs.size());
        if (newState->firLength <= 0)
            continue;

        newState->convFFTSize = convolutionFftSize(newState->firLength, ConvBlockLen);
        newState->freqCoeffs = buildFirFrequencyCoeffs(newState->coeffs,
                                                       newState->convFFTSize);

        // The design is complete but not published until a matching warm state
        // exists. Keep it as the worker's pending design so a warm-up retry
        // does not recompute the (potentially expensive) FIR coefficients.
        lastDesign = newState;
        lastDesignEpoch = epoch;

        auto warm = std::make_shared<WarmState>();
        if (!buildWarmState(*newState, warm))
        {
            // History not filled yet (first FIR entry) or a transient snapshot
            // failure. Retry the warm-up without a full redesign.
            warmRebuildRequested.store(true, std::memory_order_release);
            continue;
        }

        std::shared_ptr<const FIRState> replacedState;
        std::shared_ptr<WarmState> replacedWarm;
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            if (epoch != designEpoch.load(std::memory_order_acquire))
                continue;   // stale design; shared_ptrs destruct on this thread

            // Single writer rule: every version increment uses fetch_add, so
            // reset/clear events and background publishes can never duplicate
            // a version number.
            const int publishedVersion = stateVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
            newState->version = publishedVersion;
            warm->version = publishedVersion;

            replacedState = std::move (currentState);
            replacedWarm = std::move (currentWarm);
            currentState = newState;
            currentWarm = warm;
            publishedLatency.store(newState->groupDelay + (ConvBlockLen - 1),
                                   std::memory_order_release);
        }
        warmVersion.fetch_add(1, std::memory_order_release);

        // Background destruction only.
        replacedState.reset();
        replacedWarm.reset();
    }
}

//==============================================================================
bool FFTProcessor::retireState(std::shared_ptr<const FIRState>& stateToRetire) const
{
    if (stateToRetire == nullptr)
        return true;

    std::lock_guard<std::mutex> lock(stateMutex);
    auto& slot = pendingDestroy[pendingDestroyHead];
    if (slot != nullptr)
        return false;

    slot = std::move(stateToRetire);
    pendingDestroyHead = (pendingDestroyHead + 1) % NumRetireSlots;
    return true;
}

bool FFTProcessor::retireWarm(std::shared_ptr<WarmState>& warmToRetire) const
{
    if (warmToRetire == nullptr)
        return true;

    std::lock_guard<std::mutex> lock(stateMutex);
    auto& slot = pendingWarmDestroy[pendingWarmDestroyHead];
    if (slot != nullptr)
        return false;

    slot = std::move(warmToRetire);
    pendingWarmDestroyHead = (pendingWarmDestroyHead + 1) % NumWarmRetireSlots;
    return true;
}

std::shared_ptr<const FFTProcessor::FIRState> FFTProcessor::getPublishedStateForAudioThread() const
{
    const int version = stateVersion.load(std::memory_order_acquire);
    if (version != lastSeenVersion)
    {
        std::shared_ptr<const FIRState> next;
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            next = currentState;
        }

        if (next != localState)
        {
            const int oldVersion = lastSeenVersion;
            auto prev = std::move(localState);
            localState = next;
            lastSeenVersion = version;

            if (prev != nullptr && !retireState(prev))
            {
                if (deferredDesignRetire == nullptr)
                {
                    deferredDesignRetire = std::move(prev);
                }
                else
                {
                    // Ring full and deferred slot occupied: keep the previous
                    // design cached rather than destroying it on this thread.
                    localState = std::move(prev);
                    lastSeenVersion = oldVersion;
                }
            }
        }
        else
        {
            lastSeenVersion = version;
        }
    }

    return localState;
}

std::shared_ptr<FFTProcessor::WarmState> FFTProcessor::getWarmStateForAudioThread(int designVersion) const
{
    const int version = warmVersion.load(std::memory_order_acquire);
    if (version != lastSeenWarmVersion)
    {
        std::shared_ptr<WarmState> next;
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            next = currentWarm;
        }

        if (next != localWarm)
        {
            auto prev = std::move(localWarm);
            localWarm = next;
            lastSeenWarmVersion = version;

            if (prev != nullptr && !retireWarm(prev))
            {
                if (deferredWarmRetire == nullptr)
                    deferredWarmRetire = std::move(prev);
                else
                    localWarm = std::move(prev);
            }
        }
        else
        {
            lastSeenWarmVersion = version;
        }
    }

    if (localWarm != nullptr && localWarm->version == designVersion)
        return localWarm;

    return nullptr;
}

//==============================================================================
// Build the exact OLA state at the latest completed OLA block boundary from
// the raw input history. Feeding the last N+M-1 samples through a zero state
// is enough for the FIR memory to forget the zero initial state; two extra
// blocks are added for margin.
bool FFTProcessor::buildWarmState(const FIRState& design, std::shared_ptr<WarmState>& out)
{
    if (out == nullptr || design.freqCoeffs.empty()
        || design.convFFTSize <= 0 || warmFeedSamples <= 0
        || static_cast<int>(warmInput.size()) < warmFeedSamples
        || static_cast<int>(warmOutput.size()) < warmFeedSamples
        || static_cast<int>(warmScratch.size()) < design.convFFTSize)
        return false;

    const int channels = preparedChannels.load(std::memory_order_acquire);
    const int M = ConvBlockLen;
    const int K = (design.firLength + M - 1) / M + 2;   // warm-up blocks
    int latestBlock = static_cast<int>(history.publishedCount()
                                       / static_cast<uint64_t>(M));
    const int firstValidBlock = static_cast<int>(historyValidFrom.load(std::memory_order_acquire)
                                                  / static_cast<uint64_t>(M));

    // Do not build a warm state from samples that belong to a previous
    // sample-rate session; wait until K fresh blocks are available.
    if (latestBlock - firstValidBlock < K)
        return false;

    out->states.resize(static_cast<size_t>(std::max(1, channels)));
    out->version = design.version;
    out->convFFTSize = design.convFFTSize;
    out->groupDelay = design.groupDelay;

    std::vector<float> dummyOutput(static_cast<size_t>(M), 0.0f);

    for (int ch = 0; ch < channels; ++ch)
    {
        OlaChannelState st;
        st.inputBlock.assign(static_cast<size_t>(M), {});
        st.overlap.assign(static_cast<size_t>(design.convFFTSize), 0.0);
        st.outQueue.clear();
        st.outQueue.reserve(static_cast<size_t>(M) * 2);
        st.pos = 0;
        st.outRead = 0;
        out->states[static_cast<size_t>(ch)] = std::move(st);
    }

    // Start K blocks before the latest published boundary (negative blocks
    // are silence) and feed forward. The audio thread may publish new blocks
    // while we work; keep extending to the newest boundary. Each OLA block is
    // much cheaper than real time, so this catches up.
    int64_t cursor = std::max<int64_t>(static_cast<int64_t>(firstValidBlock),
                                       static_cast<int64_t>(latestBlock) - K);
    int fedBlocks = 0;
    const int maxFedBlocks = K + 128;

    while (fedBlocks < maxFedBlocks)
    {
        if (cursor >= static_cast<int64_t>(latestBlock))
        {
            latestBlock = static_cast<int>(history.publishedCount()
                                           / static_cast<uint64_t>(M));
            if (cursor >= static_cast<int64_t>(latestBlock))
                break;
        }

        const int64_t blockStart = cursor * static_cast<int64_t>(M);

        for (int ch = 0; ch < channels; ++ch)
        {
            std::fill(warmInput.begin(), warmInput.begin() + M, 0.0f);

            if (blockStart >= 0)
            {
                const uint64_t end = static_cast<uint64_t>(blockStart)
                                   + static_cast<uint64_t>(M);
                if (!history.snapshot(ch, end, M, warmInput.data()))
                    return false;
            }

            processOlaChannel(out->states[static_cast<size_t>(ch)],
                              warmInput.data(), dummyOutput.data(), M,
                              M, design.convFFTSize, design.freqCoeffs.data(),
                              warmScratch);
        }

        ++cursor;
        ++fedBlocks;

        if ((fedBlocks & 7) == 0)
            latestBlock = static_cast<int>(history.publishedCount()
                                           / static_cast<uint64_t>(M));
    }

    out->blockIndex = static_cast<int>(cursor);
    return true;
}

bool FFTProcessor::catchUpWarmState(WarmState& warm, const FIRState& design, int targetBlock)
{
    const int missing = targetBlock - warm.blockIndex;
    if (missing < 0 || missing > MaxWarmCatchUpBlocks
        || warm.states.empty()
        || static_cast<int>(catchUpInput.size()) < ConvBlockLen
        || static_cast<int>(catchUpOutput.size()) < ConvBlockLen
        || static_cast<int>(catchUpScratch.size()) < design.convFFTSize)
        return false;

    const int channels = std::min(numChannels, static_cast<int>(warm.states.size()));

    // The audio thread is the only writer of the history ring, so the range
    // cannot change during this function. Preflight every snapshot before
    // mutating any warm state, otherwise a partial catch-up would leave the
    // state at a boundary that does not match its blockIndex.
    for (int block = warm.blockIndex; block < targetBlock; ++block)
    {
        const uint64_t end = static_cast<uint64_t>(block + 1) * ConvBlockLen;
        for (int ch = 0; ch < channels; ++ch)
            if (!history.snapshot(ch, end, ConvBlockLen, catchUpInput.data()))
                return false;
    }

    for (int block = warm.blockIndex; block < targetBlock; ++block)
    {
        const uint64_t end = static_cast<uint64_t>(block + 1) * ConvBlockLen;
        for (int ch = 0; ch < channels; ++ch)
        {
            if (!history.snapshot(ch, end, ConvBlockLen, catchUpInput.data()))
                return false;   // should not happen (single writer)

            processOlaChannel(warm.states[static_cast<size_t>(ch)],
                              catchUpInput.data(), catchUpOutput.data(), ConvBlockLen,
                              ConvBlockLen, design.convFFTSize,
                              design.freqCoeffs.data(), catchUpScratch);
        }
    }

    warm.blockIndex = targetBlock;
    return true;
}

//==============================================================================
void FFTProcessor::flushConvolvers()
{
    auto clear = [](std::vector<OlaChannelState>& list)
    {
        for (auto& st : list)
        {
            std::fill(st.inputBlock.begin(), st.inputBlock.end(), std::complex<double>(0.0, 0.0));
            st.pos = 0;
            std::fill(st.overlap.begin(), st.overlap.end(), 0.0);
            st.outQueue.clear();
            st.outRead = 0;
        }
    };

    clear(activeStates);
    clear(oldStates);
    crossfadeRemaining = 0;
    if (oldState != nullptr && !retireState(oldState) && deferredDesignRetire == nullptr)
        deferredDesignRetire = std::move(oldState);
}

//==============================================================================
bool FFTProcessor::process(juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return false;

    // Retry deferred retirements; never destroy large objects on this thread.
    if (deferredDesignRetire != nullptr && retireState(deferredDesignRetire))
        deferredDesignRetire.reset();
    if (deferredWarmRetire != nullptr && retireWarm(deferredWarmRetire))
        deferredWarmRetire.reset();
    if (oldState != nullptr && crossfadeRemaining <= 0
        && retireState(oldState))
        oldState.reset();

    auto published = getPublishedStateForAudioThread();
    if (published == nullptr || published->freqCoeffs.empty())
    {
        // No design yet: leave the buffer untouched. The caller keeps the IIR
        // path alive instead of muting the plugin for the warm-up period.
        return false;
    }

    // One-time lazy twiddle warm-up on the audio thread. prepareToPlay may run
    // on a different thread, and the twiddle cache is thread_local; doing this
    // once here avoids a hidden allocation on the first real FFT block.
    if (!audioTwiddlesPrewarmed)
    {
        for (int i = 0; i < NumFirLengthChoices; ++i)
            prewarmTwiddleTable<double>(convolutionFftSize(FirLengthChoices[i], ConvBlockLen));
        audioTwiddlesPrewarmed = true;
        twiddlePrewarmedSize = published->convFFTSize;
    }

    const int channelCount = std::min(buffer.getNumChannels(), numChannels);
    if (channelCount <= 0)
        return false;

    const uint64_t historyCount = history.publishedCount();
    currentOlaBlockIndex = static_cast<int>(historyCount / ConvBlockLen);
    const int prefix = static_cast<int>(historyCount % ConvBlockLen);

    auto warm = getWarmStateForAudioThread(published->version);

    // A warm state can be adopted at its own exact boundary or advanced by a
    // small number of OLA blocks on the audio thread. The background builder
    // may finish during an audio block, which makes the state one block older
    // than the next block's boundary; bounded catch-up makes that case
    // deterministic without a second background thread.
    bool warmOk = false;
    if (warm != nullptr
        && warm->version == published->version
        && warm->states.size() == activeStates.size()
        && firWarmCatchUpAllowed(warm->blockIndex, currentOlaBlockIndex,
                                 MaxWarmCatchUpBlocks))
    {
        const int missing = currentOlaBlockIndex - warm->blockIndex;
        warmOk = (missing == 0)
              || catchUpWarmState(*warm, *published, currentOlaBlockIndex);
    }

    auto replayPrefix = [&](const FIRState& design)
    {
        if (prefix <= 0)
            return;

        for (int ch = 0; ch < channelCount; ++ch)
        {
            if (!history.snapshot(ch, historyCount, prefix, prefixScratch.data()))
                std::fill(prefixScratch.begin(), prefixScratch.end(), 0.0f);

            processOlaChannel(activeStates[static_cast<size_t>(ch)],
                              prefixScratch.data(), prefixScratch.data(), prefix,
                              ConvBlockLen, design.convFFTSize, design.freqCoeffs.data(), fftWork);
        }
    };

    auto retireWarmSafely = [&](std::shared_ptr<WarmState>& w)
    {
        if (w == nullptr)
            return;
        if (retireWarm(w))
            localWarm.reset();
        // If the ring is full, localWarm still owns this object; the local
        // copy released at scope end is not the last reference.
    };

    // ---- adopt a new design ----
    if (published->version != processedVersion && crossfadeRemaining <= 0)
    {
        const bool latencyChanging = (activeState == nullptr)
                                  || (published->groupDelay != activeGroupDelay);

        // PDC handshake: the host must be told the new latency before the
        // audio switches to it. Wait/guard are measured in samples so the
        // policy does not change with the host block size.
        bool pdcBlocked = false;
        if (latencyChanging)
        {
            const int ack = latencyAckVersion.load(std::memory_order_acquire);
            const double sr = sampleRate.load(std::memory_order_acquire);
            if (ack < published->version)
            {
                if (latencyWaitVersion != published->version)
                {
                    latencyWaitVersion = published->version;
                    latencyWaitSamples = 0;
                }

                latencyWaitSamples += numSamples;
                // Offline/headless hosts may never pump the message thread:
                // after ~1 s of audio, stop gating rather than staying silent.
                if (sr > 0.0 && static_cast<double>(latencyWaitSamples) <= sr)
                    pdcBlocked = true;
            }
            else
            {
                if (latencyGuardVersion != published->version)
                {
                    latencyGuardVersion = published->version;
                    const int guardByTime = (sr > 0.0) ? static_cast<int>(0.05 * sr) : 0;
                    latencyGuardSamples = std::max(2 * numSamples, guardByTime);
                }

                if (latencyGuardSamples > 0)
                {
                    latencyGuardSamples -= numSamples;
                    pdcBlocked = true;
                }
            }
        }

        if (!warmOk)
            warmRebuildRequested.store(true, std::memory_order_release);

        // Last-resort first-design start: if no warm state can be adopted for
        // 2 s and there is no active design, start from zero state and fade in
        // rather than staying silent forever. PDC gating still wins.
        bool fallbackAdopted = false;
        if (!warmOk && activeState == nullptr && !pdcBlocked)
        {
            if (firstDesignWaitVersion != published->version)
            {
                firstDesignWaitVersion = published->version;
                firstDesignWaitSamples = 0;
            }
            firstDesignWaitSamples += numSamples;
            const double sr = sampleRate.load(std::memory_order_acquire);
            if (sr > 0.0 && static_cast<double>(firstDesignWaitSamples) >= 2.0 * sr)
            {
                flushConvolvers();
                activeState = published;
                activeConvFFTSize = published->convFFTSize;
                activeGroupDelay = published->groupDelay;
                processedVersion = published->version;
                reportedActiveLatency.store(published->groupDelay + (ConvBlockLen - 1),
                                            std::memory_order_release);
                startupFadeRemaining = CrossfadeLen;
                firstDesignWaitSamples = 0;
                fallbackAdopted = true;
            }
        }

        if (!fallbackAdopted && warmOk && !pdcBlocked)
        {
            const bool sameDelay = activeState != nullptr
                                && published->groupDelay == activeGroupDelay;

            if (activeState == nullptr)
            {
                activeStates.swap(warm->states);
                crossfadeRemaining = 0;
            }
            else if (sameDelay)
            {
                // Both sides share the same time axis: old state continues,
                // new state starts from the exact warm snapshot.
                oldState = activeState;
                oldStates.swap(activeStates);
                activeStates.swap(warm->states);
                fadeConvFFTSize = activeConvFFTSize;
                crossfadeRemaining = CrossfadeLen;
            }
            else
            {
                // Different group delay: flushing is the only time-aligned
                // option. The old design is retired (never destroyed here).
                if (activeState != nullptr && !retireState(activeState))
                {
                    if (deferredDesignRetire == nullptr)
                        deferredDesignRetire = std::move(activeState);
                    else
                    {
                        warmRebuildRequested.store(true, std::memory_order_release);
                        buffer.clear();
                        return true;   // FIR path owns this block; keep it silent
                    }
                }

                activeStates.swap(warm->states);
                oldState = nullptr;
                crossfadeRemaining = 0;
            }

            activeState = published;
            activeConvFFTSize = published->convFFTSize;
            activeGroupDelay = published->groupDelay;
            processedVersion = published->version;
            reportedActiveLatency.store(published->groupDelay + (ConvBlockLen - 1),
                                        std::memory_order_release);
            firstDesignWaitSamples = 0;

            replayPrefix(*published);
            retireWarmSafely(warm);
        }
    }

    if (activeState == nullptr)
    {
        // Design exists but is not active yet (warm-up/PDC). Leave the raw
        // input untouched so the caller can keep the IIR path running.
        return false;
    }

    // ---- convolve ----
    if (crossfadeRemaining > 0 && numSamples <= xfadeNew.getNumSamples()
        && oldState != nullptr)
    {
        const int startRemaining = crossfadeRemaining;
        const int oldL = fadeConvFFTSize;

        for (int ch = 0; ch < channelCount; ++ch)
        {
            auto* data = buffer.getWritePointer(ch);

            processOlaChannel(oldStates[static_cast<size_t>(ch)], data,
                              xfadeOld.getWritePointer(ch), numSamples,
                              ConvBlockLen, oldL, oldState->freqCoeffs.data(), fftWork);

            processOlaChannel(activeStates[static_cast<size_t>(ch)], data,
                              xfadeNew.getWritePointer(ch), numSamples,
                              ConvBlockLen, activeConvFFTSize,
                              activeState->freqCoeffs.data(), fftWork);

            const float* oldData = xfadeOld.getReadPointer(ch);
            const float* newData = xfadeNew.getReadPointer(ch);

            for (int n = 0; n < numSamples; ++n)
            {
                const float w = firCrossfadeWeight(n, startRemaining, CrossfadeLen);
                data[n] = w * newData[n] + (1.0f - w) * oldData[n];
            }
        }

        crossfadeRemaining = std::max(0, startRemaining - numSamples);
        if (crossfadeRemaining == 0 && oldState != nullptr)
        {
            if (!retireState(oldState) && deferredDesignRetire == nullptr)
                deferredDesignRetire = std::move(oldState);
        }
    }
    else
    {
        if (crossfadeRemaining > 0)
        {
            // Host block larger than the prepared xfade buffer: abandon the
            // fade and switch cleanly rather than reading past the buffer.
            crossfadeRemaining = 0;
            if (oldState != nullptr)
            {
                if (!retireState(oldState) && deferredDesignRetire == nullptr)
                    deferredDesignRetire = std::move(oldState);
            }
        }

        for (int ch = 0; ch < channelCount; ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            processOlaChannel(activeStates[static_cast<size_t>(ch)], data, data,
                              numSamples, ConvBlockLen, activeConvFFTSize,
                              activeState->freqCoeffs.data(), fftWork);
        }

        if (startupFadeRemaining > 0)
        {
            for (int n = 0; n < numSamples; ++n)
            {
                const int rem = startupFadeRemaining - n;
                const float gain = (rem > 0)
                                       ? 1.0f - static_cast<float>(rem)
                                                / static_cast<float>(CrossfadeLen)
                                       : 1.0f;
                for (int ch = 0; ch < channelCount; ++ch)
                    buffer.getWritePointer(ch)[n] *= gain;
            }
            startupFadeRemaining = std::max(0, startupFadeRemaining - numSamples);
        }
    }

    return true;
}

//==============================================================================
int FFTProcessor::getLatencySamples() const
{
    return reportedActiveLatency.load(std::memory_order_acquire);
}

int FFTProcessor::getTailLengthSamples() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (currentState == nullptr)
        return MaxFirLength + ConvBlockLen;
    return currentState->firLength + ConvBlockLen;
}

//==============================================================================
std::shared_ptr<const FFTProcessor::FIRState> FFTProcessor::getFirDesignSnapshot() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return currentState;
}

bool FFTProcessor::computeFirPhaseResponseDegrees(const FIRState& state,
                                                  double normalizedW, double& phaseDeg)
{
    if (state.freqCoeffs.empty() || state.convFFTSize <= 1)
        return false;

    const int fftSize = state.convFFTSize;
    const double twoPi = juce::MathConstants<double>::twoPi;
    const double groupDelay = static_cast<double>(state.groupDelay);

    double binPos = normalizedW * static_cast<double>(fftSize) / twoPi;
    binPos = std::fmod(binPos, static_cast<double>(fftSize));
    if (binPos < 0.0)
        binPos += static_cast<double>(fftSize);

    const double lower = std::floor(binPos);
    const int index0 = static_cast<int>(lower) % fftSize;
    const int index1 = (index0 + 1) % fftSize;
    const double frac = binPos - lower;

    const double w0 = twoPi * lower / static_cast<double>(fftSize);
    const double w1 = twoPi * (lower + 1.0) / static_cast<double>(fftSize);
    const std::complex<double> derotate0(std::cos(w0 * groupDelay), std::sin(w0 * groupDelay));
    const std::complex<double> derotate1(std::cos(w1 * groupDelay), std::sin(w1 * groupDelay));

    const std::complex<double> a0 = state.freqCoeffs[static_cast<size_t>(index0)] * derotate0;
    const std::complex<double> a1 = state.freqCoeffs[static_cast<size_t>(index1)] * derotate1;
    const std::complex<double> response = a0 + (a1 - a0) * frac;

    const double magnitude = std::abs(response);
    if (!std::isfinite(magnitude) || magnitude <= 0.0)
        return false;

    double phase = std::arg(response);
    phase = std::fmod(phase, twoPi);
    if (phase <= -juce::MathConstants<double>::pi)
        phase += twoPi;
    else if (phase > juce::MathConstants<double>::pi)
        phase -= twoPi;

    phaseDeg = phase * 180.0 / juce::MathConstants<double>::pi;
    return true;
}

bool FFTProcessor::computeFirMagnitudeDb(const FIRState& state,
                                         double normalizedW, double& magnitudeDb)
{
    if (state.freqCoeffs.empty() || state.convFFTSize <= 1)
        return false;

    const int fftSize = state.convFFTSize;
    const double twoPi = juce::MathConstants<double>::twoPi;
    double binPos = normalizedW * static_cast<double>(fftSize) / twoPi;
    binPos = std::fmod(binPos, static_cast<double>(fftSize));
    if (binPos < 0.0)
        binPos += static_cast<double>(fftSize);

    const double lower = std::floor(binPos);
    const int index0 = static_cast<int>(lower) % fftSize;
    const int index1 = (index0 + 1) % fftSize;
    const double frac = binPos - lower;
    const std::complex<double> a0 = state.freqCoeffs[static_cast<size_t>(index0)];
    const std::complex<double> a1 = state.freqCoeffs[static_cast<size_t>(index1)];
    const std::complex<double> response = a0 + (a1 - a0) * frac;
    const double magnitude = std::abs(response);

    if (!std::isfinite(magnitude) || magnitude <= 0.0)
        return false;

    magnitudeDb = 20.0 * std::log10(magnitude);
    return true;
}

//==============================================================================
juce::StringArray FFTProcessor::getModeChoices()
{
    return { "Zero Latency", "Minimum Phase", "Linear Phase" };
}

juce::String FFTProcessor::getModeName(EQMode mode)
{
    switch (mode)
    {
        case EQMode::ZeroLatency:   return "Zero Latency";
        case EQMode::MinimumPhase:  return "Minimum Phase";
        case EQMode::LinearPhase:   return "Linear Phase";
    }
    return "Zero Latency";
}

juce::String FFTProcessor::getLatencyText(int latencySamples, double sr)
{
    double ms = (sr > 0.0) ? (latencySamples * 1000.0 / sr) : 0.0;
    return juce::String::formatted("Latency: %.1f ms (%d samples)", ms, latencySamples);
}

} // namespace SubEQ
