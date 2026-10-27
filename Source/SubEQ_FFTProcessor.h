/*
  ==============================================================================

    SubEQ_FFTProcessor.h
    FIR-based EQ processor supporting Minimum Phase and Linear Phase modes.
    Uses FFT overlap-add convolution (blocked, low-latency partition-free
    scheme with ConvBlockLen-sample processing latency).
    FIR coefficient design runs on a background thread; the audio thread
    consumes the latest published coefficient set (lock-free in steady state).
    DFT/IDFT computed with the shared radix-2 FFT (SubEQ_DSPMath.h) — JUCE FFT
    wrapper is unreliable in this build.

  ==============================================================================
*/

#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <JuceHeader.h>
#include "SubEQ_Core.h"
#include "SubEQ_DSPMath.h"
#include "SubEQ_FFTConvolver.h"
#include "SubEQ_FIRHistory.h"
#include "SubEQ_WarmPlan.h"
#include "SubEQ_LatencyMath.h"

namespace SubEQ
{

enum class EQMode { ZeroLatency = 0, MinimumPhase, LinearPhase };

class FFTProcessor : private juce::Thread
{
public:
    // Selectable FIR lengths (via the "fir_length" parameter)
    static constexpr int DefaultFirLength = 4096;
    static constexpr int FirLengthChoices[] = { 4096, 16384, 65536 };
    static constexpr int NumFirLengthChoices = 3;
    static constexpr int MaxFirLength = 65536;

    // Overlap-add block size (samples): processing latency added on top of the
    // FIR group delay (~10.7 ms @ 48 kHz).
    static constexpr int ConvBlockLen = 512;

    // Worst-case pipeline latency (linear phase, 65536 taps). OutputStage uses
    // this to size its bypass dry-delay ring once, in prepare().
    static constexpr int MaxLatencySamples =
        pipelineLatency(2, MaxFirLength, ConvBlockLen);


    FFTProcessor();
    ~FFTProcessor() override;

    // Audio thread
    void prepare(double sampleRate, int maxBlockSize, int numChannels);

    // Clear convolution/design state but keep the raw input history (used on
    // mode switches, so re-entering FIR can warm the next design from the
    // audio that played in Zero Latency).
    void resetConvolution();

    // Rebuild only the warm state for the current design, choosing a fresh
    // future host-block target. Used when the audio missed a warm boundary;
    // the design (and its latency) stays published.
    void requestWarmRebuild();

    // Capture the raw input for the warm-up history. Called for every mode;
    // publishNow=false defers visibility until publishHistory() at block end.
    void captureHistory(const float* const* channels, int numChannels, int numSamples,
                        bool publishNow = true);
    void publishHistory() noexcept { history.publish(); }

    // Provide the parameter source used by the background thread to build the
    // design-time EQ engine snapshot (apvts must outlive this processor).
    void setParameterSource(juce::AudioProcessorValueTreeState* source);

    // Ask the background thread to redesign the FIR from current parameters.
    // Non-blocking; the new coefficients are published asynchronously.
    void requestRedesign();

    // Offline rendering has no real-time deadline: the audio thread builds the
    // design and exact warm state synchronously at the current OLA boundary.
    void setNonRealtime(bool shouldBeNonRealtime) noexcept
    {
        nonRealtime.store(shouldBeNonRealtime, std::memory_order_release);
    }

    // Structural change (mode/FIR length/prepare): invalidate any design
    // currently being computed, but keep the active design running until the
    // replacement is ready. Used to avoid starvation under continuous
    // automation.
    void invalidateDesigns();

    // Process entire audio buffer (all channels). Returns true when an active
    // FIR design wrote the buffer; false when no design is active yet (the
    // buffer is left untouched so the caller can keep the IIR path alive).
    bool process(juce::AudioBuffer<float>& buffer);
    bool hasActiveDesign() const noexcept { return activeState != nullptr; }

    // Latency/readiness of the design actually active in the audio path.
    // getLatencySamples() includes the overlap-add block latency.
    int getLatencySamples() const;

