/*
  ==============================================================================

    SubEQ_FFTProcessor.cpp
    FIR coefficient design (Linear Phase + Minimum Phase) and FFT overlap-add
    convolution. Coefficient design runs on a background thread with the shared
    radix-2 FFT (SubEQ_DSPMath.h) — JUCE FFT wrapper is unreliable here.

    v0.5.0 T2 switching:
      - captured/committed raw input history ring (audio writer / background reader)
      - background exact-boundary warm synthesis: overlap-save at S=T-M plus one
        final block replay (no O(N/M) feed loop, no audio-thread catch-up)
      - audio adopts only when the candidate boundary equals the current OLA
        boundary; missed candidates are rebuilt, never chased on the audio thread
      - same group delay crossfades over 1024 samples; different group delay is
        gated by the PDC handshake in PluginProcessor
      - no shared_ptr/vector of a large object is ever destroyed on the audio
        thread; superseded states go through the retirement rings

  ==============================================================================
*/

#include "SubEQ_FFTProcessor.h"
#include <chrono>
#include "SubEQ_DSPMath.h"
#include "SubEQ_OlaBank.h"
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
            prepareOlaChannelState(ch, ConvBlockLen, maxL);
    }

    xfadeOld.setSize(numChannels, xfadeSamples);
    xfadeNew.setSize(numChannels, xfadeSamples);

    fftWork.assign(static_cast<size_t>(maxL), {});
    if (static_cast<int>(warmScratch.size()) < maxL)
        warmScratch.assign(static_cast<size_t>(maxL), {});
    prefixScratch.assign(static_cast<size_t>(ConvBlockLen), 0.0f);
    warmCatchUpOutput.assign(static_cast<size_t>(ConvBlockLen), 0.0f);
    maxHostBlockSamples = std::max(1, maxBlockSize);
    currentHostBlockSamples.store(maxHostBlockSamples, std::memory_order_release);

    // Enough history to warm the longest FIR with two extra OLA blocks of
    // margin; power-of-two capacity keeps modulo cheap.
    const int warmBlocks = (MaxFirLength + ConvBlockLen - 1) / ConvBlockLen + 2;
    warmFeedSamples = warmBlocks * ConvBlockLen;
    if (static_cast<int>(warmInput.size()) < warmFeedSamples)
        warmInput.assign(static_cast<size_t>(warmFeedSamples), 0.0f);
    if (static_cast<int>(warmOutput.size()) < ConvBlockLen)
        warmOutput.assign(static_cast<size_t>(ConvBlockLen), 0.0f);

    // Allocate the history ring for the maximum channel count ONCE. The
    // background warm builder may read it concurrently, so prepare() must not
    // reallocate or clear it on ordinary sample-rate/channel-count changes.
    // Samples before historyValidFrom belong to a previous session and are
    // never used for warm-up.
    const int historyCapacity = nextPowerOfTwo(MaxFirLength + ConvBlockLen);
    if (history.capacity() != historyCapacity
        || history.numChannels() < SubEQ::EQNode::MaxChannels)
        history.prepare(SubEQ::EQNode::MaxChannels, historyCapacity);
    historyValidFrom.store(history.capturedCount(), std::memory_order_release);

    activeConvFFTSize = 0;
    activeGroupDelay = 0;
    processedVersion = 0;
    currentOlaBlockIndex = 0;
    reportedActiveLatency.store(0, std::memory_order_release);
    publishedLatency.store(0, std::memory_order_release);
    pendingTargetSample.store(0, std::memory_order_release);
    pendingAbortVersion.store(-1, std::memory_order_release);
    latencyAckVersion.store(0, std::memory_order_release);
    offlineDesignCooldown = 0;
    latencyGuardVersion = 0;
    latencyGuardSamples = 0;
    latencyWaitVersion = 0;
    latencyWaitSamples = 0;
    warmBuildSecondsEWMA = 0.012;
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
    pendingTargetSample.store(0, std::memory_order_release);
    pendingAbortVersion.store(-1, std::memory_order_release);
    latencyGuardVersion = 0;
    latencyGuardSamples = 0;
    latencyWaitVersion = 0;
    latencyWaitSamples = 0;
    offlineDesignCooldown = 0;
    warmBuildSecondsEWMA = 0.012;

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
        // Audio-thread path: never block on the background publisher. If the
        // lock is busy the warm is left for the next publish/retirement;
        // correctness does not depend on clearing it synchronously.
        std::unique_lock<std::mutex> lock (stateMutex, std::try_to_lock);
        if (lock.owns_lock() && currentWarm != nullptr)
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
    currentHostBlockSamples.store(std::max(1, numSamples), std::memory_order_release);
    history.writeFrame(channels, numChannelsIn, numSamples, publishNow);
}

