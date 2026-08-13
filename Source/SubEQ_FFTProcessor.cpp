/*
  ==============================================================================

    SubEQ_FFTProcessor.cpp
    FIR coefficient design (Linear Phase + Minimum Phase) and FFT overlap-add
    convolution. Coefficient design runs on a background thread with the shared
    radix-2 FFT (SubEQ_DSPMath.h) — JUCE FFT wrapper is unreliable here.

  ==============================================================================
*/

#include "SubEQ_FFTProcessor.h"
#include "SubEQ_DSPMath.h"
#include "SubEQ_Parameters.h"
#include "SubEQ_FIRDesign.h"

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
    // 50 ms and a single design (longest FIR) completes in well under a
    // second, so 5 s leaves ample margin against scheduler stalls.
    if (!stopThread (5000))
        jassertfalse;   // worker did not exit in time — would be a lifetime bug
}

//==============================================================================
void FFTProcessor::prepare(double sr, int maxBlockSize, int channels)
{
    juce::ignoreUnused(maxBlockSize);
    sampleRate.store(sr, std::memory_order_release);

    // Same session parameters: keep the buffers and any published design.
    // Hosts commonly re-call prepareToPlay (transport stop/start, loop
    // points) with an unchanged setup; dropping the published design would
    // mute the output until the background redesign completes. The design
    // epoch is deliberately NOT bumped here: an in-flight design was built
    // from the same sample rate and is still valid — discarding it would
    // silently revert the user's latest parameter edit.
    if (sr == preparedSampleRate && channels == numChannels && !fftWork.empty())
        return;

    // Any design started before this prepare is stale (it may have snapshotted
    // a previous sample rate) and must not be published.
    designEpoch.fetch_add(1, std::memory_order_release);

    preparedSampleRate = sr;
    numChannels = channels;

    // Pre-allocate for the longest FIR so the audio thread never allocates
    // when a new design is published mid-stream.
    const int maxL = convolutionFftSize(MaxFirLength, ConvBlockLen);

    channelStates.resize(numChannels);
    for (auto& ch : channelStates)
    {
        ch.inputBlock.assign(ConvBlockLen, { 0.0, 0.0 });
        ch.pos = 0;
        ch.overlap.assign(maxL, 0.0);
        ch.outQueue.clear();
        ch.outQueue.reserve(ConvBlockLen * 2);
        ch.outRead = 0;
    }
    fftWork.assign(maxL, { 0.0, 0.0 });
    activeConvFFTSize = 0;

    // Pre-build the twiddle tables for every selectable convolution size on
    // this thread, so the first real-time FFT does not allocate (the cache
    // is thread_local; prepareToPlay normally runs on the audio thread).
    for (int i = 0; i < NumFirLengthChoices; ++i)
        prewarmTwiddleTable<double>(convolutionFftSize(FirLengthChoices[i], ConvBlockLen));

    // Drop any previously published design: the engine will be redesigned
    // once the mode/parameters demand it.
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        currentState.reset();
    }
    stateVersion.fetch_add(1, std::memory_order_release);

    // The published design was just dropped. If the plugin is in a FIR mode
    // the audio thread would otherwise output silence (or stale coefficients)
    // until the user touches a parameter again — request a redesign right
    // away. Non-blocking and audio-thread safe; the worker ignores the
    // request in Zero Latency mode.
    requestRedesign();
}

void FFTProcessor::reset()
{
    for (auto& ch : channelStates)
    {
        std::fill(ch.inputBlock.begin(), ch.inputBlock.end(), std::complex<double>(0.0, 0.0));
        ch.pos = 0;
        std::fill(ch.overlap.begin(), ch.overlap.end(), 0.0);
        ch.outQueue.clear();
        ch.outRead = 0;
    }
}

void FFTProcessor::setParameterSource(juce::AudioProcessorValueTreeState* source)
{
    parameterSource.store(source, std::memory_order_release);
    startThread();
}

void FFTProcessor::requestRedesign()
{
    redesignRequested.store(true, std::memory_order_release);
    redesignEvent.signal();
}