    // Version, latency and target published as one PlanMeta record (seqlock);
    // the slot index rides along so the audio thread can take the payload
    // without shared ownership or a mutex.
    void getPublishedPlan(int& version, int& latency, bool& warmReady) const noexcept
    {
        PlanMeta meta;
        if (readPlanMeta (meta))
        {
            cachedPlan = meta;
            cachedPlanValid = true;
        }
        else if (cachedPlanValid)
        {
            // A long torn-writer window must not look like "no plan" to the
            // coordinator; keep the last coherent plan for this block.
            meta = cachedPlan;
        }
        else
        {
            version = 0;
            latency = 0;
            warmReady = false;
            return;
        }

        const int epoch = designEpoch.load (std::memory_order_acquire);
        if (meta.epoch == epoch)
        {
            version = meta.version;
            latency = meta.latency;
            warmReady = meta.warmSlot >= 0
                     && meta.version >= minimumPlanVersion.load (std::memory_order_acquire);
        }
        else
        {
            version = 0;
            latency = 0;
            warmReady = false;
        }
    }

    // The ack is paired with the epoch captured when the timer loaded the
    // request. An abort increments the epoch, so a timer that started before
    // the abort can never acknowledge the superseded request afterwards.
    uint64_t getLatencyAckEpoch() const noexcept
    {
        return latencyAckState.load(std::memory_order_acquire) >> 32;
    }
    int getAckedLatencyVersion() const noexcept
    {
        return static_cast<int>(static_cast<uint32_t>(
            latencyAckState.load(std::memory_order_acquire)));
    }
    bool acknowledgeLatency(int version, uint64_t epoch) noexcept
    {
        uint64_t cur = latencyAckState.load(std::memory_order_acquire);
        for (;;)
        {
            if ((cur >> 32) != epoch)
                return false;
            const uint64_t desired = (epoch << 32)
                                   | static_cast<uint32_t>(version);
            if (latencyAckState.compare_exchange_weak(cur, desired,
                                                      std::memory_order_release,
                                                      std::memory_order_acquire))
                return true;
        }
    }

    // Host-facing latency currently compensated by the host. A freshly built
    // design at the SAME latency needs no new PDC handshake; without this an
    // SR/channel re-prepare would reset the ack version and block adoption
    // forever (the coordinator only reports latency CHANGES).
    void setHostLatency(int samples) noexcept
    {
        hostLatencySamples.store(samples, std::memory_order_release);
    }

    // A transition was aborted after the host was told the old latency. The
    // same design version must not be adopted until the coordinator reports
    // the new latency again; the epoch bump also fences in-flight acks.
    void invalidateLatencyAck() noexcept
    {
        // An aborted candidate must re-arm the 50 ms guard if it is acked
        // again; otherwise the next ack would switch with no PDC settling.
        latencyGuardVersion = 0;
        latencyGuardSamples = 0;
        pdcGuardMirror.store(false, std::memory_order_release);

        uint64_t cur = latencyAckState.load(std::memory_order_acquire);
        for (;;)
        {
            const uint64_t epoch = ((cur >> 32) + 1) & 0xffffffffu;
            const uint64_t desired = epoch << 32;   // ack version 0
            if (latencyAckState.compare_exchange_weak(cur, desired,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire))
                return;
        }
    }

    // True while the ack guard (50 ms) is holding the switch. The PDC
    // coordinator must not interpret a warm target that passed during this
    // guard as a miss, or it would discard a valid acked candidate.
    bool isPdcGuardActive() const noexcept
    {
        return pdcGuardMirror.load(std::memory_order_acquire);
    }

    // Host-block-aligned target of the latest published plan (0 when none).
    // PluginProcessor uses this to delay the PDC report/gate until the plan is
    // actually about to be adopted, instead of gating for the whole build lead.
    uint64_t getPendingTargetSample() const noexcept
    {
        PlanMeta meta;
        if (readPlanMeta (meta))
        {
            cachedPlan = meta;
            cachedPlanValid = true;
        }
        else if (cachedPlanValid)
        {
            meta = cachedPlan;
        }
        else
        {
            return 0;
        }

        return meta.epoch == designEpoch.load (std::memory_order_acquire)
                   ? meta.target : 0;
    }