void FFTProcessor::setParameterSource(juce::AudioProcessorValueTreeState* source)
{
    parameterSource.store(source, std::memory_order_release);
    startThread();
}

void FFTProcessor::requestRedesign()
{
    // Do not bump designEpoch here: continuous parameter automation must not
    // invalidate every design in flight. Structural invalidation goes through
    // invalidateDesigns()/prepare().
    redesignRequested.store(true, std::memory_order_release);
    redesignEvent.signal();
}

void FFTProcessor::requestWarmRebuild()
{
    warmRebuildRequested.store(true, std::memory_order_release);
    redesignEvent.signal();
}

void FFTProcessor::invalidateDesigns()
{
    designEpoch.fetch_add(1, std::memory_order_release);
    offlineDesignCooldown = 0;
    requestRedesign();
}

//==============================================================================
// Background thread: design FIR coefficients, publish, then build a pre-warmed
// OLA state from the input history (shadow convolution).
void FFTProcessor::run()
{
    std::shared_ptr<FIRState> lastDesign;
    int lastDesignEpoch = -1;
    uint64_t lastDesignTarget = 0;

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

        // Offline rendering builds designs/warm states synchronously on the
        // audio thread; keep the worker out of the way.
        if (nonRealtime.load(std::memory_order_acquire))
            continue;

        if (redesignRequested.load(std::memory_order_acquire))
            warmRebuildRequested.store(false, std::memory_order_release);

        // Retry a warm for an already-published design whose target is still
        // in the future. The design metadata was published early so the audio
        // coordinator could start its latency transition in time.
        if (warmRebuildRequested.exchange(false, std::memory_order_acquire)
            && lastDesign != nullptr && mode != EQMode::ZeroLatency
            && lastDesign->version != 0
            && lastDesign->version == stateVersion.load(std::memory_order_acquire))
        {
            // If the previous target was missed, choose a fresh future host
            // block for the SAME design instead of rebuilding coefficients.
            if (lastDesignTarget <= history.publishedCount())
            {
                const int hostBlock = std::max(1, currentHostBlockSamples.load(std::memory_order_acquire));
                const double sr = sampleRate.load(std::memory_order_acquire);
                const uint64_t lead = warmLeadSamples(sr, hostBlock, warmBuildSecondsEWMA);
                lastDesignTarget = planWarmTarget(history.capturedCount(), lead,
                                                  hostBlock, ConvBlockLen).targetSample;
                pendingTargetSample.store(lastDesignTarget, std::memory_order_release);
            }

            auto warm = std::make_shared<WarmState>();
            if (buildWarmState(*lastDesign, warm, lastDesignEpoch, lastDesignTarget))
            {
                std::shared_ptr<WarmState> replacedWarm;
                bool publish = false;
                {
                    std::lock_guard<std::mutex> lock (stateMutex);
                    if (lastDesignEpoch == designEpoch.load(std::memory_order_acquire)
                        && lastDesign->version == stateVersion.load(std::memory_order_acquire))
                    {
                        warm->version = lastDesign->version;
                        replacedWarm = std::move (currentWarm);
                        currentWarm = warm;
                        pendingTargetSample.store(lastDesignTarget,
                                                  std::memory_order_release);
                        pendingAbortVersion.store(-1, std::memory_order_release);
                        publish = true;
                    }
                }
                if (publish)
                    warmVersion.fetch_add(1, std::memory_order_release);
                replacedWarm.reset();
            }
            else if (lastDesignEpoch != designEpoch.load(std::memory_order_acquire))
            {
                lastDesign.reset();
                lastDesignEpoch = -1;
            }
            else
            {
                warmRebuildRequested.store(true, std::memory_order_release);
            }
        }

        if (!redesignRequested.exchange(false, std::memory_order_acquire))
            continue;

        if (mode == EQMode::ZeroLatency)
            continue;

        int epoch = 0;
        auto newState = buildDesignSnapshot(epoch);
        if (newState == nullptr)
            continue;

        const int hostBlock = std::max(1, currentHostBlockSamples.load(std::memory_order_acquire));
        const double sr = sampleRate.load(std::memory_order_acquire);
        const uint64_t lead = warmLeadSamples(sr, hostBlock, warmBuildSecondsEWMA);
        const WarmTarget target = planWarmTarget(history.capturedCount(), lead,
                                                 hostBlock, ConvBlockLen);

        // Phase 1: publish design + target metadata immediately. The audio
        // coordinator can now schedule the latency transition with a real
        // lead; adoption still requires the matching warm state.
        std::shared_ptr<const FIRState> replacedState;
        int publishedVersion = 0;
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            if (epoch != designEpoch.load(std::memory_order_acquire))
                continue;

            publishedVersion = stateVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
            newState->version = publishedVersion;
            replacedState = std::move (currentState);
            currentState = newState;
            publishedLatency.store(newState->groupDelay + (ConvBlockLen - 1),
                                   std::memory_order_release);
            pendingTargetSample.store(target.targetSample, std::memory_order_release);
            pendingAbortVersion.store(-1, std::memory_order_release);
        }
        replacedState.reset();

        lastDesign = newState;
        lastDesignEpoch = epoch;
        lastDesignTarget = target.targetSample;

        // Phase 2: build the exact warm state for the published target.
        auto warm = std::make_shared<WarmState>();
        if (!buildWarmState(*newState, warm, epoch, target.targetSample))
        {
            if (epoch != designEpoch.load(std::memory_order_acquire))
            {
                lastDesign.reset();
                lastDesignEpoch = -1;
            }
            else
            {
                warmRebuildRequested.store(true, std::memory_order_release);
            }
            continue;
        }

        std::shared_ptr<WarmState> replacedWarm;
        bool publishWarm = false;
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            if (publishedVersion == stateVersion.load(std::memory_order_acquire)
                && epoch == designEpoch.load(std::memory_order_acquire))
            {
                warm->version = publishedVersion;
                replacedWarm = std::move (currentWarm);
                currentWarm = warm;
                pendingTargetSample.store(target.targetSample,
                                          std::memory_order_release);
                pendingAbortVersion.store(-1, std::memory_order_release);
                publishWarm = true;
            }
        }
        if (publishWarm)
            warmVersion.fetch_add(1, std::memory_order_release);
        replacedWarm.reset();
    }
}

