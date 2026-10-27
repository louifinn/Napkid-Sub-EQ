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

    for (auto& state : designSlotStates)
        state.store(DesignSlotFree, std::memory_order_relaxed);
    for (auto& state : warmSlotStates)
        state.store(WarmSlotFree, std::memory_order_relaxed);
    planMeta.slot.store (-1, std::memory_order_relaxed);
    planMeta.warmSlot.store (-1, std::memory_order_relaxed);
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
// Design ring / PlanMeta seqlock
//==============================================================================

void FFTProcessor::writePlanMeta(int slot, int warmSlot, int version, int latency,
                                 uint64_t target, int epoch) noexcept
{
    std::lock_guard<std::mutex> lock (planMetaMutex);

    // A slower producer (worker vs offline) must not overwrite a newer plan
    // with an older design or a plan from an older reset epoch. Equal versions
    // are warm updates and are allowed.
    const int publishedEpoch = planMeta.epoch.load (std::memory_order_relaxed);
    if (epoch < publishedEpoch)
        return;
    if (epoch == publishedEpoch
        && version < planMeta.version.load (std::memory_order_relaxed))
        return;

    const uint32_t seq = planMetaSeq.load (std::memory_order_relaxed);
    planMetaSeq.store (seq + 1, std::memory_order_release);   // odd: writer active
    std::atomic_thread_fence (std::memory_order_release);     // fields before even seq

    planMeta.slot.store (slot, std::memory_order_relaxed);
    planMeta.warmSlot.store (warmSlot, std::memory_order_relaxed);
    planMeta.version.store (version, std::memory_order_relaxed);
    planMeta.latency.store (latency, std::memory_order_relaxed);
    planMeta.target.store (target, std::memory_order_relaxed);
    planMeta.epoch.store (epoch, std::memory_order_relaxed);

    std::atomic_thread_fence (std::memory_order_release);
    planMetaSeq.store (seq + 2, std::memory_order_release);   // even: stable
}

bool FFTProcessor::readPlanMeta(PlanMeta& out) const noexcept
{
    // A torn read is transient (the writer holds the odd sequence for a few
    // instructions). Retry a few times so the audio thread does not mistake a
    // torn record for "no plan" and abort a live PDC transition.
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t seq1 = planMetaSeq.load (std::memory_order_acquire);
        if ((seq1 & 1u) != 0)
            continue;

        out.slot = planMeta.slot.load (std::memory_order_relaxed);
        out.warmSlot = planMeta.warmSlot.load (std::memory_order_relaxed);
        out.version = planMeta.version.load (std::memory_order_relaxed);
        out.latency = planMeta.latency.load (std::memory_order_relaxed);
        out.target = planMeta.target.load (std::memory_order_relaxed);
        out.epoch = planMeta.epoch.load (std::memory_order_relaxed);

        std::atomic_thread_fence (std::memory_order_acquire);
        const uint32_t seq2 = planMetaSeq.load (std::memory_order_acquire);
        if (seq1 == seq2 && (seq2 & 1u) == 0)
            return true;
    }
    return false;
}