    uint64_t getCommittedSampleCount() const noexcept
    {
        return history.publishedCount();
    }

#if SUBEQ_TESTING
    // Test-only publication diagnostics (the runtime host calls process() on
    // the same thread, so these reads do not race the audio thread there).
    int getWarmVersionForTest() const
    {
        PlanMeta meta;
        return readPlanMeta (meta) && meta.warmSlot >= 0 ? meta.version : -1;
    }
    uint64_t getWarmTargetForTest() const
    {
        PlanMeta meta;
        return readPlanMeta (meta) && meta.warmSlot >= 0 ? meta.target : 0;
    }
    int getAudioLocalVersionForTest() const
    {
        return localDesignSlot >= 0
                   ? designSlots[localDesignSlot].state.version : -1;
    }
    int getAudioLocalWarmVersionForTest() const
    {
        return localWarmSlot >= 0
                   ? warmSlots[localWarmSlot].state.version : -1;
    }
#endif

    // Total pipeline tail (FIR length + block latency), for tail-length reporting
    int getTailLengthSamples() const;

    // 已发布设计的版本号（每次发布/丢弃都自增）：GUI 据此在后台设计异步发布
    // 之后刷新相位曲线，无需依赖参数变更通知。
    int getDesignVersion() const noexcept { return stateVersion.load(std::memory_order_acquire); }

    // Published FIR state (written by the background thread, read by the audio
    // thread). Immutable once published — safe to share. Public so the GUI can
    // take one snapshot and evaluate both magnitude and phase without taking
    // the state lock once per pixel.
    struct FIRState
    {
        std::vector<double> coeffs;                      // time-domain, firLength taps (double: DSP 约定)
        std::vector<std::complex<double>> freqCoeffs;    // FFT(h) over convFFTSize (double)
        int firLength = DefaultFirLength;
        int convFFTSize = 0;
        // FIR bulk delay used for PDC (no block latency). Linear Phase:
        // (N-1)/2. Minimum Phase: 0 — a min-phase FIR's direct sound sits at
        // tap 0, and reporting the max group delay over-compensated (the
        // plugin played EARLY against other tracks, with PDC churn on edits).
        int groupDelay = 0;
        int version = 0;
    };

    // GUI-friendly response table: 1024 log-spaced points over 0.5..500 Hz.
    // Published off the audio thread; the GUI interpolates it instead of
    // holding a published FIRState (and its lifetime) across paints.
    static constexpr int NumResponsePoints = 1024;
    struct FirResponseSnapshot
    {
        int version = -1;
        int groupDelay = 0;
        double sampleRate = 48000.0;
        int count = 0;
        float magnitudeDb[NumResponsePoints] {};
        float phaseDeg[NumResponsePoints] {};
        bool phaseValid[NumResponsePoints] {};
    };
    bool getFirResponseSnapshot(FirResponseSnapshot& out, int version) const;

    // Version of the design actually feeding the audio path (0 = none).
    int getActiveDesignVersion() const noexcept
    {
        return activeDesignVersion.load(std::memory_order_acquire);
    }

    static bool computeFirPhaseResponseDegrees(const FIRState& state,
                                               double normalizedW, double& phaseDeg);
    static bool computeFirMagnitudeDb(const FIRState& state,
                                      double normalizedW, double& magnitudeDb);

    static juce::StringArray getModeChoices();
    static juce::String getModeName(EQMode mode);
    static juce::String getLatencyText(int latencySamples, double sampleRate);

private:
    void run() override;

    // A pre-warmed OLA state for one published design. Built on the background
    // thread from the input history ring; the audio thread swaps the per-channel
    // states in at an OLA block boundary. Never destroyed on the audio thread.
    struct WarmState
    {
        std::vector<OlaChannelState> states;
        int version = 0;
        uint64_t targetSample = 0;   // exact host block start
        int convFFTSize = 0;
        int groupDelay = 0;
    };

    // ---- Immutable design ring (4 slots) + PlanMeta seqlock ----
    // The producer writes only Free slots and publishes (slot, version,
    // latency, target) through the seqlock. The audio thread pins the slots it
    // references in audioDesignMask; the producer never reuses a pinned slot
    // or the slot the current meta points at. No shared_ptr and no retirement
    // ring is involved on the audio path.
    static constexpr int NumDesignSlots = 4;
    static constexpr int DesignSlotFree = 0;
    static constexpr int DesignSlotReady = 1;
    static constexpr int DesignSlotInUse = 2;
    static constexpr int DesignSlotReserved = 3;

    struct DesignSlot
    {
        FIRState state;
    };
    DesignSlot designSlots[NumDesignSlots];
    std::atomic<int> designSlotStates[NumDesignSlots];

