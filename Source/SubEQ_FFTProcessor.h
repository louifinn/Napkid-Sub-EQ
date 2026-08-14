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


    FFTProcessor();
    ~FFTProcessor() override;

    // Audio thread
    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    void reset();

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

    // Process entire audio buffer (all channels)
    void process(juce::AudioBuffer<float>& buffer);

    // Latency/readiness of the currently published coefficient set.
    // getLatencySamples() includes the overlap-add block latency.
    int getLatencySamples() const;
    bool isReady() const;

    // Total pipeline tail (FIR length + block latency), for tail-length reporting
    int getTailLengthSamples() const;

    static juce::StringArray getModeChoices();
    static juce::String getModeName(EQMode mode);
    static juce::String getLatencyText(int latencySamples, double sampleRate);

private:
    void run() override;

    // Published FIR state (written by the background thread, read by the
    // audio thread). Immutable once published — safe to share.
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
    };

    // Audio-thread-only accessor: returns the latest published state. Owns
    // localState/lastSeenVersion; must never be called from the GUI/host
    // thread — cross-thread reads go through getTailLengthSamples().
    std::shared_ptr<const FIRState> getPublishedStateForAudioThread() const;

    mutable std::mutex stateMutex;                               // guards currentState (rarely contended)
    mutable std::shared_ptr<const FIRState> currentState;        // written by bg thread under lock
    std::atomic<int> stateVersion { 0 };                         // bumped on every publish (release)
    mutable std::shared_ptr<const FIRState> localState;          // audio-thread cache
    mutable int lastSeenVersion = 0;                             // audio-thread cache

    // Retirement ring (guarded by stateMutex): the audio thread hands each
    // superseded state here instead of destroying it, and the background
    // thread drains the slots at the top of its loop — so no large
    // (~2.3 MB) FIRState deallocation ever runs on the audio thread. The
    // ring is large enough that a publish burst between two background
    // drains cannot realistically overflow it.
    static constexpr int NumRetireSlots = 64;
    mutable std::shared_ptr<const FIRState> pendingDestroy[NumRetireSlots];
    mutable int pendingDestroyHead = 0;               // audio-thread append index

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
    std::vector<OlaChannelState> channelStates;          // per channel
    std::vector<std::complex<double>> fftWork;           // shared scratch, convFFTSize

    // convFFTSize of the state currently feeding the overlap accumulators.
    int activeConvFFTSize = 0;

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

    // 淡化期间发布的新设计（音频线程独占，F-004）：同延迟设计先挂起，
    // 等当前淡化收尾后再切换，避免新侧在混合权重下中途跳变；延迟不同
    // 的设计不挂起——立即收尾淡化并冲刷切换（见 process()）。
    std::shared_ptr<const FIRState> pendingState;

    // Crossfade state (audio thread only).
    static constexpr int CrossfadeLen = 1024;               // ≈ 21 ms @ 48 kHz
    std::vector<OlaChannelState> oldChannelStates;          // 旧设计的累加器/FIFO
    std::shared_ptr<const FIRState> oldState;               // 淡化期间钉住旧设计
    std::shared_ptr<const FIRState> processingState;        // 上一块实际使用的设计
    int fadeConvFFTSize = 0;                                // 旧设计的 convFFTSize
    int crossfadeRemaining = 0;                             // 剩余淡化样本数（跨声道共享）
    juce::AudioBuffer<float> xfadeOld, xfadeNew;            // 淡化混合的整块临时缓冲

    // 音频线程惰性 twiddle 预热去重（F-010）：prepareToPlay 可能运行于其他
    // 线程（thread_local 缓存未命中），音频线程首块对实际尺寸惰性预热。
    int twiddlePrewarmedSize = 0;

    // 把被取代的设计移交退役环（后台线程在锁外析构）。音频线程只做指针
    // 搬移，~2.3 MB 的 FIRState 释放永不发生在锁内/音频线程（F-002）。
    void retireState(std::shared_ptr<const FIRState>& stateToRetire);

    // 冲刷新侧卷积累加器与输出队列（延迟不同的设计切换用，见 process()）。
    void flushConvolvers();

    std::atomic<double> sampleRate { 48000.0 };   // written by audio thread, read by bg thread
    double preparedSampleRate = 0.0;              // audio thread: sr of the last full prepare
    int numChannels = 2;
};

} // namespace SubEQ