int FFTProcessor::reserveFreeDesignSlot() noexcept
{
    PlanMeta meta;
    if (! readPlanMeta (meta))
        return -1;   // torn publish: retry later, never reuse the live slot

    const int publishedSlot = meta.slot;
    const uint32_t mask = audioDesignMask.load (std::memory_order_acquire);

    for (int i = 0; i < NumDesignSlots; ++i)
    {
        if (i == publishedSlot || (mask & (1u << i)) != 0)
            continue;

        int state = designSlotStates[i].load (std::memory_order_acquire);
        if (state != DesignSlotFree && state != DesignSlotReady)
            continue;

        // Reserve atomically so the worker and the offline renderer can never
        // build into the same slot.
        if (designSlotStates[i].compare_exchange_strong (
                state, DesignSlotReserved,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return i;
    }
    return -1;
}

void FFTProcessor::updateAudioDesignMask() noexcept
{
    uint32_t mask = 0;
    if (localDesignSlot >= 0)
        mask |= 1u << localDesignSlot;
    if (activeDesignSlot >= 0)
        mask |= 1u << activeDesignSlot;
    if (oldDesignSlot >= 0)
        mask |= 1u << oldDesignSlot;
    audioDesignMask.store (mask, std::memory_order_release);
}

void FFTProcessor::releaseDesignSlot(int slot) noexcept
{
    if (slot >= 0 && slot < NumDesignSlots
        && slot != activeDesignSlot && slot != oldDesignSlot
        && slot != localDesignSlot)
    {
        designSlotStates[slot].store (DesignSlotFree, std::memory_order_release);
    }
}

const FFTProcessor::FIRState* FFTProcessor::getPublishedStateForAudioThread()
{
    const int minimumVersion = minimumPlanVersion.load (std::memory_order_acquire);

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        PlanMeta meta;
        if (! readPlanMeta (meta))
        {
            // Torn writer window: do NOT release the current pin, or the
            // producer could recycle the slot while the meta still names it.
            return localDesignSlot >= 0 ? &designSlots[localDesignSlot].state
                                        : activeState;
        }

        if (meta.slot < 0 || meta.slot >= NumDesignSlots
            || meta.version < minimumVersion
            || meta.epoch != designEpoch.load (std::memory_order_acquire))
        {
            if (localDesignSlot >= 0
                && localDesignSlot != activeDesignSlot
                && localDesignSlot != oldDesignSlot)
            {
                const int oldLocal = localDesignSlot;
                localDesignSlot = -1;
                updateAudioDesignMask();
                releaseDesignSlot (oldLocal);
            }
            return nullptr;
        }

        const int slot = meta.slot;
        if (localDesignSlot == slot)
            return &designSlots[slot].state;

        int expected = DesignSlotReady;
        if (! designSlotStates[slot].compare_exchange_strong (
                expected, DesignSlotInUse,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            // The meta may point at a slot whose Ready store has not landed
            // yet (Ready is published last). Keep the currently active FIR
            // for this block instead of dropping to the IIR bridge.
            if (activeState != nullptr)
                return activeState;
            continue;   // producer took/rewrote it; re-read the meta
        }

        // Re-validate after pinning: a newer meta may have landed between the
        // seqlock read and the CAS, or the slot may hold a different version.
        PlanMeta check;
        if (! readPlanMeta (check) || check.slot != slot
            || check.version != meta.version
            || designSlots[slot].state.version != meta.version)
        {
            designSlotStates[slot].store (DesignSlotReady, std::memory_order_release);
            if (activeState != nullptr)
                return activeState;
            continue;
        }

        const int oldLocal = localDesignSlot;
        localDesignSlot = slot;
        updateAudioDesignMask();
        if (oldLocal >= 0 && oldLocal != activeDesignSlot && oldLocal != oldDesignSlot)
            releaseDesignSlot (oldLocal);

        return &designSlots[slot].state;
    }

    return activeState;
}

int FFTProcessor::reserveFreeWarmSlot() noexcept
{
    PlanMeta meta;
    if (! readPlanMeta (meta))
        return -1;

    const int publishedSlot = meta.warmSlot;
    const uint32_t mask = audioWarmMask.load (std::memory_order_acquire);

    for (int i = 0; i < NumWarmSlots; ++i)
    {
        if (i == publishedSlot || (mask & (1u << i)) != 0)
            continue;

        int state = warmSlotStates[i].load (std::memory_order_acquire);
        if (state != WarmSlotFree && state != WarmSlotReady)
            continue;

        if (warmSlotStates[i].compare_exchange_strong (
                state, WarmSlotReserved,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return i;
    }
    return -1;
}

void FFTProcessor::updateAudioWarmMask() noexcept
{
    const uint32_t mask = localWarmSlot >= 0 ? (1u << localWarmSlot) : 0u;
    audioWarmMask.store (mask, std::memory_order_release);
}

void FFTProcessor::releaseWarmSlot(int slot) noexcept
{
    if (slot >= 0 && slot < NumWarmSlots && slot != localWarmSlot)
        warmSlotStates[slot].store (WarmSlotFree, std::memory_order_release);
}

FFTProcessor::WarmState* FFTProcessor::getWarmStateForAudioThread(int designVersion)
{
    PlanMeta meta;
    if (! readPlanMeta (meta))
    {
        // Torn writer window: keep the pinned warm and its progressed target.
        return localWarmSlot >= 0 ? &warmSlots[localWarmSlot].state : nullptr;
    }

    if (meta.warmSlot < 0 || meta.warmSlot >= NumWarmSlots
        || meta.version != designVersion
        || meta.version < minimumPlanVersion.load (std::memory_order_acquire)
        || meta.epoch != designEpoch.load (std::memory_order_acquire))
    {
        return nullptr;
    }

    const int slot = meta.warmSlot;
    if (localWarmSlot == slot)
        return &warmSlots[slot].state;

    int expected = WarmSlotReady;
    if (! warmSlotStates[slot].compare_exchange_strong (
            expected, WarmSlotInUse,
            std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return nullptr;
    }

    PlanMeta check;
    if (! readPlanMeta (check) || check.warmSlot != slot
        || check.version != designVersion
        || warmSlots[slot].state.version != designVersion)
    {
        warmSlotStates[slot].store (WarmSlotReady, std::memory_order_release);
        return nullptr;
    }

    const int oldSlot = localWarmSlot;
    localWarmSlot = slot;
    updateAudioWarmMask();
    if (oldSlot >= 0 && oldSlot != slot)
        releaseWarmSlot (oldSlot);

    return &warmSlots[slot].state;
}

//==============================================================================
void FFTProcessor::prepare(double sr, int maxBlockSize, int channels)
{
    // getTwiddles() prefers the thread-local active bank. A previous spectrum
    // bank may already have been released after an SR change; clear the raw
    // pointer before the twiddle prewarm below can dereference it.
    setActiveTwiddleBank<double>(nullptr);

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

    {
        // An in-flight background warm build reads warmInput/warmOutput/
        // warmScratch and the history ring. The epoch bump above makes it
        // abort at its 1 ms wait checkpoint; take the build mutex so these
        // reallocations cannot race a build that is still inside an FFT.
        std::lock_guard<std::mutex> warmBuildLock (warmBuildMutex);

        if (static_cast<int>(warmScratch.size()) < maxL)
            warmScratch.assign(static_cast<size_t>(maxL), {});
        prefixScratch.assign(static_cast<size_t>(ConvBlockLen), 0.0f);
        warmCatchUpOutput.assign(static_cast<size_t>(ConvBlockLen), 0.0f);

        // Enough history to warm the longest FIR with two extra OLA blocks of
        // margin; power-of-two capacity keeps modulo cheap.
        const int warmBlocks = (MaxFirLength + ConvBlockLen - 1) / ConvBlockLen + 2;
        const int warmFeedSamples = warmBlocks * ConvBlockLen;
        if (static_cast<int>(warmInput.size()) < warmFeedSamples)
            warmInput.assign(static_cast<size_t>(warmFeedSamples), 0.0f);
        if (static_cast<int>(warmOutput.size()) < ConvBlockLen)
            warmOutput.assign(static_cast<size_t>(ConvBlockLen), 0.0f);

        // Allocate the history ring for the maximum channel count ONCE. The
        // background warm builder may read it concurrently, so prepare() must
        // not reallocate or clear it on ordinary sample-rate/channel-count
        // changes. Samples before historyValidFrom belong to a previous
        // session and are never used for warm-up.
        const int leadSamples = static_cast<int>(0.35 * sr)
                              + std::max(1, maxBlockSize);
        const int historyCapacity = nextPowerOfTwo(MaxFirLength + ConvBlockLen
                                                    + leadSamples);
        if (history.capacity() != historyCapacity
            || history.numChannels() < SubEQ::EQNode::MaxChannels)
            history.prepare(SubEQ::EQNode::MaxChannels, historyCapacity);
    }

    {
        std::vector<int> sizes;
        sizes.reserve(static_cast<size_t>(NumFirLengthChoices));
        for (int i = 0; i < NumFirLengthChoices; ++i)
            sizes.push_back(convolutionFftSize(FirLengthChoices[i], ConvBlockLen));
        twiddleBank.prepare(sizes);
    }
    currentHostBlockSamples.store(std::max(1, maxBlockSize), std::memory_order_release);
    historyValidFrom.store(history.capturedCount(), std::memory_order_release);

    activeConvFFTSize = 0;
    activeGroupDelay = 0;
    processedVersion = 0;
    reportedActiveLatency.store(0, std::memory_order_release);
    invalidateLatencyAck();
    pdcGuardMirror.store(false, std::memory_order_release);
    offlineDesignCooldown = 0;
    latencyGuardVersion = 0;
    latencyGuardSamples = 0;
    warmBuildSecondsEWMA.store(0.012, std::memory_order_relaxed);
    crossfadeRemaining = 0;
    fadeConvFFTSize = 0;
    audioTwiddlesPrewarmed = false;

    // Invalidate every design published before this prepare. The epoch bump
    // above makes an in-flight worker abort; the version cutoff makes the
    // audio thread ignore a meta that raced the reset.
    const int cutoff = stateVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    minimumPlanVersion.store(cutoff, std::memory_order_release);
    for (auto& state : designSlotStates)
    {
        // A producer may own a Reserved slot; preserve it so the worker and
        // offline renderer can never build into the same slot during reset.
        int expected = state.load (std::memory_order_acquire);
        while (expected != DesignSlotReserved)
        {
            if (state.compare_exchange_weak (expected, DesignSlotFree,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire))
                break;
        }
    }
    localDesignSlot = activeDesignSlot = oldDesignSlot = -1;
    activeState = nullptr;
    oldState = nullptr;
    audioDesignMask.store(0, std::memory_order_release);
    activeDesignVersion.store(0, std::memory_order_release);

    localWarmSlot = -1;
    audioWarmMask.store(0, std::memory_order_release);
    for (auto& state : warmSlotStates)
    {
        int expected = state.load (std::memory_order_acquire);
        while (expected != WarmSlotReserved)
        {
            if (state.compare_exchange_weak (expected, WarmSlotFree,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire))
                break;
        }
    }
    audioWarmPtr = nullptr;
    audioWarmTargetSample = 0;

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
    reportedActiveLatency.store(0, std::memory_order_release);
    invalidateLatencyAck();
    pdcGuardMirror.store(false, std::memory_order_release);
    latencyGuardVersion = 0;
    latencyGuardSamples = 0;
    offlineDesignCooldown = 0;
    warmBuildSecondsEWMA.store(0.012, std::memory_order_relaxed);

    // Invalidate any design that is currently being computed. Without this,
    // a design started before a mode/length change could still publish and be
    // adopted after the clear.
    designEpoch.fetch_add(1, std::memory_order_release);

    // Release every design slot. This runs on the audio thread (mode change)
    // or the setup thread (prepare); the ring state machine makes reuse safe.
    localDesignSlot = activeDesignSlot = oldDesignSlot = -1;
    activeState = nullptr;
    oldState = nullptr;
    audioDesignMask.store(0, std::memory_order_release);
    for (auto& state : designSlotStates)
    {
        // A producer may own a Reserved slot; preserve it so the worker and
        // offline renderer can never build into the same slot during reset.
        int expected = state.load (std::memory_order_acquire);
        while (expected != DesignSlotReserved)
        {
            if (state.compare_exchange_weak (expected, DesignSlotFree,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire))
                break;
        }
    }

    localWarmSlot = -1;
    audioWarmMask.store(0, std::memory_order_release);
    for (auto& state : warmSlotStates)
    {
        int expected = state.load (std::memory_order_acquire);
        while (expected != WarmSlotReserved)
        {
            if (state.compare_exchange_weak (expected, WarmSlotFree,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire))
                break;
        }
    }

    const int cutoff = stateVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    minimumPlanVersion.store(cutoff, std::memory_order_release);
    activeDesignVersion.store(0, std::memory_order_release);
    audioWarmPtr = nullptr;
    audioWarmTargetSample = 0;
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
    int lastDesignSlot = -1;
    int lastDesignVersion = 0;
    int lastDesignEpoch = -1;
    uint64_t lastDesignTarget = 0;

    while (!threadShouldExit())
    {
        redesignEvent.wait(20);

        auto* source = parameterSource.load (std::memory_order_acquire);
        if (source == nullptr)
            continue;

        const auto mode = static_cast<EQMode> (
            static_cast<int> (source->getRawParameterValue ("eq_mode")->load()));

        // Offline rendering builds designs/warm states synchronously on the
        // audio thread; keep the worker out of the way.
        if (nonRealtime.load (std::memory_order_acquire))
            continue;

        if (redesignRequested.load (std::memory_order_acquire))
            warmRebuildRequested.store (false, std::memory_order_release);

        const bool haveLastDesign = lastDesignSlot >= 0
            && lastDesignSlot < NumDesignSlots
            && lastDesignEpoch == designEpoch.load (std::memory_order_acquire)
            && lastDesignVersion >= minimumPlanVersion.load (std::memory_order_acquire)
            && designSlotStates[lastDesignSlot].load (std::memory_order_acquire)
                   != DesignSlotFree
            && designSlots[lastDesignSlot].state.version == lastDesignVersion;

        // Retry a warm for an already-published design whose target is still
        // in the future. The design metadata was published early so the audio
        // coordinator could start its latency transition in time.
        if (warmRebuildRequested.exchange (false, std::memory_order_acquire)
            && haveLastDesign && mode != EQMode::ZeroLatency
            && lastDesignVersion == stateVersion.load (std::memory_order_acquire))
        {
            const FIRState& design = designSlots[lastDesignSlot].state;

            // If the previous target was missed, choose a fresh future host
            // block for the SAME design instead of rebuilding coefficients.
            if (lastDesignTarget <= history.publishedCount())
            {
                const int hostBlock = std::max (1, currentHostBlockSamples.load (std::memory_order_acquire));
                const double sr = sampleRate.load (std::memory_order_acquire);
                const uint64_t lead = warmLeadSamples (
                    sr, hostBlock, warmBuildSecondsEWMA.load (std::memory_order_relaxed));
                lastDesignTarget = planWarmTarget (history.capturedCount(), lead,
                                                   hostBlock, ConvBlockLen).targetSample;
            }

            const int warmSlot = reserveFreeWarmSlot();
            if (warmSlot < 0)
            {
                warmRebuildRequested.store (true, std::memory_order_release);
            }
            else
            {
                WarmState& warm = warmSlots[warmSlot].state;
                if (buildWarmState (design, warm, lastDesignEpoch, lastDesignTarget))
                {
                    warm.version = lastDesignVersion;
                    warmSlotStates[warmSlot].store (WarmSlotReady,
                                                    std::memory_order_release);
                    writePlanMeta (lastDesignSlot, warmSlot, lastDesignVersion,
                                   design.groupDelay + (ConvBlockLen - 1),
                                   lastDesignTarget, lastDesignEpoch);
                }
                else
                {
                    warmSlotStates[warmSlot].store (WarmSlotFree,
                                                    std::memory_order_release);
                    if (lastDesignEpoch != designEpoch.load (std::memory_order_acquire))
                    {
                        lastDesignSlot = -1;
                        lastDesignVersion = 0;
                        lastDesignEpoch = -1;
                    }
                    else
                    {
                        warmRebuildRequested.store (true, std::memory_order_release);
                    }
                }
            }
        }

        if (! redesignRequested.exchange (false, std::memory_order_acquire))
            continue;

        if (mode == EQMode::ZeroLatency)
            continue;

        int epoch = 0;
        auto newState = buildDesignSnapshot (epoch);
        if (newState == nullptr)
            continue;

        const int hostBlock = std::max (1, currentHostBlockSamples.load (std::memory_order_acquire));
        const double sr = sampleRate.load (std::memory_order_acquire);
        const uint64_t lead = warmLeadSamples (
            sr, hostBlock, warmBuildSecondsEWMA.load (std::memory_order_relaxed));
        const WarmTarget target = planWarmTarget (history.capturedCount(), lead,
                                                  hostBlock, ConvBlockLen);

        // Wait for a free design slot (bounded; the audio mask keeps active,
        // fading and local designs pinned, so one of four slots is normally
        // free). Never blocks the audio thread.
        int slot = -1;
        for (int tries = 0; tries < 100 && slot < 0 && ! threadShouldExit(); ++tries)
        {
            if (epoch != designEpoch.load (std::memory_order_acquire))
                break;
            slot = reserveFreeDesignSlot();
            if (slot < 0)
                juce::Thread::sleep (2);
        }

        if (slot < 0)
        {
            redesignRequested.store (true, std::memory_order_release);
            continue;
        }

        if (epoch != designEpoch.load (std::memory_order_acquire))
        {
            designSlotStates[slot].store (DesignSlotFree, std::memory_order_release);
            continue;
        }

        // Phase 1: move the immutable design into the ring slot and publish
        // (slot, version, latency, target) through the seqlock. The audio
        // coordinator can now schedule the latency transition; adoption still
        // requires the matching warm state.
        const int publishedVersion = stateVersion.fetch_add (1, std::memory_order_acq_rel) + 1;
        designSlots[slot].state = std::move (*newState);
        designSlots[slot].state.version = publishedVersion;

        // A reset may have landed while we were moving the payload into the
        // reserved slot. Do not publish an old-epoch design; drop it.
        if (epoch != designEpoch.load (std::memory_order_acquire))
        {
            designSlotStates[slot].store (DesignSlotFree, std::memory_order_release);
            continue;
        }

        // Keep the slot Reserved until the payload, snapshot and meta are
        // fully written. A reset can then only see Reserved (and preserve it)
        // instead of freeing it while this thread is still reading it.
        publishFirResponseSnapshot (designSlots[slot].state);
        writePlanMeta (slot, -1, publishedVersion,
                       designSlots[slot].state.groupDelay + (ConvBlockLen - 1),
                       target.targetSample, epoch);
        designSlotStates[slot].store (DesignSlotReady, std::memory_order_release);
        pdcGuardMirror.store (false, std::memory_order_release);

        lastDesignSlot = slot;
        lastDesignVersion = publishedVersion;
        lastDesignEpoch = epoch;
        lastDesignTarget = target.targetSample;

        // Phase 2: build the exact warm state for the published target.
        const int warmSlot = reserveFreeWarmSlot();
        if (warmSlot < 0)
        {
            warmRebuildRequested.store (true, std::memory_order_release);
            continue;
        }

        WarmState& warm = warmSlots[warmSlot].state;
        if (! buildWarmState (designSlots[slot].state, warm, epoch, target.targetSample))
        {
            warmSlotStates[warmSlot].store (WarmSlotFree, std::memory_order_release);
            if (epoch != designEpoch.load (std::memory_order_acquire))
            {
                lastDesignSlot = -1;
                lastDesignVersion = 0;
                lastDesignEpoch = -1;
            }
            else
            {
                warmRebuildRequested.store (true, std::memory_order_release);
            }
            continue;
        }

        warm.version = publishedVersion;
        warmSlotStates[warmSlot].store (WarmSlotReady, std::memory_order_release);

        if (publishedVersion == stateVersion.load (std::memory_order_acquire)
            && epoch == designEpoch.load (std::memory_order_acquire))
        {
            writePlanMeta (slot, warmSlot, publishedVersion,
                           designSlots[slot].state.groupDelay + (ConvBlockLen - 1),
                           target.targetSample, epoch);
        }
        else
        {
            warmSlotStates[warmSlot].store (WarmSlotFree, std::memory_order_release);
        }
    }
}

//==============================================================================
// Build FIR coefficients from the current APVTS snapshot. Safe to call from
// the background worker or, in offline rendering, from the audio thread; the
// designEngine is serialized by designMutex. Returns nullptr for ZL / failure.
std::unique_ptr<FFTProcessor::FIRState> FFTProcessor::buildDesignSnapshot(int& outEpoch)
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

    auto newState = std::make_unique<FIRState>();
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
bool FFTProcessor::buildWarmState(const FIRState& design, WarmState& out,
                                  int expectedEpoch, uint64_t targetSample)
{
    if (design.freqCoeffs.empty()
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

    out.states.resize(static_cast<size_t>(std::max(1, channels)));
    out.version = design.version;
    out.convFFTSize = L;
    out.groupDelay = design.groupDelay;
    out.targetSample = targetSample;

    const int maxL = convolutionFftSize(MaxFirLength, ConvBlockLen);
    for (int ch = 0; ch < channels; ++ch)
        prepareOlaChannelState(out.states[static_cast<size_t>(ch)], M, maxL);

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
                                      out.states[static_cast<size_t>(ch)],
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
                                out.states[static_cast<size_t>(ch)],
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
            processOlaChannel(out.states[static_cast<size_t>(ch)],
                              warmInput.data(), warmOutput.data(), prefix,
                              M, L, design.freqCoeffs.data(), warmScratch);
        }
        workSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
    }

    const double previousEWMA = warmBuildSecondsEWMA.load(std::memory_order_relaxed);
    warmBuildSecondsEWMA.store(0.75 * previousEWMA + 0.25 * workSeconds,
                               std::memory_order_relaxed);
    return true;
}

//==============================================================================
// Offline path: build the design and an exact warm state at the current
// committed boundary. This runs on the audio thread only when the host marks
// the processor non-realtime; there is no deadline to miss.
bool FFTProcessor::buildWarmAtCurrentBoundary(const FIRState& design, WarmState& out)
{
    const int M = ConvBlockLen;
    const uint64_t committed = history.publishedCount();

    const int channels = preparedChannels.load(std::memory_order_acquire);
    const int N = design.firLength;
    const int L = design.convFFTSize;
    if (N <= 0 || L < M
        || static_cast<int>(warmInput.size()) < std::max(N, M)
        || static_cast<int>(warmOutput.size()) < M
        || static_cast<int>(warmScratch.size()) < L)
        return false;

    const int maxL = convolutionFftSize(MaxFirLength, ConvBlockLen);
    out.states.resize(static_cast<size_t>(std::max(1, channels)));
    out.version = design.version;
    out.convFFTSize = L;
    out.groupDelay = design.groupDelay;
    out.targetSample = committed;

    for (int ch = 0; ch < channels; ++ch)
        prepareOlaChannelState(out.states[static_cast<size_t>(ch)], M, maxL);

    const uint64_t validFrom = historyValidFrom.load(std::memory_order_acquire);

    for (int ch = 0; ch < channels; ++ch)
    {
        if (!olaSynthesizeAt(history, ch, committed, validFrom,
                             N, L, M, design.freqCoeffs.data(),
                             out.states[static_cast<size_t>(ch)],
                             warmInput, warmOutput, warmScratch))
            return false;
    }

    return true;
}

bool FFTProcessor::ensureOfflineReady()
{
    if (! nonRealtime.load (std::memory_order_acquire))
        return false;

    // The live worker may still be finishing a warm build when the host flips
    // to offline. Never share its scratch buffers; skip this block and retry.
    std::unique_lock<std::mutex> warmBuildLock (warmBuildMutex, std::try_to_lock);
    if (! warmBuildLock.owns_lock())
        return false;

    const uint64_t committed = history.publishedCount();

    auto published = getPublishedStateForAudioThread();

    const bool needDesign = published == nullptr
                         || redesignRequested.load (std::memory_order_acquire);
    if (needDesign)
    {
        if (offlineDesignCooldown > 0)
        {
            --offlineDesignCooldown;
        }
        else
        {
            int epoch = 0;
            auto design = buildDesignSnapshot (epoch);
            if (design != nullptr
                && epoch == designEpoch.load (std::memory_order_acquire))
            {
                const int warmSlot = reserveFreeWarmSlot();
                if (warmSlot >= 0
                    && buildWarmAtCurrentBoundary (*design, warmSlots[warmSlot].state))
                {
                    const int slot = reserveFreeDesignSlot();
                    if (slot >= 0)
                    {
                        if (epoch == designEpoch.load (std::memory_order_acquire))
                        {
                            const int v = stateVersion.fetch_add (1, std::memory_order_acq_rel) + 1;
                            design->version = v;
                            warmSlots[warmSlot].state.version = v;

                            designSlots[slot].state = std::move (*design);

                            if (epoch != designEpoch.load (std::memory_order_acquire))
                            {
                                designSlotStates[slot].store (DesignSlotFree,
                                                              std::memory_order_release);
                                warmSlotStates[warmSlot].store (WarmSlotFree,
                                                                std::memory_order_release);
                                return false;
                            }

                            publishFirResponseSnapshot (designSlots[slot].state);
                            writePlanMeta (slot, warmSlot, v,
                                           designSlots[slot].state.groupDelay + (ConvBlockLen - 1),
                                           warmSlots[warmSlot].state.targetSample, epoch);
                            designSlotStates[slot].store (DesignSlotReady,
                                                          std::memory_order_release);
                            warmSlotStates[warmSlot].store (WarmSlotReady,
                                                            std::memory_order_release);
                            pdcGuardMirror.store (false, std::memory_order_release);

                            redesignRequested.store (false, std::memory_order_release);
                            offlineDesignCooldown = 1;
                            return true;
                        }

                        designSlotStates[slot].store (DesignSlotFree,
                                                      std::memory_order_release);
                    }

                    warmSlotStates[warmSlot].store (WarmSlotFree,
                                                    std::memory_order_release);
                }
                else if (warmSlot >= 0)
                {
                    warmSlotStates[warmSlot].store (WarmSlotFree,
                                                    std::memory_order_release);
                }
            }
            offlineDesignCooldown = 8;
        }
    }

    published = getPublishedStateForAudioThread();
    if (published == nullptr)
        return false;

    // Once the published design is the one already driving the audio path,
    // there is no pending adoption; do not rebuild a warm state every block.
    if (published->version == processedVersion)
        return false;

    auto warm = getWarmStateForAudioThread (published->version);
    if (warm == nullptr || warm->version != published->version
        || warm->targetSample != committed)
    {
        const int warmSlot = reserveFreeWarmSlot();
        if (warmSlot < 0)
            return false;

        if (! buildWarmAtCurrentBoundary (*published, warmSlots[warmSlot].state))
        {
            warmSlotStates[warmSlot].store (WarmSlotFree, std::memory_order_release);
            return false;
        }

        warmSlots[warmSlot].state.version = published->version;
        warmSlotStates[warmSlot].store (WarmSlotReady, std::memory_order_release);

        if (published->version == stateVersion.load (std::memory_order_acquire)
            && localDesignSlot >= 0)
        {
            writePlanMeta (localDesignSlot, warmSlot, published->version,
                           published->groupDelay + (ConvBlockLen - 1),
                           warmSlots[warmSlot].state.targetSample,
                           designEpoch.load (std::memory_order_acquire));
        }
        else
        {
            warmSlotStates[warmSlot].store (WarmSlotFree, std::memory_order_release);
        }
        return true;
    }

    return false;
}

//==============================================================================
bool FFTProcessor::process(juce::AudioBuffer<float>& buffer)
{
    // All twiddle tables were prepared off this thread; the pointer switch is
    // O(1) and means the first audio FFT of every size performs no allocation.
    setActiveTwiddleBank<double>(&twiddleBank);

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return false;

    if (oldState != nullptr && crossfadeRemaining <= 0)
    {
        const int oldSlot = oldDesignSlot;
        oldState = nullptr;
        oldDesignSlot = -1;
        updateAudioDesignMask();
        releaseDesignSlot(oldSlot);
    }

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
    }

    const int channelCount = std::min(buffer.getNumChannels(), numChannels);
    if (channelCount <= 0)
        return false;

    const uint64_t historyCount = history.publishedCount();

    auto warm = getWarmStateForAudioThread(published->version);

    // Track the candidate's progressed target on the audio thread only. The
    // published WarmState is immutable once published; writing its target in
    // place raced the worker and made fresh candidates look stale.
    if (warm != audioWarmPtr)
    {
        audioWarmPtr = warm;
        audioWarmTargetSample = warm != nullptr ? warm->targetSample : 0;
    }

    // A warm candidate is built at a target host block. Normally it is ready
    // exactly at that block; if the worker finished just after the boundary,
    // advance it by at most one OLA block on the audio thread (bounded cost:
    // at most one extra FFT). Larger gaps are rejected and rebuilt.
    bool warmOk = warm != nullptr
        && warm->version == published->version
        && warm->convFFTSize == published->convFFTSize
        && warm->states.size() == activeStates.size()
        && audioWarmTargetSample <= historyCount;
    int warmGap = 0;
    if (warmOk)
    {
        warmGap = static_cast<int>(historyCount - audioWarmTargetSample);
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
                const uint64_t end = audioWarmTargetSample
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
            audioWarmTargetSample = historyCount;
    }

    auto releaseWarm = [&](const WarmState* w)
    {
        if (w == nullptr)
            return;
        const int slot = localWarmSlot;
        localWarmSlot = -1;
        updateAudioWarmMask();
        releaseWarmSlot (slot);
    };

    // ---- adopt a new design ----
    if (published->version != processedVersion && crossfadeRemaining <= 0)
    {
        const bool designChangedDelay = (activeState == nullptr)
                                     || (published->groupDelay != activeGroupDelay);
        const int publishedTotal = published->groupDelay + (ConvBlockLen - 1);
        const bool hostAlreadyCompensates =
            publishedTotal == hostLatencySamples.load (std::memory_order_acquire);

        // PDC handshake: the host must be told a NEW latency before the audio
        // switches to it. A new design at the latency the host already
        // compensates needs no handshake (and must not wait for an ack that
        // the coordinator will never request, e.g. after an SR re-prepare).
        // Wait/guard are measured in samples so the policy does not change
        // with the host block size.
        bool pdcBlocked = false;
        if (designChangedDelay
            && ! hostAlreadyCompensates
            && ! nonRealtime.load(std::memory_order_acquire))
        {
            const int ack = getAckedLatencyVersion();
            const double sr = sampleRate.load(std::memory_order_acquire);
            if (ack < published->version)
            {
                // Offline/headless hosts may never pump the message thread.
                // Never switch a latency the host has not acknowledged: keep
                // the old path and let the PluginProcessor coordinator abort
                // its bounded transition and report the old latency. The
                // coordinator reads the design target from publishedPlan, so
                // a retry can report it once acknowledgements resume. (The
                // host-facing value is reportedLatency, not this atomic.)
                pdcBlocked = true;
            }
            else
            {
                // The host has acknowledged this version (or a later one):
                // run the sample-based guard before switching.
                if (latencyGuardVersion != published->version)
                {
                    latencyGuardVersion = published->version;
                    const int guardByTime = (sr > 0.0) ? static_cast<int>(0.05 * sr) : 0;
                    latencyGuardSamples = std::max(2 * numSamples, guardByTime);
                    pdcGuardMirror.store(latencyGuardSamples > 0,
                                         std::memory_order_release);
                }

                if (latencyGuardSamples > 0)
                {
                    latencyGuardSamples -= numSamples;
                    // Stay "guarding" until the design is actually adopted:
                    // the switch happens on the block AFTER the counter hits
                    // zero, and the coordinator must not abort in that gap.
                    pdcGuardMirror.store(true, std::memory_order_release);
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
        else if (pdcBlocked)
        {
            // The warm is alive (possibly advanced by catch-up) but the PDC
            // ack has not arrived. If its published target is already behind
            // the playhead, the coordinator can never start a transition
            // (it only reports near a FUTURE target). Ask the worker for a
            // fresh future target so the report/ack cycle can resume.
            PlanMeta meta;
            if (readPlanMeta (meta) && meta.version == published->version
                && meta.target <= historyCount)
            {
                warmRebuildRequested.store(true, std::memory_order_release);
            }
        }

        // No zero-state fallback: a design is published only together with a
        // matching warm state. If the candidate's boundary was missed, keep
        // the active design (or the delayed IIR bridge) and rebuild the warm
        // for a new future boundary. Never start a FIR from zero history.
        if (warmOk && !pdcBlocked)
        {
            const bool sameDelay = activeState != nullptr
                                && published->groupDelay == activeGroupDelay;
            const int previousActiveSlot = activeDesignSlot;
            const int newSlot = localDesignSlot;

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
                oldDesignSlot = activeDesignSlot;
                oldStates.swap(activeStates);
                activeStates.swap(warm->states);
                fadeConvFFTSize = activeConvFFTSize;
                crossfadeRemaining = CrossfadeLen;
            }
            else
            {
                // Different group delay: flushing is the only time-aligned
                // option. The old design slot is released below.
                activeStates.swap(warm->states);
                oldState = nullptr;
                oldDesignSlot = -1;
                crossfadeRemaining = 0;
            }

            activeState = published;
            activeDesignSlot = newSlot;
            activeConvFFTSize = published->convFFTSize;
            activeGroupDelay = published->groupDelay;
            processedVersion = published->version;
            activeDesignVersion.store(published->version, std::memory_order_release);
            reportedActiveLatency.store(published->groupDelay + (ConvBlockLen - 1),
                                        std::memory_order_release);
            pdcGuardMirror.store(false, std::memory_order_release);

            updateAudioDesignMask();
            if (previousActiveSlot >= 0
                && previousActiveSlot != activeDesignSlot
                && previousActiveSlot != oldDesignSlot
                && previousActiveSlot != localDesignSlot)
            {
                releaseDesignSlot(previousActiveSlot);
            }

            releaseWarm(warm);
            audioWarmPtr = nullptr;
            audioWarmTargetSample = 0;
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
            const int oldSlot = oldDesignSlot;
            oldState = nullptr;
            oldDesignSlot = -1;
            updateAudioDesignMask();
            releaseDesignSlot(oldSlot);
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
                const int oldSlot = oldDesignSlot;
                oldState = nullptr;
                oldDesignSlot = -1;
                updateAudioDesignMask();
                releaseDesignSlot(oldSlot);
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
    PlanMeta meta;
    if (readPlanMeta (meta) && meta.slot >= 0 && meta.slot < NumDesignSlots
        && meta.version >= minimumPlanVersion.load (std::memory_order_acquire)
        && meta.epoch == designEpoch.load (std::memory_order_acquire))
    {
        return tailSamples (designSlots[meta.slot].state.firLength, ConvBlockLen);
    }
    return tailSamples (firActualTaps (MaxFirLength), ConvBlockLen);
}

//==============================================================================
void FFTProcessor::publishFirResponseSnapshot (const FIRState& state)
{
    FirResponseSnapshot snapshot;
    const double sr = sampleRate.load (std::memory_order_acquire);
    snapshot.version = state.version;
    snapshot.groupDelay = state.groupDelay;
    snapshot.sampleRate = sr;
    snapshot.count = NumResponsePoints;

    constexpr double fMin = 0.5;
    constexpr double fMax = 500.0;
    const double logSpan = std::log (fMax / fMin);

    for (int i = 0; i < NumResponsePoints; ++i)
    {
        const double t = static_cast<double> (i)
                       / static_cast<double> (NumResponsePoints - 1);
        const double freq = fMin * std::exp (t * logSpan);
        const double w = juce::MathConstants<double>::twoPi * freq / sr;

        double magDb = -120.0;
        if (! computeFirMagnitudeDb (state, w, magDb))
            magDb = -120.0;
        snapshot.magnitudeDb[i] = static_cast<float> (magDb);

        double phaseDeg = 0.0;
        snapshot.phaseValid[i] = computeFirPhaseResponseDegrees (state, w, phaseDeg);
        snapshot.phaseDeg[i] = static_cast<float> (phaseDeg);
    }

    std::lock_guard<std::mutex> lock (responseSnapshotMutex);

    // Never evict the snapshot for the design that is actually audible: a
    // burst of automation while PDC adoption is blocked could otherwise make
    // the GUI fall back to the IIR curve.
    const int activeVersion = activeDesignVersion.load (std::memory_order_acquire);
    int target = responseSnapshotNext;
    for (int i = 0; i < NumResponseSnapshots; ++i)
    {
        const int candidate = (responseSnapshotNext + i) % NumResponseSnapshots;
        if (responseSnapshots[candidate].version != activeVersion)
        {
            target = candidate;
            break;
        }
    }

    responseSnapshots[target] = snapshot;
    responseSnapshotNext = (target + 1) % NumResponseSnapshots;
}

bool FFTProcessor::getFirResponseSnapshot (FirResponseSnapshot& out, int version) const
{
    if (version <= 0)
        return false;

    std::lock_guard<std::mutex> lock (responseSnapshotMutex);
    for (int i = 0; i < NumResponseSnapshots; ++i)
    {
        if (responseSnapshots[i].version == version)
        {
            out = responseSnapshots[i];
            return true;
        }
    }
    return false;
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
