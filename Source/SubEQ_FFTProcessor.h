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
#include "SubEQ_FFTConvolver.h"
#include "SubEQ_FIRHistory.h"

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
    static constexpr int MaxLatencySamples = (MaxFirLength - 1) / 2 + ConvBlockLen - 1;


    FFTProcessor();
    ~FFTProcessor() override;

    // Audio thread
    void prepare(double sampleRate, int maxBlockSize, int numChannels);

    // Clear convolution/design state but keep the raw input history (used on
    // mode switches, so re-entering FIR can warm the next design from the
    // audio that played in Zero Latency).
    void resetConvolution();

    // Capture the raw input for the warm-up history. Called for every mode;
    // publishNow=false defers visibility until publishHistory() at block end.
    void captureHistory(const float* const* channels, int numChannels, int numSamples,
                        bool publishNow = true);
    void publishHistory() noexcept { history.publish(); }

    // Drop the published design without destroying it here: used when
    // (re-)entering a FIR mode, so the audio thread outputs silence until
    // the next design is published instead of serving a stale curve.
    // Audio-thread safe — pointer moves under the lock only; destruction
    // happens on the background thread via the retirement ring.
    void clearPublishedState();

    // Provide the parameter source used by the background thread to build the
    // design-time EQ engine snapshot (apvts must outlive this processor).
    void setParameterSource(juce::AudioProcessorValueTreeState* source);

    // Ask the background thread to redesign the FIR from current parameters.
    // Non-blocking; the new coefficients are published asynchronously.
    void requestRedesign();

    // Process entire audio buffer (all channels). Returns true when an active
    // FIR design wrote the buffer; false when no design is active yet (the
    // buffer is left untouched so the caller can keep the IIR path alive).
    bool process(juce::AudioBuffer<float>& buffer);
    bool hasActiveDesign() const noexcept { return activeState != nullptr; }

    // Latency/readiness of the design actually active in the audio path.
    // getLatencySamples() includes the overlap-add block latency.
    int getLatencySamples() const;

    // Latency of the latest published design (may not be active yet). Used by
    // the PDC handshake: report first, acknowledge, then let the audio switch.
    int getPublishedLatencySamples() const noexcept
    {
        return publishedLatency.load(std::memory_order_acquire);
    }
    void acknowledgeLatency(int version) noexcept
    {
        latencyAckVersion.store(version, std::memory_order_release);
    }

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

    std::shared_ptr<const FIRState> getFirDesignSnapshot() const;

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
        int blockIndex = -1;
        int convFFTSize = 0;
        int groupDelay = 0;
    };

    // Audio-thread-only accessor: returns the latest published state. Owns
    // localState/lastSeenVersion; must never be called from the GUI/host
    // thread — cross-thread reads go through getTailLengthSamples().
    std::shared_ptr<const FIRState> getPublishedStateForAudioThread() const;

    mutable std::mutex stateMutex;                               // guards currentState/currentWarm (rarely contended)
    mutable std::shared_ptr<const FIRState> currentState;        // written by bg thread under lock
    std::atomic<int> stateVersion { 0 };                         // bumped on every publish (release)
    mutable std::shared_ptr<const FIRState> localState;          // audio-thread cache
    mutable int lastSeenVersion = 0;                             // audio-thread cache

    mutable std::shared_ptr<WarmState> currentWarm;              // bg thread under lock
    std::atomic<int> warmVersion { 0 };                          // bumped when currentWarm changes
    mutable std::shared_ptr<WarmState> localWarm;                // audio-thread cache
    mutable int lastSeenWarmVersion = 0;                         // audio-thread cache
    std::atomic<bool> warmRebuildRequested { false };

    // Retirement rings (guarded by stateMutex): the audio thread hands every
    // superseded design/warm state here instead of destroying it. The
    // background drains them outside the lock. If a ring is full the audio
    // thread keeps the value in its local cache and retries next block; it
    // never drops a large object on the audio thread.
    static constexpr int NumRetireSlots = 64;
    static constexpr int NumWarmRetireSlots = 8;
    mutable std::shared_ptr<const FIRState> pendingDestroy[NumRetireSlots];
    mutable int pendingDestroyHead = 0;               // audio-thread append index
    mutable std::shared_ptr<WarmState> pendingWarmDestroy[NumWarmRetireSlots];
    mutable int pendingWarmDestroyHead = 0;

    // Bumped by prepare(); a design started before the latest prepare is
    // stale (e.g. wrong sample rate) and must not be published. The validity
    // re-check happens INSIDE the publish critical section (F-001): prepare()
    // bumps the epoch before resetting currentState under the same lock, so
    // a stale design can never be published over the just-reset state.
    std::atomic<int> designEpoch { 0 };

    // Redesign request (audio thread -> background thread)
    std::atomic<bool> redesignRequested { false };
    juce::WaitableEvent redesignEvent;

    std::atomic<juce::AudioProcessorValueTreeState*> parameterSource { nullptr };  // read by bg thread
    SubEQ::EQEngine designEngine;                            // background-thread private
    double designSampleRate = 0.0;                           // bg-thread cache: last prepared sr

    // Overlap-add state (audio thread only). The convolution FFT runs in
    // double precision (float FFT error ~0.4% relative would be audible).
    FirHistoryRing history;
    std::atomic<uint64_t> historyValidFrom { 0 };   // samples before this are a previous session
    std::vector<OlaChannelState> activeStates;
    std::vector<OlaChannelState> oldStates;
    std::vector<std::complex<double>> fftWork;           // shared scratch, convFFTSize

    // convFFTSize/groupDelay of the design currently feeding the accumulators.
    int activeConvFFTSize = 0;
    int activeGroupDelay = 0;
    int currentOlaBlockIndex = 0;
    std::atomic<int> reportedActiveLatency { 0 };
    std::atomic<int> publishedLatency { 0 };
    std::atomic<int> latencyAckVersion { 0 };
    int latencyGuardVersion = 0;
    int latencyGuardSamples = 0;
    int latencyWaitVersion = 0;
    int latencyWaitSamples = 0;

    // First-design fallback: if no warm state can be adopted for this long,
    // start from a zero-state OLA and fade in over 1024 samples rather than
    // staying silent forever. Only applies when there is no active design.
    int firstDesignWaitSamples = 0;
    int firstDesignWaitVersion = -1;
    int startupFadeRemaining = 0;
    static constexpr int MaxWarmCatchUpBlocks = 4;

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
    std::shared_ptr<const FIRState> activeState;            // current design
    std::shared_ptr<const FIRState> oldState;               // fading old design
    int fadeConvFFTSize = 0;                                // old convFFTSize
    int crossfadeRemaining = 0;                             // remaining fade samples
    juce::AudioBuffer<float> xfadeOld, xfadeNew;            // real-input convolution outputs

    // 音频线程惰性 twiddle 预热去重（F-010）：prepareToPlay 可能运行于其他
    // 线程（thread_local 缓存未命中），音频线程首块对实际尺寸惰性预热。
    int twiddlePrewarmedSize = 0;
    bool audioTwiddlesPrewarmed = false;

    // Background warm-up scratch (never touched by the audio thread).
    std::vector<float> warmInput;
    std::vector<float> warmOutput;
    std::vector<std::complex<double>> warmScratch;
    int warmFeedSamples = 0;
    std::atomic<int> preparedChannels { 2 };
    std::vector<float> prefixScratch;   // audio: replay p samples into a warm state
    std::vector<float> catchUpInput;    // audio: one OLA block of history
    std::vector<float> catchUpOutput;   // audio: discarded warm catch-up output
    std::vector<std::complex<double>> catchUpScratch;   // audio: catch-up FFT scratch

    // Audio-thread deferred retirements (ring was full): retried next block.
    mutable std::shared_ptr<const FIRState> deferredDesignRetire;
    mutable std::shared_ptr<WarmState> deferredWarmRetire;

    // Hand superseded designs/warm states to the background; never destroy a
    // large object on the audio thread. Returns false when the ring is full
    // (the caller keeps its local reference and retries next block).
    bool retireState(std::shared_ptr<const FIRState>& stateToRetire) const;
    bool retireWarm(std::shared_ptr<WarmState>& warmToRetire) const;
    std::shared_ptr<WarmState> getWarmStateForAudioThread(int designVersion) const;

    // Background: build the exact OLA state at the latest completed OLA block
    // boundary from the input history.
    bool buildWarmState(const FIRState& design, std::shared_ptr<WarmState>& out);

    // Audio: advance a warm state from its blockIndex to targetBlock by
    // replaying the missing history blocks. Bounded by MaxWarmCatchUpBlocks.
    bool catchUpWarmState(WarmState& warm, const FIRState& design, int targetBlock);

    // Flush the active OLA accumulators (delay-changing switch).
    void flushConvolvers();

    std::atomic<double> sampleRate { 48000.0 };   // written by audio thread, read by bg thread
    double preparedSampleRate = 0.0;              // audio thread: sr of the last full prepare
    int numChannels = 2;
};

} // namespace SubEQ
