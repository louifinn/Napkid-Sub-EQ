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

    inputBlocks.assign(numChannels, std::vector<std::complex<double>>(ConvBlockLen, { 0.0, 0.0 }));
    inputPos.assign(numChannels, 0);
    overlapBufs.assign(numChannels, std::vector<double>(maxL, 0.0));
    outBufs.assign(numChannels, std::vector<double>());
    outReadPos.assign(numChannels, 0);
    for (auto& ob : outBufs)
        ob.reserve(ConvBlockLen * 2);
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
    for (auto& ib : inputBlocks)
        std::fill(ib.begin(), ib.end(), std::complex<double>(0.0, 0.0));
    std::fill(inputPos.begin(), inputPos.end(), 0);

    for (auto& ob : overlapBufs)
        std::fill(ob.begin(), ob.end(), 0.0);

    for (auto& ob : outBufs)
        ob.clear();
    std::fill(outReadPos.begin(), outReadPos.end(), 0);
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

        if (mode == EQMode::LinearPhase)
            designLinearPhaseFIR(designEngine, firLength, newState->coeffs, newState->groupDelay);
        else
            designMinimumPhaseFIR(designEngine, firLength, newState->coeffs, newState->groupDelay);

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
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            retiredState = currentState;
            currentState = newState;
        }
        stateVersion.fetch_add(1, std::memory_order_release);
    }
}

//==============================================================================
std::shared_ptr<const FFTProcessor::FIRState> FFTProcessor::getPublishedState() const
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
void FFTProcessor::designLinearPhaseFIR(const EQEngine& eqEngine, int firLength,
                                        std::vector<float>& coeffsOut, int& latencyOut)
{
    std::vector<std::complex<double>> spectrum(firLength);
    const double delayPhaseFactor = -juce::MathConstants<double>::pi * (firLength - 1) / firLength;

    // Step 1: magnitude response with linear phase term for strict symmetry.
    // For even-length Type-II FIR, the Nyquist response (i = N/2) must be 0.
    for (int i = 0; i < firLength / 2; ++i)
    {
        double w = juce::MathConstants<double>::pi * i / (firLength / 2);
        double mag = eqEngine.getMagnitudeLinear(w);
        double phase = delayPhaseFactor * i;
        spectrum[i] = { mag * std::cos(phase), mag * std::sin(phase) };
    }
    spectrum[firLength / 2] = { 0.0, 0.0 }; // Type-II FIR: H(pi) = 0

    // Conjugate symmetry for second half
    for (int i = firLength / 2 + 1; i < firLength; ++i)
    {
        spectrum[i] = std::conj(spectrum[firLength - i]);
    }

    // Step 2: IDFT -> time-domain coefficients
    ifftInPlace(spectrum.data(), firLength);

    coeffsOut.resize(firLength);
    float cmax = 0.0f;
    for (int i = 0; i < firLength; ++i)
    {
        coeffsOut[i] = static_cast<float>(spectrum[i].real());
        cmax = std::max(cmax, std::abs(coeffsOut[i]));
    }
    DBG("[SubEQ DBG] LinearPhase coeff peak=" + juce::String(cmax, 2));

    latencyOut = linearPhaseLatencyFor(firLength);
}

//==============================================================================
void FFTProcessor::designMinimumPhaseFIR(const EQEngine& eqEngine, int firLength,
                                         std::vector<float>& coeffsOut, int& latencyOut)
{
    const double epsilon = 1.0e-12;
    std::vector<std::complex<double>> spectrum(firLength);

    // Step 1: log magnitude spectrum
    for (int i = 0; i <= firLength / 2; ++i)
    {
        double w = juce::MathConstants<double>::pi * i / (firLength / 2);
        double mag = eqEngine.getMagnitudeLinear(w);
        spectrum[i] = { std::log(mag + epsilon), 0.0 };
    }
    for (int i = firLength / 2 + 1; i < firLength; ++i)
    {
        spectrum[i] = { spectrum[firLength - i].real(), 0.0 };
    }

    // Step 2: IDFT -> cepstrum
    std::vector<std::complex<double>> cepstrum = spectrum;
    ifftInPlace(cepstrum.data(), firLength);

    // Step 3: Causalize cepstrum
    cepstrum[0] = { cepstrum[0].real(), 0.0 };
    for (int i = 1; i < firLength / 2; ++i)
        cepstrum[i] = { cepstrum[i].real() * 2.0, 0.0 };
    cepstrum[firLength / 2] = { 0.0, 0.0 };
    for (int i = firLength / 2 + 1; i < firLength; ++i)
        cepstrum[i] = { 0.0, 0.0 };

    // Step 4: DFT -> minimum phase log spectrum
    fftInPlace(cepstrum.data(), firLength);

    // Step 5: exp() -> minimum phase complex spectrum
    for (int i = 0; i < firLength; ++i)
    {
        const double re = cepstrum[i].real();
        const double im = cepstrum[i].imag();
        const double expRe = std::exp(re) * std::cos(im);
        const double expIm = std::exp(re) * std::sin(im);
        spectrum[i] = { expRe, expIm };
    }

    // Step 6: IDFT -> minimum phase FIR coefficients
    ifftInPlace(spectrum.data(), firLength);

    coeffsOut.resize(firLength);
    bool hasInvalid = false;
    float cmax = 0.0f;
    for (int i = 0; i < firLength; ++i)
    {
        float val = static_cast<float>(spectrum[i].real());
        if (std::isnan(val) || std::isinf(val))
            hasInvalid = true;
        coeffsOut[i] = val;
        cmax = std::max(cmax, std::abs(val));
    }
    DBG("[SubEQ DBG] MinimumPhase coeff peak=" + juce::String(cmax, 2)
        + " hasInvalid=" + juce::String((int)hasInvalid));

    // Fallback to impulse if cepstral method produced invalid coefficients
    if (hasInvalid)
    {
        std::fill(coeffsOut.begin(), coeffsOut.end(), 0.0f);
        coeffsOut[0] = 1.0f;
    }

    // Minimum-phase PDC reports no FIR bulk delay: the cepstral design places
    // the direct sound at tap 0, so only the overlap-add block latency applies
    // (added by getLatencySamples). Reporting the max group delay instead
    // over-compensated — low-frequency group-delay spikes could exceed the
    // Linear Phase latency, making this "minimum phase" mode play EARLY
    // against other tracks, and the value churned on every parameter edit.
    latencyOut = 0;
}