//==============================================================================
// Background thread: wait for a redesign request, build a design-time engine
// snapshot from the parameter source, design the FIR + frequency coefficients
// and publish them.
void FFTProcessor::run()
{
    while (!threadShouldExit())
    {
        redesignEvent.wait(50);

        if (!redesignRequested.exchange(false, std::memory_order_acquire))
            continue;

        auto* source = parameterSource.load(std::memory_order_acquire);
        if (source == nullptr)
            continue;

        const auto mode = static_cast<EQMode> (
            static_cast<int> (source->getRawParameterValue ("eq_mode")->load()));

        if (mode == EQMode::ZeroLatency)
            continue;

        const int firChoice = static_cast<int> (
            source->getRawParameterValue ("fir_length")->load());
        const int firLength = (firChoice >= 0 && firChoice < NumFirLengthChoices)
                                  ? FirLengthChoices[firChoice]
                                  : DefaultFirLength;

        // Capture the design epoch before snapshotting anything: if prepare()
        // runs while this design is in flight, the design is stale (e.g. it
        // snapshotted a previous sample rate) and must be discarded, not
        // published — otherwise the audio thread would consume coefficients
        // computed for the wrong sample rate until the next redesign.
        const int epoch = designEpoch.load(std::memory_order_acquire);

        // Build an engine snapshot owned by this thread; the parameter values
        // are read atomically from the APVTS, so no lock is needed.
        const double sr = sampleRate.load(std::memory_order_acquire);
        if (sr != designSampleRate)
        {
            designEngine.prepare(sr, 512);
            designSampleRate = sr;
        }
        applyParametersToEngine(*source, designEngine);

        auto newState = std::make_shared<FIRState>();
        newState->firLength = firLength;

        auto magnitude = [&](double w) { return designEngine.getMagnitudeLinear(w); };
        if (mode == EQMode::LinearPhase)
            newState->coeffs = designLinearPhaseFIR(magnitude, firLength, newState->groupDelay);
        else
            newState->coeffs = designMinimumPhaseFIR(magnitude, firLength, newState->groupDelay);

        // Pre-compute the frequency-domain filter for overlap-add convolution
        newState->convFFTSize = convolutionFftSize(firLength, ConvBlockLen);
        std::vector<std::complex<double>> spec(newState->convFFTSize);
        for (int i = 0; i < firLength; ++i)
            spec[i] = { static_cast<double> (newState->coeffs[i]), 0.0 };
        for (int i = firLength; i < newState->convFFTSize; ++i)
            spec[i] = { 0.0, 0.0 };
        fftInPlace(spec.data(), newState->convFFTSize);

        newState->freqCoeffs = spec;   // double precision frequency response

        // A prepare() during the design invalidated it — drop the result.
        if (epoch != designEpoch.load(std::memory_order_acquire))
            continue;

        // Publish (release) so the audio thread sees a fully-formed state.
        // The replaced state is moved into the retirement slot: its (up to
        // ~2.3 MB) destructor then runs here on the background thread instead
        // of on the audio thread at its next publication pick-up.
        std::shared_ptr<const FIRState> previous;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            previous = std::move(retiredState);
            retiredState = currentState;
            currentState = newState;
        }
        // `previous` (the state retired before this publish) is destroyed here,
        // on the background thread, outside the lock — the critical section now
        // only swaps pointers instead of running the ~2.3 MB deallocation.
        stateVersion.fetch_add(1, std::memory_order_release);
    }
}

//==============================================================================
std::shared_ptr<const FFTProcessor::FIRState> FFTProcessor::getPublishedStateForAudioThread() const
{
    // Fast path: no new publish since we last looked — zero locks.
    if (stateVersion.load(std::memory_order_acquire) != lastSeenVersion)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        localState = currentState;
        lastSeenVersion = stateVersion.load(std::memory_order_relaxed);
    }

    return localState;
}

//==============================================================================
void FFTProcessor::process(juce::AudioBuffer<float>& buffer)
{
    auto state = getPublishedStateForAudioThread();
    if (state == nullptr || state->freqCoeffs.empty())
        return;

    const int L = state->convFFTSize;
    if (L <= 0 || L > static_cast<int>(fftWork.size()))
        return;

    // A newly published design (parameter edit, FIR length or mode change)
    // invalidates everything the accumulators hold. The overlap tail and the
    // pending output FIFO were computed with the previous coefficients — up
    // to FIR-length-1 samples of old-filter audio (over a second for a
    // 65536-tap FIR at 48 kHz) — and must be dropped, otherwise echoes of the
    // previous filter contaminate the new output. The old check only flushed
    // on a convFFTSize change, so same-length redesigns (the common knob
    // drag) mixed stale tails with fresh output.
    const int version = stateVersion.load(std::memory_order_acquire);
    if (version != processedVersion || L != activeConvFFTSize)
    {
        for (auto& ch : channelStates)
        {
            std::fill(ch.overlap.begin(), ch.overlap.end(), 0.0);
            ch.outQueue.clear();
            ch.outRead = 0;
        }
        activeConvFFTSize = L;
        processedVersion = version;
    }

    const int channelCount = std::min(buffer.getNumChannels(), numChannels);
    const int numSamples = buffer.getNumSamples();

    for (int ch = 0; ch < channelCount; ++ch)
    {
        auto* data = buffer.getWritePointer(ch);
        processOlaChannel(channelStates[ch], data, data, numSamples,
                          ConvBlockLen, L, state->freqCoeffs.data(), fftWork);
    }
}

//==============================================================================
int FFTProcessor::getLatencySamples() const
{
    auto state = getPublishedStateForAudioThread();
    if (state == nullptr)
        return 0;
    // Overlap-add pipeline delays the output by ConvBlockLen-1 samples
    // (the first output sample appears after the first complete block).
    return state->groupDelay + (ConvBlockLen - 1);
}

bool FFTProcessor::isReady() const
{
    auto state = getPublishedStateForAudioThread();
    return (state != nullptr) && !state->freqCoeffs.empty();
}

int FFTProcessor::getTailLengthSamples() const
{
    // Lock-guarded direct read — may be called from the host/GUI thread, so it
    // must not touch the audio-thread localState cache.
    std::lock_guard<std::mutex> lock(stateMutex);
    if (currentState == nullptr)
        return DefaultFirLength + ConvBlockLen;
    return currentState->firLength + ConvBlockLen;
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