//==============================================================================
bool FFTProcessor::retireState(std::shared_ptr<const FIRState>& stateToRetire) const
{
    if (stateToRetire == nullptr)
        return true;

    std::unique_lock<std::mutex> lock(stateMutex, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
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

    std::unique_lock<std::mutex> lock(stateMutex, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
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
            std::unique_lock<std::mutex> lock (stateMutex, std::try_to_lock);
            if (!lock.owns_lock())
                return localState;   // never block the audio thread on GUI/worker
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
            std::unique_lock<std::mutex> lock (stateMutex, std::try_to_lock);
            if (!lock.owns_lock())
                return localWarm;    // retry next block
            next = currentWarm;
        }

        if (next != localWarm)
        {
            const int oldVersion = lastSeenWarmVersion;
            auto prev = std::move(localWarm);
            localWarm = next;
            lastSeenWarmVersion = version;

            if (prev != nullptr && !retireWarm(prev))
            {
                if (deferredWarmRetire == nullptr)
                {
                    deferredWarmRetire = std::move(prev);
                }
                else
                {
                    localWarm = std::move(prev);
                    lastSeenWarmVersion = oldVersion;
                }
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
// Build FIR coefficients from the current APVTS snapshot. Safe to call from
// the background worker or, in offline rendering, from the audio thread; the
// designEngine is serialized by designMutex. Returns nullptr for ZL / failure.
std::shared_ptr<FFTProcessor::FIRState> FFTProcessor::buildDesignSnapshot(int& outEpoch)
{
    auto* source = parameterSource.load(std::memory_order_acquire);
    if (source == nullptr)
        return nullptr;

    const auto mode = static_cast<EQMode> (
        static_cast<int> (source->getRawParameterValue ("eq_mode")->load()));
    if (mode == EQMode::ZeroLatency)
        return nullptr;

    const int firChoice = static_cast<int> (
        source->getRawParameterValue ("fir_length")->load());
    const int firLength = (firChoice >= 0 && firChoice < NumFirLengthChoices)
                              ? FirLengthChoices[firChoice]
                              : DefaultFirLength;

    outEpoch = designEpoch.load(std::memory_order_acquire);
    const double sr = sampleRate.load(std::memory_order_acquire);

    std::lock_guard<std::mutex> lock (designMutex);

    if (sr != designSampleRate)
    {
        designEngine.prepare(sr);
        designSampleRate = sr;
    }
    applyNodesToEngine(*source, designEngine);

    auto newState = std::make_shared<FIRState>();
    newState->firLength = firLength;

    // Master gain and bypass are applied after convolution by OutputStage;
    // baking them into the FIR caused a full redesign per gain automation step
    // and delayed bypass.
    auto magnitude = [&](double w) { return designEngine.getNodesMagnitudeLinear(w); };
    if (mode == EQMode::LinearPhase)
    {
        newState->coeffs = designLinearPhaseFIR(magnitude, firLength, newState->groupDelay);
    }
    else
    {
        // Analytic DC / Nyquist transmission-zero orders for the minimum-phase
        // cepstral design; bypass is not part of the design target.
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

    newState->firLength = static_cast<int>(newState->coeffs.size());
    if (newState->firLength <= 0)
        return nullptr;

    newState->convFFTSize = convolutionFftSize(newState->firLength, ConvBlockLen);
    newState->freqCoeffs = buildFirFrequencyCoeffs(newState->coeffs,
                                                   newState->convFFTSize);
    return newState;
}

//==============================================================================
// Build the exact OLA state at a future OLA boundary from raw history.
//
// For target boundary T (= k*M), the accumulator at S=T-M depends only on the
// last N input samples before S. One overlap-save convolution at S gives that
// accumulator exactly; replaying the single block [S,T) then rebuilds the
// pending output FIFO and advances the accumulator to T. This is equivalent to
// feeding from zero for the last N samples, but costs 4 FFTs per channel
// instead of O(N/M) blocks, and never runs on the audio thread.
bool FFTProcessor::buildWarmState(const FIRState& design, std::shared_ptr<WarmState>& out,
                                  int expectedEpoch, uint64_t targetSample)
{
    if (out == nullptr || design.freqCoeffs.empty()
        || design.convFFTSize <= 0 || design.firLength <= 0
        || static_cast<int>(warmInput.size()) < std::max(design.firLength, ConvBlockLen)
        || static_cast<int>(warmOutput.size()) < ConvBlockLen
        || static_cast<int>(warmScratch.size()) < design.convFFTSize)
        return false;

    const auto abortRequested = [&]() noexcept
    {
        return threadShouldExit()
            || nonRealtime.load(std::memory_order_acquire)
            || designEpoch.load(std::memory_order_acquire) != expectedEpoch;
    };

    std::lock_guard<std::mutex> warmBuildLock(warmBuildMutex);
    const int channels = preparedChannels.load(std::memory_order_acquire);
    const int M = ConvBlockLen;
    const int N = design.firLength;
    const int L = design.convFFTSize;
    const uint64_t validFrom = historyValidFrom.load(std::memory_order_acquire);

    out->states.resize(static_cast<size_t>(std::max(1, channels)));
    out->version = design.version;
    out->convFFTSize = L;
    out->groupDelay = design.groupDelay;
    out->targetSample = targetSample;

    const int maxL = convolutionFftSize(MaxFirLength, ConvBlockLen);
    for (int ch = 0; ch < channels; ++ch)
        prepareOlaChannelState(out->states[static_cast<size_t>(ch)], M, maxL);

    double workSeconds = 0.0;

    const uint64_t boundary = targetSample
                            - (targetSample % static_cast<uint64_t>(M));
    const int prefix = static_cast<int>(targetSample - boundary);

    if (boundary >= static_cast<uint64_t>(M))
    {
        const uint64_t S = boundary - static_cast<uint64_t>(M);

        // The overlap at S only needs history through S, one block before the
        // OLA boundary. Replay [S, boundary) at the boundary. Only the final
        // prefix [boundary, target) depends on samples at the host block start.
        while (history.capturedCount() < S)
        {
            if (abortRequested())
                return false;
            juce::Thread::sleep(1);
        }

        auto t0 = std::chrono::steady_clock::now();
        for (int ch = 0; ch < channels; ++ch)
        {
            if (!olaSynthesizeOverlap(history, ch, S, validFrom, N, L, M,
                                      design.freqCoeffs.data(),
                                      out->states[static_cast<size_t>(ch)],
                                      warmInput, warmScratch))
                return false;
        }
        workSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();

        while (history.capturedCount() < boundary)
        {
            if (abortRequested())
                return false;
            juce::Thread::sleep(1);
        }

        t0 = std::chrono::steady_clock::now();
        for (int ch = 0; ch < channels; ++ch)
        {
            if (!olaReplayBlock(history, ch, S, validFrom, L, M,
                                design.freqCoeffs.data(),
                                out->states[static_cast<size_t>(ch)],
                                warmInput, warmOutput, warmScratch))
                return false;
        }
        workSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
    }

    if (prefix > 0)
    {
        while (history.capturedCount() < targetSample)
        {
            if (abortRequested())
                return false;
            juce::Thread::sleep(1);
        }

        const auto t0 = std::chrono::steady_clock::now();
        for (int ch = 0; ch < channels; ++ch)
        {
            if (!history.snapshotPadded(ch, boundary, prefix, validFrom,
                                        warmInput.data()))
                return false;
            processOlaChannel(out->states[static_cast<size_t>(ch)],
                              warmInput.data(), warmOutput.data(), prefix,
                              M, L, design.freqCoeffs.data(), warmScratch);
        }
        workSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
    }

    warmBuildSecondsEWMA = 0.75 * warmBuildSecondsEWMA + 0.25 * workSeconds;
    return true;
}

//==============================================================================
// Offline path: build the design and an exact warm state at the current
// committed boundary. This runs on the audio thread only when the host marks
// the processor non-realtime; there is no deadline to miss.
bool FFTProcessor::buildWarmAtCurrentBoundary(const FIRState& design,
                                               std::shared_ptr<WarmState>& out)
{
    const int M = ConvBlockLen;
    const uint64_t committed = history.publishedCount();

    const int channels = preparedChannels.load(std::memory_order_acquire);
    const int N = design.firLength;
    const int L = design.convFFTSize;
    if (out == nullptr || N <= 0 || L < M
        || static_cast<int>(warmInput.size()) < std::max(N, M)
        || static_cast<int>(warmOutput.size()) < M
        || static_cast<int>(warmScratch.size()) < L)
        return false;

    const int maxL = convolutionFftSize(MaxFirLength, ConvBlockLen);
    out->states.resize(static_cast<size_t>(std::max(1, channels)));
    out->version = design.version;
    out->convFFTSize = L;
    out->groupDelay = design.groupDelay;
    out->targetSample = committed;

    for (int ch = 0; ch < channels; ++ch)
        prepareOlaChannelState(out->states[static_cast<size_t>(ch)], M, maxL);

    const uint64_t validFrom = historyValidFrom.load(std::memory_order_acquire);

    for (int ch = 0; ch < channels; ++ch)
    {
        if (!olaSynthesizeAt(history, ch, committed, validFrom,
                             N, L, M, design.freqCoeffs.data(),
                             out->states[static_cast<size_t>(ch)],
                             warmInput, warmOutput, warmScratch))
            return false;
    }

    return true;
}

bool FFTProcessor::ensureOfflineReady()
{
    if (!nonRealtime.load(std::memory_order_acquire))
        return false;

    // The live worker may still be finishing a warm build when the host flips
    // to offline. Never share its scratch buffers; skip this block and retry.
    std::unique_lock<std::mutex> warmBuildLock(warmBuildMutex, std::try_to_lock);
    if (!warmBuildLock.owns_lock())
        return false;

    const uint64_t committed = history.publishedCount();

    auto published = getPublishedStateForAudioThread();

    const bool needDesign = published == nullptr
                         || redesignRequested.load(std::memory_order_acquire);
    if (needDesign)
    {
        if (offlineDesignCooldown > 0)
        {
            --offlineDesignCooldown;
        }
        else
        {
            int epoch = 0;
            auto design = buildDesignSnapshot(epoch);
            if (design != nullptr && epoch == designEpoch.load(std::memory_order_acquire))
            {
                auto warm = std::make_shared<WarmState>();
                if (buildWarmAtCurrentBoundary(*design, warm))
                {
                    std::shared_ptr<const FIRState> replacedState;
                    std::shared_ptr<WarmState> replacedWarm;
                    {
                        std::lock_guard<std::mutex> lock (stateMutex);
                        if (epoch == designEpoch.load(std::memory_order_acquire))
                        {
                            const int v = stateVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
                            design->version = v;
                            warm->version = v;
                            replacedState = std::move (currentState);
                            replacedWarm = std::move (currentWarm);
                            currentState = design;
                            currentWarm = warm;
                            publishedLatency.store(design->groupDelay + (ConvBlockLen - 1),
                                                   std::memory_order_release);
                            pendingTargetSample.store(warm->targetSample,
                                                      std::memory_order_release);
                            redesignRequested.store(false, std::memory_order_release);
                            warmVersion.fetch_add(1, std::memory_order_release);
                            offlineDesignCooldown = 1;
                        }
                    }
                    // Offline: a one-off large-object release on this thread is
                    // acceptable (no real-time deadline); steady-state uses the
                    // retirement rings.
                    replacedState.reset();
                    replacedWarm.reset();
                    return true;
                }
            }
            offlineDesignCooldown = 8;
        }
    }

    if (published == nullptr)
        return false;

    // Once the published design is the one already driving the audio path,
    // there is no pending adoption; do not rebuild a warm state every block.
    if (published->version == processedVersion)
        return false;

    auto warm = getWarmStateForAudioThread(published->version);
    if (warm == nullptr || warm->version != published->version
        || warm->targetSample != committed)
    {
        auto rebuilt = std::make_shared<WarmState>();
        if (!buildWarmAtCurrentBoundary(*published, rebuilt))
            return false;

        std::shared_ptr<WarmState> replacedWarm;
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            if (published->version == stateVersion.load(std::memory_order_acquire))
            {
                rebuilt->version = published->version;
                replacedWarm = std::move (currentWarm);
                currentWarm = rebuilt;
                pendingTargetSample.store(rebuilt->targetSample,
                                          std::memory_order_release);
                warmVersion.fetch_add(1, std::memory_order_release);
            }
        }
        replacedWarm.reset();
        return true;
    }

    return false;
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

    if (nonRealtime.load(std::memory_order_acquire))
        ensureOfflineReady();

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

    auto warm = getWarmStateForAudioThread(published->version);
    const bool adoptionAborted = pendingAbortVersion.load(std::memory_order_acquire)
                                 == published->version;

    // A warm candidate is built at a target host block. Normally it is ready
    // exactly at that block; if the worker finished just after the boundary,
    // advance it by at most one OLA block on the audio thread (bounded cost:
    // at most one extra FFT). Larger gaps are rejected and rebuilt.
    bool warmOk = warm != nullptr
        && warm->version == published->version
        && warm->convFFTSize == published->convFFTSize
        && warm->states.size() == activeStates.size()
        && warm->targetSample <= historyCount;
    int warmGap = 0;
    if (warmOk)
    {
        warmGap = static_cast<int>(historyCount - warm->targetSample);
        // The worker may just miss the target when the host block is large.
        // Catch up at most the current host block (bounded by the caller's
        // block size); larger gaps are rebuilt rather than chased.
        const int catchUpLimit = std::max(ConvBlockLen, numSamples);
        if (warmGap > catchUpLimit)
            warmOk = false;
    }

    if (warmOk && warmGap > 0)
    {
        for (int ch = 0; ch < channelCount; ++ch)
        {
            int offset = 0;
            while (offset < warmGap)
            {
                const int chunk = std::min(ConvBlockLen, warmGap - offset);
                const uint64_t end = warm->targetSample
                                   + static_cast<uint64_t>(offset + chunk);
                if (!history.snapshot(ch, end, chunk, prefixScratch.data()))
                {
                    warmOk = false;
                    break;
                }

                processOlaChannel(warm->states[static_cast<size_t>(ch)],
                                  prefixScratch.data(), warmCatchUpOutput.data(),
                                  chunk, ConvBlockLen, published->convFFTSize,
                                  published->freqCoeffs.data(), fftWork);
                offset += chunk;
            }

            if (!warmOk)
                break;
        }

        if (warmOk)
            warm->targetSample = historyCount;
    }

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
    if (published->version != processedVersion && crossfadeRemaining <= 0
        && !adoptionAborted)
    {
        const bool latencyChanging = (activeState == nullptr)
                                  || (published->groupDelay != activeGroupDelay);

        // PDC handshake: the host must be told the new latency before the
        // audio switches to it. Wait/guard are measured in samples so the
        // policy does not change with the host block size.
        bool pdcBlocked = false;
        if (latencyChanging && !nonRealtime.load(std::memory_order_acquire))
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
                // Offline/headless hosts may never pump the message thread.
                // Never switch a latency the host has not acknowledged: keep
                // the old path and revert the host-facing latency instead.
                // If the host later acknowledges, restore the new latency and
                // resume the normal guard/switch path.
                pdcBlocked = true;
                if (sr > 0.0 && static_cast<double>(latencyWaitSamples) >= sr)
                    publishedLatency.store(reportedActiveLatency.load(std::memory_order_acquire),
                                           std::memory_order_release);
            }
            else
            {
                // The host has acknowledged this version (or a later one):
                // restore the published target and run the sample-based guard.
                publishedLatency.store(published->groupDelay + (ConvBlockLen - 1),
                                       std::memory_order_release);
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
        {
            const bool haveCandidate = warm != nullptr
                && warm->version == published->version
                && warm->convFFTSize == published->convFFTSize
                && warm->states.size() == activeStates.size();
            const bool missed = haveCandidate
                && warm->targetSample < historyCount;
            // A candidate for a future host block is valid: wait for it.
            // Rebuild only when absent, malformed, or already behind the
            // playhead. No gate/catch-up is involved here.
            if (!haveCandidate || missed)
                warmRebuildRequested.store(true, std::memory_order_release);
        }

        // No zero-state fallback: a design is published only together with a
        // matching warm state. If the candidate's boundary was missed, keep
        // the active design (or the delayed IIR bridge) and rebuild the warm
        // for a new future boundary. Never start a FIR from zero history.
        if (warmOk && !pdcBlocked)
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
            pendingTargetSample.store(0, std::memory_order_release);
            pendingAbortVersion.store(-1, std::memory_order_release);

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

void FFTProcessor::abortPendingPlan() noexcept
{
    // The published plan is no longer wanted. Keep the active design (if any)
    // playing; the audio thread ignores the aborted version and the worker
    // will publish a fresh plan on the next request.
    pendingAbortVersion.store(stateVersion.load(std::memory_order_acquire),
                              std::memory_order_release);
    pendingTargetSample.store(0, std::memory_order_release);
    requestRedesign();
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