//==============================================================================
void FFTProcessor::process(juce::AudioBuffer<float>& buffer)
{
    auto state = getPublishedState();
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
        for (auto& ob : overlapBufs)
            std::fill(ob.begin(), ob.end(), 0.0);
        for (auto& ob : outBufs)
            ob.clear();
        std::fill(outReadPos.begin(), outReadPos.end(), 0);
        activeConvFFTSize = L;
        processedVersion = version;
    }

    const int channels = std::min(buffer.getNumChannels(), numChannels);

    for (int ch = 0; ch < channels; ++ch)
    {
        auto* data = buffer.getWritePointer(ch);
        auto& inputBlock = inputBlocks[ch];
        auto& overlap = overlapBufs[ch];
        auto& outBuf = outBufs[ch];
        int& pos = inputPos[ch];
        int& outRead = outReadPos[ch];

        const int numSamples = buffer.getNumSamples();

        for (int n = 0; n < numSamples; ++n)
        {
            inputBlock[pos] = { static_cast<double> (data[n]), 0.0 };
            ++pos;

            if (pos == ConvBlockLen)
            {
                // ---- Overlap-add step for this input block ----
                for (int i = 0; i < ConvBlockLen; ++i)
                    fftWork[i] = inputBlock[i];
                for (int i = ConvBlockLen; i < L; ++i)
                    fftWork[i] = { 0.0, 0.0 };

                fftInPlace(fftWork.data(), L);
                for (int i = 0; i < L; ++i)
                    fftWork[i] *= state->freqCoeffs[i];
                ifftInPlace(fftWork.data(), L);

                // Output block = Y[0..M-1] + accumulated tail
                bool poisoned = false;
                for (int i = 0; i < ConvBlockLen; ++i)
                {
                    double y = fftWork[i].real() + overlap[i];
                    if (std::isnan(y) || std::isinf(y))
                    {
                        y = 0.0;
                        poisoned = true;
                    }
                    outBuf.push_back(y);
                }

                if (poisoned)
                {
                    // NaN/Inf must not recirculate through the accumulator:
                    // flush it so one bad block does not mute the output
                    // until the next manual reset.
                    std::fill(overlap.begin(), overlap.end(), 0.0);
                }
                else
                {
                    // Carry the tail into the next block: shift the accumulator
                    // and ADD this block's tail (a convolution spans multiple
                    // blocks, so older tails must accumulate).
                    for (int i = 0; i < L - ConvBlockLen; ++i)
                        overlap[i] = overlap[i + ConvBlockLen]
                                     + fftWork[ConvBlockLen + i].real();
                    for (int i = L - ConvBlockLen; i < L; ++i)
                        overlap[i] = 0.0;
                }

                pos = 0;
            }

            // Drain one output sample (zero during the initial block latency)
            if (outRead < static_cast<int>(outBuf.size()))
                data[n] = static_cast<float>(outBuf[outRead++]);
            else
                data[n] = 0.0f;

            if (outRead == static_cast<int>(outBuf.size()))
            {
                outBuf.clear();
                outRead = 0;
            }
        }
    }
}

//==============================================================================
int FFTProcessor::getLatencySamples() const
{
    auto state = getPublishedState();
    if (state == nullptr)
        return 0;
    // Overlap-add pipeline delays the output by ConvBlockLen-1 samples
    // (the first output sample appears after the first complete block).
    return state->groupDelay + (ConvBlockLen - 1);
}

bool FFTProcessor::isReady() const
{
    auto state = getPublishedState();
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