    struct PlanMeta
    {
        uint32_t seq = 0;
        int slot = -1;          // design ring slot
        int warmSlot = -1;      // warm ring slot (-1 = warming)
        int version = 0;
        int latency = 0;
        uint64_t target = 0;
        int epoch = 0;          // designEpoch at publish; stale epochs ignored
    };
    struct PlanMetaStorage
    {
        std::atomic<int> slot { -1 };
        std::atomic<int> warmSlot { -1 };
        std::atomic<int> version { 0 };
        std::atomic<int> latency { 0 };
        std::atomic<uint64_t> target { 0 };
        std::atomic<int> epoch { 0 };
    };
    std::atomic<uint32_t> planMetaSeq { 0 };
    PlanMetaStorage planMeta;                   // relaxedly stored; seqlock fences
    mutable PlanMeta cachedPlan;                // audio-side last coherent plan
    mutable bool cachedPlanValid = false;
    mutable std::mutex planMetaMutex;           // serializes producer writes only
    std::atomic<int> minimumPlanVersion { 0 };  // ignore meta below reset cutoff
    void writePlanMeta(int slot, int warmSlot, int version, int latency,
                       uint64_t target, int epoch) noexcept;
    bool readPlanMeta(PlanMeta& out) const noexcept;

    std::atomic<uint32_t> audioDesignMask { 0 };
    int localDesignSlot = -1;                   // audio thread
    int activeDesignSlot = -1;                  // audio thread
    int oldDesignSlot = -1;                     // audio thread
    int reserveFreeDesignSlot() noexcept;       // producer only (Free/Ready -> Reserved)
    void updateAudioDesignMask() noexcept;      // audio thread
    void releaseDesignSlot(int slot) noexcept;  // audio thread: state -> Free

    // Audio-thread-only accessor: latest published design as a raw pointer
    // into a pinned ring slot, or nullptr. No shared ownership.
    const FIRState* getPublishedStateForAudioThread();

    // GUI response snapshots (written off the audio thread, read by the GUI).
    static constexpr int NumResponseSnapshots = 16;
    mutable std::mutex responseSnapshotMutex;
    FirResponseSnapshot responseSnapshots[NumResponseSnapshots];
    int responseSnapshotNext = 0;
    void publishFirResponseSnapshot(const FIRState& state);
    std::atomic<int> activeDesignVersion { 0 };

    std::atomic<int> stateVersion { 0 };        // bumped on every publish/reset

    // ---- Warm ring (2 slots) ----
    static constexpr int NumWarmSlots = 2;
    static constexpr int WarmSlotFree = 0;
    static constexpr int WarmSlotReady = 1;
    static constexpr int WarmSlotInUse = 2;
    static constexpr int WarmSlotReserved = 3;

    struct WarmSlot
    {
        WarmState state;
    };
    WarmSlot warmSlots[NumWarmSlots];
    std::atomic<int> warmSlotStates[NumWarmSlots];
    std::atomic<uint32_t> audioWarmMask { 0 };
    int localWarmSlot = -1;                     // audio thread
    int reserveFreeWarmSlot() noexcept;         // producer only
    void updateAudioWarmMask() noexcept;        // audio thread
    void releaseWarmSlot(int slot) noexcept;    // audio thread: -> Free
    WarmState* getWarmStateForAudioThread(int designVersion);

    std::atomic<bool> warmRebuildRequested { false };

    // Bumped by prepare(); a design started before the latest prepare is
    // stale (e.g. wrong sample rate) and must not be published. The worker
    // re-checks the epoch before writing/publishing a slot.
    std::atomic<int> designEpoch { 0 };

    // Redesign request (audio thread -> background thread)
    std::atomic<bool> redesignRequested { false };
    juce::WaitableEvent redesignEvent;

    std::atomic<juce::AudioProcessorValueTreeState*> parameterSource { nullptr };  // read by bg thread
    SubEQ::EQEngine designEngine;                            // guarded by designMutex
    std::mutex designMutex;
    mutable std::mutex warmBuildMutex;   // guards warmInput/warmOutput/warmScratch
    double designSampleRate = 0.0;                           // bg-thread cache: last prepared sr
    std::atomic<bool> nonRealtime { false };                 // host is offline-rendering
    int offlineDesignCooldown = 0;                           // blocks between offline redesigns

