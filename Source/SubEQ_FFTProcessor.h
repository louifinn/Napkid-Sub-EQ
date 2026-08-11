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

    // Fixed latency of a length-N Linear Phase FIR (group delay (N-1)/2).
    static int linearPhaseLatencyFor(int firLength) { return (firLength - 1) / 2; }

    FFTProcessor();
    ~FFTProcessor() override;

    // Audio thread
    void prepare(double sampleRate, int maxBlockSize, int numChannels);
    void reset();

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

    void designLinearPhaseFIR(const EQEngine& eqEngine, int firLength,
                              std::vector<float>& coeffsOut, int& latencyOut);
    void designMinimumPhaseFIR(const EQEngine& eqEngine, int firLength,
                               std::vector<float>& coeffsOut, int& latencyOut);

    // Published FIR state (written by the background thread, read by the
    // audio thread). Immutable once published — safe to share.
    struct FIRState
    {
        std::vector<float> coeffs;                       // time-domain, firLength taps
        std::vector<std::complex<double>> freqCoeffs;    // FFT(h) over convFFTSize (double)
        int firLength = DefaultFirLength;
        int convFFTSize = 0;
        // FIR bulk delay used for PDC (no block latency). Linear Phase:
        // (N-1)/2. Minimum Phase: 0 — a min-phase FIR's direct sound sits at
        // tap 0, and reporting the max group delay over-compensated (the
        // plugin played EARLY against other tracks, with PDC churn on edits).
        int groupDelay = 0;
    };

    // Audio-thread accessor: returns the latest published state.
    std::shared_ptr<const FIRState> getPublishedState() const;

    mutable std::mutex stateMutex;                               // guards currentState (rarely contended)
    mutable std::shared_ptr<const FIRState> currentState;        // written by bg thread under lock
    std::atomic<int> stateVersion { 0 };                         // bumped on every publish (release)
    mutable std::shared_ptr<const FIRState> localState;          // audio-thread cache
    mutable int lastSeenVersion = 0;                             // audio-thread cache

    // Background-thread retirement slot: holds the previously published state
    // so its (up to ~2.3 MB) destructor runs on the background thread, not on
    // the audio thread when it drops its last reference.
    std::shared_ptr<const FIRState> retiredState;

    // Bumped by prepare(); a design started before the latest prepare is
    // stale (e.g. wrong sample rate) and must not be published.
    std::atomic<int> designEpoch { 0 };

    // Redesign request (audio thread -> background thread)
    std::atomic<bool> redesignRequested { false };
    juce::WaitableEvent redesignEvent;

    std::atomic<juce::AudioProcessorValueTreeState*> parameterSource { nullptr };  // read by bg thread
    SubEQ::EQEngine designEngine;                            // background-thread private
    double designSampleRate = 0.0;                           // bg-thread cache: last prepared sr

    // Overlap-add state (audio thread only). The convolution FFT runs in
    // double precision (float FFT error ~0.4% relative would be audible).
    std::vector<std::vector<std::complex<double>>> inputBlocks;  // per channel, ConvBlockLen
    std::vector<int> inputPos;                                 // per channel
    std::vector<std::vector<double>> overlapBufs;              // per channel, convFFTSize
    std::vector<std::vector<double>> outBufs;                  // per channel output FIFO
    std::vector<int> outReadPos;                               // per channel
    std::vector<std::complex<double>> fftWork;                 // shared scratch, convFFTSize

    // convFFTSize of the state currently feeding the overlap accumulators.
    // When a newly published state changes it (FIR length switch), the stale
    // tails must be flushed — they are laid out for the old size and would
    // otherwise revive old audio as echoes.
    int activeConvFFTSize = 0;

    // stateVersion of the design the overlap/output FIFOs were last flushed
    // for (audio thread only). A newly published design — parameter edit, FIR
    // length or mode change — invalidates both the accumulated convolution
    // tail and the pending output FIFO (old-filter audio), so process() drops
    // them on the first block it sees the new version.
    int processedVersion = 0;

    std::atomic<double> sampleRate { 48000.0 };   // written by audio thread, read by bg thread
    double preparedSampleRate = 0.0;              // audio thread: sr of the last full prepare
    int numChannels = 2;
};

} // namespace SubEQ
