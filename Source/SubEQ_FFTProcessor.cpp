/*
  ==============================================================================

    SubEQ_FFTProcessor.cpp
    FIR coefficient design (Linear Phase + Minimum Phase) and convolution.
    Uses direct time-domain convolution with manual delay lines.
    DFT/IDFT computed directly — JUCE FFT wrapper is unreliable in this build.

  ==============================================================================
*/

#include "SubEQ_FFTProcessor.h"

namespace SubEQ
{

namespace
{
    // Direct DFT: X[k] = Σ x[n] * e^(-j*2π*k*n/N)
    void dft(const juce::dsp::Complex<float>* in,
             juce::dsp::Complex<float>* out, int N)
    {
        const float twoPiOverN = 2.0f * juce::MathConstants<float>::pi / static_cast<float>(N);
        for (int k = 0; k < N; ++k)
        {
            float real = 0.0f, imag = 0.0f;
            for (int n = 0; n < N; ++n)
            {
                float angle = -twoPiOverN * static_cast<float>(k * n);
                float c = std::cos(angle);
                float s = std::sin(angle);
                real += in[n].real() * c - in[n].imag() * s;
                imag += in[n].real() * s + in[n].imag() * c;
            }
            out[k] = { real, imag };
        }
    }

    // Direct IDFT: x[n] = (1/N) * Σ X[k] * e^(j*2π*k*n/N)
    void idft(const juce::dsp::Complex<float>* in,
              juce::dsp::Complex<float>* out, int N)
    {
        const float twoPiOverN = 2.0f * juce::MathConstants<float>::pi / static_cast<float>(N);
        const float invN = 1.0f / static_cast<float>(N);
        for (int n = 0; n < N; ++n)
        {
            float real = 0.0f, imag = 0.0f;
            for (int k = 0; k < N; ++k)
            {
                float angle = twoPiOverN * static_cast<float>(k * n);
                float c = std::cos(angle);
                float s = std::sin(angle);
                real += in[k].real() * c - in[k].imag() * s;
                imag += in[k].real() * s + in[k].imag() * c;
            }
            out[n] = { real * invN, imag * invN };
        }
    }
}

//==============================================================================
FFTProcessor::FFTProcessor() = default;
FFTProcessor::~FFTProcessor() = default;

//==============================================================================
void FFTProcessor::prepare(double sr, int maxBlockSize, int channels)
{
    juce::ignoreUnused(maxBlockSize);
    sampleRate = sr;
    numChannels = channels;

    delayLines.resize(numChannels);
    for (auto& dl : delayLines)
        dl.assign(FIRLength, 0.0f);

    firCoeffs.resize(FIRLength, 0.0f);
    ready = false;
    currentLatency = 0;
}

void FFTProcessor::reset()
{
    for (auto& dl : delayLines)
        std::fill(dl.begin(), dl.end(), 0.0f);
}

//==============================================================================
void FFTProcessor::updateFIR(const EQEngine& eqEngine, EQMode mode)
{
    currentMode = mode;

    if (mode == EQMode::LinearPhase)
    {
        designLinearPhaseFIR(eqEngine);
    }
    else if (mode == EQMode::MinimumPhase)
    {
        designMinimumPhaseFIR(eqEngine);
    }

    reloadFilters();
}

//==============================================================================
void FFTProcessor::designLinearPhaseFIR(const EQEngine& eqEngine)
{
    std::vector<juce::dsp::Complex<float>> spectrum(FFTSize);
    const double delayPhaseFactor = -juce::MathConstants<double>::pi * (FIRLength - 1) / FIRLength;

    // Step 1: magnitude response with linear phase term for strict symmetry
    // For even-length Type-II FIR, the Nyquist response (i = N/2) must be 0
    for (int i = 0; i < FFTSize / 2; ++i)
    {
        double w = juce::MathConstants<double>::pi * i / (FFTSize / 2);
        double mag = eqEngine.getMagnitudeLinear(w);
        double phase = delayPhaseFactor * i;
        spectrum[i] = { static_cast<float>(mag * std::cos(phase)),
                        static_cast<float>(mag * std::sin(phase)) };
    }
    spectrum[FFTSize / 2] = { 0.0f, 0.0f }; // Type-II FIR: H(pi) = 0

    // Conjugate symmetry for second half
    for (int i = FFTSize / 2 + 1; i < FFTSize; ++i)
    {
        spectrum[i] = { spectrum[FFTSize - i].real(),
                       -spectrum[FFTSize - i].imag() };
    }

    // Step 2: IDFT -> time-domain coefficients
    std::vector<juce::dsp::Complex<float>> timeDomain(FFTSize);
    idft(spectrum.data(), timeDomain.data(), FFTSize);

    firCoeffs.resize(FIRLength);
    float cmax = 0.0f;
    for (int i = 0; i < FIRLength; ++i)
    {
        firCoeffs[i] = timeDomain[i].real();
        cmax = std::max(cmax, std::abs(firCoeffs[i]));
    }
    DBG("[SubEQ DBG] LinearPhase coeff peak=" + juce::String(cmax, 2));

    currentLatency = LinearPhaseLatency;
}

//==============================================================================
void FFTProcessor::designMinimumPhaseFIR(const EQEngine& eqEngine)
{
    const float epsilon = 1.0e-12f;
    std::vector<juce::dsp::Complex<float>> spectrum(FFTSize);

    // Step 1: log magnitude spectrum
    for (int i = 0; i <= FFTSize / 2; ++i)
    {
        double w = juce::MathConstants<double>::pi * i / (FFTSize / 2);
        double mag = eqEngine.getMagnitudeLinear(w);
        float logMag = std::log(static_cast<float>(mag) + epsilon);
        spectrum[i] = { logMag, 0.0f };
    }
    for (int i = FFTSize / 2 + 1; i < FFTSize; ++i)
    {
        spectrum[i] = { spectrum[FFTSize - i].real(), 0.0f };
    }

    // Step 2: IDFT -> cepstrum
    std::vector<juce::dsp::Complex<float>> cepstrum(FFTSize);
    idft(spectrum.data(), cepstrum.data(), FFTSize);

    // Step 3: Causalize cepstrum
    // c[0] kept, c[1..N/2-1] doubled, c[N/2] zeroed (even length), c[N/2+1..N-1] zeroed
    cepstrum[0] = { cepstrum[0].real(), 0.0f };
    for (int i = 1; i < FFTSize / 2; ++i)
        cepstrum[i] = { cepstrum[i].real() * 2.0f, 0.0f };
    cepstrum[FFTSize / 2] = { 0.0f, 0.0f };
    for (int i = FFTSize / 2 + 1; i < FFTSize; ++i)
        cepstrum[i] = { 0.0f, 0.0f };

    // Step 4: DFT -> minimum phase log spectrum
    std::vector<juce::dsp::Complex<float>> logSpectrum(FFTSize);
    dft(cepstrum.data(), logSpectrum.data(), FFTSize);

    // Step 5: exp() -> minimum phase complex spectrum
    for (int i = 0; i < FFTSize; ++i)
    {
        float real = logSpectrum[i].real();
        float imag = logSpectrum[i].imag();
        float expReal = std::exp(real) * std::cos(imag);
        float expImag = std::exp(real) * std::sin(imag);
        spectrum[i] = { expReal, expImag };
    }

    // Step 6: IDFT -> minimum phase FIR coefficients
    std::vector<juce::dsp::Complex<float>> coeffs(FFTSize);
    idft(spectrum.data(), coeffs.data(), FFTSize);

    firCoeffs.resize(FIRLength);
    bool hasInvalid = false;
    float cmax = 0.0f;
    for (int i = 0; i < FIRLength; ++i)
    {
        float val = coeffs[i].real();
        if (std::isnan(val) || std::isinf(val))
            hasInvalid = true;
        firCoeffs[i] = val;
        cmax = std::max(cmax, std::abs(val));
    }
    DBG("[SubEQ DBG] MinimumPhase coeff peak=" + juce::String(cmax, 2) + " hasInvalid=" + juce::String((int)hasInvalid));

    // Fallback to impulse if cepstral method produced invalid coefficients
    if (hasInvalid)
    {
        std::fill(firCoeffs.begin(), firCoeffs.end(), 0.0f);
        firCoeffs[0] = 1.0f;
        currentLatency = 0;
    }
    else
    {
        currentLatency = computeMaxGroupDelay(firCoeffs);
    }
}

//==============================================================================
int FFTProcessor::computeMaxGroupDelay(const std::vector<float>& coeffs)
{
    int N = FFTSize;
    int coeffSize = static_cast<int>(coeffs.size());
    int numPoints = N / 2 + 1;

    // Direct frequency response evaluation: H(ω) = Σ h[n] * e^(-jωn)
    std::vector<double> phase(numPoints);
    for (int k = 0; k < numPoints; ++k)
    {
        double w = juce::MathConstants<double>::pi * k / (N / 2);
        double real = 0.0, imag = 0.0;
        for (int n = 0; n < coeffSize; ++n)
        {
            double angle = -w * n;
            real += coeffs[n] * std::cos(angle);
            imag += coeffs[n] * std::sin(angle);
        }
        phase[k] = std::atan2(imag, real);
    }

    // Unwrap phase
    for (int i = 1; i < numPoints; ++i)
    {
        while (phase[i] - phase[i - 1] > juce::MathConstants<double>::pi)
            phase[i] -= 2.0 * juce::MathConstants<double>::pi;
        while (phase[i] - phase[i - 1] < -juce::MathConstants<double>::pi)
            phase[i] += 2.0 * juce::MathConstants<double>::pi;
    }

    // Group delay by numerical differentiation: τ(ω) = -dφ/dω
    // ω goes from 0 to pi in N/2 steps, so dω = 2*pi/N
    double dw = 2.0 * juce::MathConstants<double>::pi / N;
    double maxDelay = 0.0;

    // Central difference for interior points
    for (int i = 1; i < numPoints - 1; ++i)
    {
        double delay = -(phase[i + 1] - phase[i - 1]) / (2.0 * dw);
        if (delay > maxDelay)
            maxDelay = delay;
    }

    // Forward difference for endpoints
    if (numPoints > 1)
    {
        double delay0 = -(phase[1] - phase[0]) / dw;
        if (delay0 > maxDelay) maxDelay = delay0;

        double delayEnd = -(phase[numPoints - 1] - phase[numPoints - 2]) / dw;
        if (delayEnd > maxDelay) maxDelay = delayEnd;
    }

    // Sanity checks
    if (std::isnan(maxDelay) || std::isinf(maxDelay) || maxDelay < 0.0)
        return 0;
    if (maxDelay > static_cast<double>(FIRLength))
        return FIRLength;

    return static_cast<int>(maxDelay + 0.5);
}

//==============================================================================
void FFTProcessor::reloadFilters()
{
    if (firCoeffs.empty() || firCoeffs.size() != static_cast<size_t>(FIRLength))
        return;

    // Clear delay lines to prevent old samples convolving with new coefficients
    for (auto& dl : delayLines)
        std::fill(dl.begin(), dl.end(), 0.0f);

    ready = true;
}

//==============================================================================
void FFTProcessor::process(juce::AudioBuffer<float>& buffer)
{
    if (!ready)
        return;

    int channelsToProcess = std::min(buffer.getNumChannels(), static_cast<int>(delayLines.size()));
    for (int ch = 0; ch < channelsToProcess; ++ch)
    {
        auto* data = buffer.getWritePointer(ch);
        auto& dl = delayLines[ch];
        int numSamples = buffer.getNumSamples();

        for (int n = 0; n < numSamples; ++n)
        {
            // Shift delay line: move samples one position back
            for (int i = FIRLength - 1; i > 0; --i)
                dl[i] = dl[i - 1];
            dl[0] = data[n];

            // Direct convolution: sum of coeff[i] * delay[i]
            float sum = 0.0f;
            for (int i = 0; i < FIRLength; ++i)
                sum += firCoeffs[i] * dl[i];

            if (std::isnan(sum) || std::isinf(sum))
                sum = 0.0f;

            data[n] = sum;
        }
    }

}

//==============================================================================
int FFTProcessor::getLatencySamples() const
{
    return currentLatency;
}

bool FFTProcessor::isReady() const
{
    return ready;
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