    // Overlap-add state (audio thread only). The convolution FFT runs in
    // double precision (float FFT error ~0.4% relative would be audible).
    FirHistoryRing history;
    std::atomic<uint64_t> historyValidFrom { 0 };   // samples before this are a previous session
    std::vector<OlaChannelState> activeStates;
    std::vector<OlaChannelState> oldStates;
    std::vector<std::complex<double>> fftWork;           // shared scratch, convFFTSize
    TwiddleBank<double> twiddleBank;                     // read-only, prepared off the audio thread

    // convFFTSize/groupDelay of the design currently feeding the accumulators.
    int activeConvFFTSize = 0;
    int activeGroupDelay = 0;
    std::atomic<int> reportedActiveLatency { 0 };
    // Packed (epoch:32 | ackedVersion:32). Starts at epoch 1.
    std::atomic<uint64_t> latencyAckState { uint64_t(1) << 32 };
    std::atomic<bool> pdcGuardMirror { false };
    std::atomic<int> hostLatencySamples { 0 };
    int latencyGuardVersion = 0;
    int latencyGuardSamples = 0;

    // Audio-thread view of the adopted warm candidate's target. The published
    // WarmState is immutable: advancing its target in place from the audio
    // thread raced the worker and made later candidates look stale.
    const WarmState* audioWarmPtr = nullptr;
    uint64_t audioWarmTargetSample = 0;
    std::atomic<double> warmBuildSecondsEWMA { 0.012 };

    // stateVersion of the design last seen by process() (audio thread). A
    // newly published design starts a short crossfade from the previous
    // design instead of flushing the accumulators, so parameter edits no
    // longer produce an output step — but ONLY when the two designs share the
    // same groupDelay: both sides of the fade then sit at the same pipeline
    // delay and the mix stays time-aligned. Delay-changing designs (FIR
    // length / mode change, e.g. linear-phase groupDelay 2047 -> 8191) flush
    // the accumulators and switch immediately — crossfading them would make
    // the old tail arrive (new-old) samples early under the freshly reported
    // PDC, an audible pre-echo (F-006).
    int processedVersion = 0;

    // Crossfade state (audio thread only).
    static constexpr int CrossfadeLen = 1024;               // ≈ 21 ms @ 48 kHz
    const FIRState* activeState = nullptr;                  // pinned design slot
    const FIRState* oldState = nullptr;                     // fading old design
    int fadeConvFFTSize = 0;                                // old convFFTSize
    int crossfadeRemaining = 0;                             // remaining fade samples
    juce::AudioBuffer<float> xfadeOld, xfadeNew;            // real-input convolution outputs

    // 音频线程惰性 twiddle 预热去重（F-010）：prepareToPlay 可能运行于其他
    // 线程（thread_local 缓存未命中），音频线程首块对实际尺寸惰性预热。
    bool audioTwiddlesPrewarmed = false;

    // Background warm-up scratch (never touched by the audio thread).
    std::vector<float> warmInput;
    std::vector<float> warmOutput;
    std::vector<std::complex<double>> warmScratch;
    std::atomic<int> preparedChannels { 2 };
    std::atomic<int> currentHostBlockSamples { 512 };  // audio: current block size for warm lead
    std::vector<float> prefixScratch;   // audio: replay p samples into a warm state
    std::vector<float> warmCatchUpOutput; // audio: discard output of a bounded warm catch-up


    // Background: build the exact OLA state at a future OLA boundary from the
    // input history (overlap-save at T-M, then replay the last M samples).
    // Waits for captured history; aborts when expectedEpoch changes or the
    // worker is asked to exit. No audio-thread catch-up is ever performed.
    bool buildWarmState(const FIRState& design, WarmState& out,
                        int expectedEpoch, uint64_t targetSample);

    // Offline path: synchronously build the design coefficients and an exact
    // warm state at the current committed boundary.
    std::unique_ptr<FIRState> buildDesignSnapshot(int& outEpoch);
    bool buildWarmAtCurrentBoundary(const FIRState& design, WarmState& out);
    bool ensureOfflineReady();

    std::atomic<double> sampleRate { 48000.0 };   // written by audio thread, read by bg thread
    double preparedSampleRate = 0.0;              // audio thread: sr of the last full prepare
    int numChannels = 2;
};

} // namespace SubEQ
