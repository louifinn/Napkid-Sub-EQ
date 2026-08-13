// Standalone regression tests for the shared standalone DSP (11 header-only
// modules, included directly — no copy drift):
//   - Source/SubEQ_DSPMath.h          (radix-2 FFT/IDFT, multi-slot twiddle cache)
//   - Source/SubEQ_FIRDesign.h        (linear / minimum phase FIR design)
//   - Source/SubEQ_FFTConvolver.h     (the shipped overlap-add convolution core)
//   - Source/SubEQ_Biquad.h           (double-precision biquad coeffs/state/response)
//   - Source/SubEQ_BiquadDesign.h     (RBJ coefficient design)
//   - Source/SubEQ_SpectrumMath.h     (octave bands / Hann / bin calibration)
//   - Source/SubEQ_SpectrumConfig.h   (spectrum choice-index decoders)
//   - Source/SubEQ_CoordinateMapper.h (response-plot coordinate mapping)
//   - Source/SubEQ_NodeInteraction.h  (gain sensitivity / type-switch / Q rules)
//   - Source/SubEQ_Spring.h           (underdamped spring integrator)
//   - Source/SubEQ_FilterType.h       (FilterType enum <-> int mapping)
//
// Build (Windows, VS dev prompt; /utf-8 for the non-ASCII comments):
//   cl /EHsc /std:c++17 /O2 /utf-8 Tests/subeq_fft_test.cpp /Fo:Tests\subeq_fft_test.obj /Fe:Tests\subeq_fft_test.exe
#include "../Source/SubEQ_DSPMath.h"
#include "../Source/SubEQ_FIRDesign.h"
#include "../Source/SubEQ_FFTConvolver.h"
#include "../Source/SubEQ_SpectrumMath.h"
#include "../Source/SubEQ_SpectrumConfig.h"
#include "../Source/SubEQ_CoordinateMapper.h"
#include "../Source/SubEQ_NodeInteraction.h"
#include "../Source/SubEQ_Spring.h"
#include "../Source/SubEQ_FilterType.h"
#include "../Source/SubEQ_Biquad.h"
#include "../Source/SubEQ_BiquadDesign.h"

#include <complex>
#include <vector>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>

using namespace SubEQ;

// Reference direct DFT for small N
static void directDft(const std::vector<std::complex<double>>& in, std::vector<std::complex<double>>& out)
{
    const int n = (int)in.size();
    out.assign(n, {});
    for (int k = 0; k < n; ++k)
    {
        std::complex<double> sum(0, 0);
        for (int t = 0; t < n; ++t)
        {
            const double ang = -2.0 * 3.14159265358979323846 * k * t / n;
            sum += in[t] * std::complex<double>(std::cos(ang), std::sin(ang));
        }
        out[k] = sum;
    }
}

// Reference: direct convolution with a linearly shifted delay line
static void directConv(const std::vector<double>& fir, const std::vector<double>& input,
                       std::vector<double>& output)
{
    const int N = (int)fir.size();
    const int numSamples = (int)input.size();
    output.assign(numSamples, 0.0);

    std::vector<double> dl(N, 0.0);
    for (int n = 0; n < numSamples; ++n)
    {
        for (int i = N - 1; i > 0; --i)
            dl[i] = dl[i - 1];
        dl[0] = input[n];

        double sum = 0.0;
        for (int i = 0; i < N; ++i)
            sum += fir[i] * dl[i];
        output[n] = sum;
    }
}

// Overlap-add block convolution through the shipped primitive
// SubEQ::processOlaChannel (block M=512, FFT size L = nextPow2(N + M - 1),
// output FIFO drains one sample per input). This is a thin adapter around the
// same code FFTProcessor::process runs, so the test exercises the shipped
// convolution core instead of a hand-copied mirror.
static void olaProcess(const std::vector<double>& fir, const std::vector<double>& input,
                       std::vector<double>& output)
{
    const int N = (int)fir.size();
    const int M = 512;
    const int L = convolutionFftSize(N, M);

    // Frequency-domain filter
    std::vector<std::complex<double>> H(L, { 0.0, 0.0 });
    for (int i = 0; i < N; ++i)
        H[i] = { fir[i], 0.0 };
    fftInPlace(H.data(), L);

    OlaChannelState st;
    st.inputBlock.assign(M, { 0.0, 0.0 });
    st.pos = 0;
    st.overlap.assign(L, 0.0);
    st.outQueue.reserve(M * 2);
    st.outRead = 0;

    std::vector<std::complex<double>> scratch(L);

    output.assign(input.size(), 0.0);
    processOlaChannel(st, input.data(), output.data(), (int)input.size(),
                      M, L, H.data(), scratch);
}

static int failures = 0;

static void check(bool ok, const std::string& name)
{
    if (!ok) { std::printf("FAIL: %s\n", name.c_str()); ++failures; }
    else     std::printf("PASS: %s\n", name.c_str());
}

int main()
{
    // ---- Benchmark (informational, no assertion): FFT throughput ----
    {
        const int N = 8192;
        std::vector<std::complex<double>> d(N);
        std::vector<std::complex<float>> f(N);
        for (int i = 0; i < N; ++i)
        {
            d[i] = { std::sin(0.01 * i), std::cos(0.013 * i) };
            f[i] = { (float)d[i].real(), (float)d[i].imag() };
        }

        const int reps = 200;
        auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < reps; ++r)
            fftInPlace(d.data(), N);
        auto t1 = std::chrono::steady_clock::now();
        for (int r = 0; r < reps; ++r)
            fftInPlace(f.data(), N);
        auto t2 = std::chrono::steady_clock::now();

        const double usDouble = std::chrono::duration<double, std::micro>(t1 - t0).count() / reps;
        const double usFloat  = std::chrono::duration<double, std::micro>(t2 - t1).count() / reps;
        std::printf("BENCH: %d-pt FFT  double=%.1f us  float=%.1f us\n", N, usDouble, usFloat);
    }

    // ---- Test 1: FFT/IDFT round-trip at N=4096 (double) ----
    {
        std::mt19937 rng(42);
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        std::vector<std::complex<double>> data(4096), orig(4096);
        for (auto& c : orig) c = { dist(rng), dist(rng) };
        data = orig;
        fftInPlace(data.data(), 4096);
        ifftInPlace(data.data(), 4096);
        double maxErr = 0.0;
        for (size_t i = 0; i < data.size(); ++i)
            maxErr = std::max(maxErr, std::abs(data[i] - orig[i]));
        check(maxErr < 1e-10, "FFT/IDFT round-trip N=4096 double (maxErr=" + std::to_string(maxErr) + ")");
    }

    // ---- Test 2: float FFT vs double FFT at N=8192 (overlap-add path) ----
    {
        std::mt19937 rng(11);
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        std::vector<std::complex<double>> d(8192);
        for (auto& c : d) c = { dist(rng), dist(rng) };
        auto d2 = d;
        fftInPlace(d.data(), 8192);

        std::vector<std::complex<float>> f(8192);
        for (int i = 0; i < 8192; ++i)
            f[i] = { (float)d2[i].real(), (float)d2[i].imag() };
        fftInPlace(f.data(), 8192);

        // float FFT with precomputed twiddles should be far more accurate
        // than the old recurrence version (~0.4% relative on random data);
        // assert a tight bound to catch regressions.
        double maxRelErr = 0.0;
        for (int i = 0; i < 8192; ++i)
        {
            const double ref = std::abs(d[i]);
            if (ref > 1e-6)
                maxRelErr = std::max(maxRelErr,
                    std::abs(std::complex<double>(f[i].real(), f[i].imag()) - d[i]) / ref);
        }
        check(maxRelErr < 1e-4, "float FFT matches double FFT N=8192 (maxRelErr=" + std::to_string(maxRelErr) + ")");
    }

    // ---- Test 3: small-N specialization paths (len=2 / len=4 stages) ----
    {
        std::mt19937 rng(31);
        std::uniform_real_distribution<double> dist(-1.0, 1.0);

        for (int n : { 2, 4 })
        {
            std::vector<std::complex<double>> data(n), ref;
            for (auto& c : data) c = { dist(rng), dist(rng) };
            auto fftResult = data;
            fftInPlace(fftResult.data(), n);
            directDft(data, ref);

            double maxErr = 0.0;
            for (int i = 0; i < n; ++i)
                maxErr = std::max(maxErr, std::abs(fftResult[i] - ref[i]));
            check(maxErr < 1e-12, "FFT vs direct DFT N=" + std::to_string(n)
                + " (specialized stages, maxErr=" + std::to_string(maxErr) + ")");
        }
    }

    // ---- Test 4: FFT vs direct DFT at N=64 ----
    {
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        std::vector<std::complex<double>> data(64), ref;
        for (auto& c : data) c = { dist(rng), dist(rng) };
        auto fftResult = data;
        fftInPlace(fftResult.data(), 64);
        directDft(data, ref);
        double maxErr = 0.0;
        for (int i = 0; i < 64; ++i)
            maxErr = std::max(maxErr, std::abs(fftResult[i] - ref[i]));
        check(maxErr < 1e-9, "FFT vs direct DFT N=64 (maxErr=" + std::to_string(maxErr) + ")");
    }

    // ---- Test 5: overlap-add convolution == direct convolution (N=4096) ----
    {
        std::mt19937 rng(123);
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        std::vector<double> fir(4096);
        for (auto& v : fir) v = dist(rng);

        std::vector<double> input(5000);
        for (auto& v : input) v = dist(rng);

        std::vector<double> outOla, outDirect;
        olaProcess(fir, input, outOla);
        directConv(fir, input, outDirect);

        // Overlap-add output is delayed by M-1 samples: outOla[k] == y[k-511],
        // valid over the entire input (the final partial block still drains).
        const int M = 512;
        const int offset = M - 1;
        const int validEnd = (int)input.size();

        double maxErr = 0.0;
        for (int i = offset; i < validEnd; ++i)
            maxErr = std::max(maxErr, std::abs(outOla[i] - outDirect[i - offset]));
        check(maxErr < 1e-6, "overlap-add == direct convolution N=4096 (maxErr=" + std::to_string(maxErr) + ")");
    }

    // ---- Test 5b: NaN/Inf poisoning flushes the accumulator (containment) ----
    {
        const int M = 512, L = 1024;
        OlaChannelState st;
        st.inputBlock.assign(M, { 0.0, 0.0 });
        st.pos = 0;
        st.overlap.assign(L, 0.0);
        st.outQueue.reserve(M * 2);
        st.outRead = 0;

        std::vector<std::complex<double>> h(L, { 0.0, 0.0 });
        h[0] = { 1.0, 0.0 };   // identity filter
        std::vector<std::complex<double>> scratch(L);
        std::vector<double> in(M * 4, 1.0), out(M * 4, 0.0);

        // 预跑一整块建立非零卷积尾
        processOlaChannel(st, in.data(), out.data(), M, M, L, h.data(), scratch);

        // 注入 NaN：毒化分支应把该块输出置零并冲刷累加器
        in[0] = std::numeric_limits<double>::quiet_NaN();
        processOlaChannel(st, in.data(), out.data(), M, M, L, h.data(), scratch);
        bool finite = true;
        for (double v : out) if (std::isnan(v) || std::isinf(v)) finite = false;
        check(finite, "NaN-poisoned overlap-add output stays finite");

        // 后续正常输入不再被污染
        std::fill(in.begin(), in.end(), 1.0);
        processOlaChannel(st, in.data(), out.data(), M, M, L, h.data(), scratch);
        bool finite2 = true;
        for (double v : out) if (std::isnan(v) || std::isinf(v)) finite2 = false;
        check(finite2, "overlap-add recovers after NaN poisoning");
    }

    // ---- Test 6: overlap-add with long FIR (N=16384) ----
    {
        std::mt19937 rng(5);
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        std::vector<double> fir(16384);
        for (auto& v : fir) v = dist(rng);

        std::vector<double> input(3000);
        for (auto& v : input) v = dist(rng);

        std::vector<double> outOla, outDirect;
        olaProcess(fir, input, outOla);
        directConv(fir, input, outDirect);

        const int M = 512;
        const int offset = M - 1;
        const int validEnd = (int)input.size();

        double maxErr = 0.0;
        for (int i = offset; i < validEnd; ++i)
            maxErr = std::max(maxErr, std::abs(outOla[i] - outDirect[i - offset]));
        check(maxErr < 1e-6, "overlap-add == direct convolution N=16384 (maxErr=" + std::to_string(maxErr) + ")");
    }

    // ---- Test 7: impulse FIR reproduces input after block latency ----
    {
        std::mt19937 rng(99);
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        std::vector<double> fir(4096, 0.0);
        fir[0] = 1.0;   // identity

        std::vector<double> input(2000);
        for (auto& v : input) v = dist(rng);

        std::vector<double> out;
        olaProcess(fir, input, out);

        // Identity FIR: out[k] == input[k-511] over the entire input
        const int M = 512;
        const int offset = M - 1;
        const int validEnd = (int)input.size();

        double maxErr = 0.0;
        for (int i = offset; i < validEnd; ++i)
            maxErr = std::max(maxErr, std::abs(out[i] - input[i - offset]));
        check(maxErr < 1e-7, "impulse FIR = identity via overlap-add (maxErr=" + std::to_string(maxErr) + ")");
    }

    // ---- Test 8: nextPowerOfTwo / convolutionFftSize ----
    {
        check(nextPowerOfTwo(4096) == 4096, "nextPow2(4096) == 4096");
        check(convolutionFftSize(4096, 512) == 8192, "convFftSize(4096,512) == 8192");
        check(convolutionFftSize(65536, 512) == 131072, "convFftSize(65536,512) == 131072");
        check(convolutionFftSize(16384, 512) == 32768, "convFftSize(16384,512) == 32768");
    }

    // ---- Test 9: multi-slot twiddle cache — alternating sizes stay correct ----
    {
        std::mt19937 rng(77);
        std::uniform_real_distribution<double> dist(-1.0, 1.0);

        // Prewarm a few sizes, then interleave FFTs of different sizes: each
        // size's cached table must survive other sizes being built and used
        // (the audio thread interleaves convolution and spectrum FFT sizes).
        prewarmTwiddleTable<double>(4096);
        prewarmTwiddleTable<double>(8192);
        prewarmTwiddleTable<float>(4096);

        bool ok = true;
        for (int round = 0; round < 3 && ok; ++round)
        {
            for (int n : { 64, 4096, 256, 8192, 128, 4096, 16384, 64 })
            {
                std::vector<std::complex<double>> data(n), orig(n);
                for (auto& c : data) c = { dist(rng), dist(rng) };
                orig = data;
                fftInPlace(data.data(), n);
                ifftInPlace(data.data(), n);
                for (int i = 0; i < n && ok; ++i)
                    ok = std::abs(data[i] - orig[i]) < 1e-10;
            }
        }
        check(ok, "twiddle cache: alternating sizes keep double round-trip accuracy");

        // Float slot independence: interleave other float sizes, then a 16384
        // float FFT must still match double within the tight bound.
        for (int n : { 256, 4096, 512 })
        {
            std::vector<std::complex<float>> junk(n);
            for (auto& c : junk) c = { 0.5f, -0.25f };
            fftInPlace(junk.data(), n);
        }

        std::vector<std::complex<double>> d(16384);
        for (auto& c : d) c = { dist(rng), dist(rng) };
        auto d2 = d;
        fftInPlace(d.data(), 16384);

        std::vector<std::complex<float>> f(16384);
        for (int i = 0; i < 16384; ++i)
            f[i] = { (float)d2[i].real(), (float)d2[i].imag() };
        fftInPlace(f.data(), 16384);

        double maxRelErr = 0.0;
        for (int i = 0; i < 16384; ++i)
        {
            const double ref = std::abs(d[i]);
            if (ref > 1e-6)
                maxRelErr = std::max(maxRelErr,
                    std::abs(std::complex<double>(f[i].real(), f[i].imag()) - d[i]) / ref);
        }
        check(maxRelErr < 1e-4, "twiddle cache: float FFT matches double at N=16384 after size mixing (maxRelErr=" + std::to_string(maxRelErr) + ")");
    }

    // ---- Test 10: linear phase FIR design ----
    {
        const int N = 1024;
        int latency = -1;
        auto target = [](double w) { return 1.0 + 0.5 * std::sin(w); };

        auto coeffs = designLinearPhaseFIR(target, N, latency);
        check(latency == (N - 1) / 2, "linear phase FIR latency == (N-1)/2");

        bool symmetric = true;
        for (int i = 0; i < N / 2; ++i)
            if (std::abs(coeffs[i] - coeffs[N - 1 - i]) > 1e-5) symmetric = false;
        check(symmetric, "linear phase FIR is symmetric");

        std::vector<std::complex<double>> spec(N);
        for (int i = 0; i < N; ++i) spec[i] = { (double)coeffs[i], 0.0 };
        fftInPlace(spec.data(), N);
        double maxErr = 0.0;
        for (int i = 0; i < N / 2; ++i)   // skip Nyquist (forced 0)
        {
            const double w = kPi * i / (N / 2);
            maxErr = std::max(maxErr, std::abs(std::abs(spec[i]) - target(w)));
        }
        check(maxErr < 1e-5, "linear phase FIR magnitude matches target (maxErr=" + std::to_string(maxErr) + ")");
    }

    // ---- Test 11: minimum phase FIR design ----
    {
        const int N = 1024;
        int latency = -1;
        auto target = [](double w) { return 1.0 + 0.5 * std::sin(w); };

        auto coeffs = designMinimumPhaseFIR(target, N, latency);
        check(latency == 0, "minimum phase FIR latency == 0");

        bool finite = true;
        for (auto c : coeffs) if (std::isnan(c) || std::isinf(c)) finite = false;
        check(finite, "minimum phase FIR has finite coefficients");

        std::vector<std::complex<double>> spec(N);
        for (int i = 0; i < N; ++i) spec[i] = { (double)coeffs[i], 0.0 };
        fftInPlace(spec.data(), N);
        double maxErr = 0.0;
        for (int i = 0; i <= N / 2; ++i)
        {
            const double w = kPi * i / (N / 2);
            maxErr = std::max(maxErr, std::abs(std::abs(spec[i]) - target(w)));
        }
        check(maxErr < 1e-5, "minimum phase FIR magnitude matches target (maxErr=" + std::to_string(maxErr) + ")");

        // 回归保护倒谱因果化的 Nyquist 折叠项（c[N/2] 必须保留而非置零）：
        // 用 Nyquist 处增益明显非 1 的目标验证 Nyquist 幅频仍贴合目标。
        auto targetNy = [](double w) { return 0.5 + 1.5 * (w / kPi); };
        auto coeffsNy = designMinimumPhaseFIR(targetNy, N, latency);
        std::vector<std::complex<double>> specNy(N);
        for (int i = 0; i < N; ++i) specNy[i] = { (double)coeffsNy[i], 0.0 };
        fftInPlace(specNy.data(), N);
        double nyqErr = std::abs(std::abs(specNy[N / 2]) - targetNy(kPi));
        check(nyqErr < 1e-5, "minimum phase FIR matches Nyquist-stressed target at Nyquist (err=" + std::to_string(nyqErr) + ")");

        // 幅值回调返回 NaN 时（倒谱法失效）应回退为冲激，而不是输出 NaN 系数。
        auto badTarget = [](double) { return std::numeric_limits<double>::quiet_NaN(); };
        auto fallback = designMinimumPhaseFIR(badTarget, 256, latency);
        bool isImpulse = std::abs(fallback[0] - 1.0) < 1e-12;
        for (int i = 1; i < 256; ++i)
            if (std::abs(fallback[i]) > 1e-12) isImpulse = false;
        check(isImpulse, "minimum phase FIR falls back to impulse for invalid magnitude");
    }

    // ---- Test 12: convolutionFftSize / nextPowerOfTwo extra cases ----
    {
        check(nextPowerOfTwo(1) == 1, "nextPow2(1) == 1");
        check(nextPowerOfTwo(3) == 4, "nextPow2(3) == 4");
        check(nextPowerOfTwo(131072) == 131072, "nextPow2(131072) == 131072");
        check(convolutionFftSize(1, 512) == 512, "convFftSize(1,512) == 512");
        check(convolutionFftSize(512, 512) == 1024, "convFftSize(512,512) == 1024");
        check(convolutionFftSize(513, 512) == 1024, "convFftSize(513,512) == 1024");
        check(convolutionFftSize(4097, 512) == 8192, "convFftSize(4097,512) == 8192");
    }

    // ---- Test 13: spectrum math (octave bands / Hann / calibration) ----
    {
        check(octaveDivisorForBands(61) == 6, "octave divisor 61 bands == 6");
        check(octaveDivisorForBands(121) == 12, "octave divisor 121 bands == 12");
        check(std::abs(octaveBandCenterFreq(0.5f, 0, 61) - 0.5f) < 1e-6f, "octave band 0 center == 0.5 Hz");
        check(std::abs(octaveBandCenterFreq(0.5f, 6, 61) - 1.0f) < 1e-6f, "octave band 6 (1/6 oct) == 1.0 Hz");
        check(std::abs(octaveBandCenterFreq(0.5f, 12, 121) - 1.0f) < 1e-6f, "octave band 12 (1/12 oct) == 1.0 Hz");

        const int N = 1025;   // odd: the exact centre sample lands on i=(N-1)/2
        check(std::abs(hannWindowValue(0, N)) < 1e-6f, "hann window[0] == 0");
        check(std::abs(hannWindowValue(N - 1, N)) < 1e-6f, "hann window[N-1] == 0");
        check(std::abs(hannWindowValue((N - 1) / 2, N) - 1.0f) < 1e-6f, "hann window[mid] == 1");

        check(std::abs(spectrumBinPowerScale(8192) - 16.0 / (8192.0 * 8192.0)) < 1e-20,
              "spectrum bin scale == 16/N^2");
    }

    // ---- Test 14: coordinate mapper round-trips and clamping ----
    {
        const float left = 20.0f, width = 800.0f, bottom = 500.0f, height = 460.0f;

        bool freqOk = true;
        for (float f : { 0.5f, 1.0f, 10.0f, 100.0f, 500.0f })
        {
            const float x = freqToX(f, left, width);
            const float back = xToFreq(x, left, width);
            if (std::abs(back - f) / f > 1e-4f) freqOk = false;
        }
        check(freqOk, "freq<->X round-trip");

        check(std::abs(xToFreq(left - 100.0f, left, width) - 0.5f) < 1e-6f, "xToFreq clamps low to 0.5 Hz");
        check(std::abs(xToFreq(left + width + 100.0f, left, width) - 500.0f) < 1e-3f, "xToFreq clamps high to 500 Hz");

        bool gainOk = true;
        for (float g : { -24.0f, -12.0f, 0.0f, 12.0f, 24.0f })
        {
            const float y = gainToY(g, bottom, height);
            const float back = yToGain(y, bottom, height);
            if (std::abs(back - g) > 1e-3f) gainOk = false;
        }
        check(gainOk, "gain<->Y round-trip");

        bool phaseOk = true;
        for (float p : { -180.0f, -90.0f, 0.0f, 90.0f, 180.0f })
        {
            const float y = phaseToY(p, bottom, height);
            const float back = yToPhase(y, bottom, height);
            if (std::abs(back - p) > 1e-2f) phaseOk = false;
        }
        check(phaseOk, "phase<->Y round-trip");
    }

    // ---- Test 15: node interaction rules (type sensitivity / Q step) ----
    {
        check(isGainSensitiveTypeIndex(0), "Bell is gain-sensitive");
        check(isGainSensitiveTypeIndex(3), "LowShelf is gain-sensitive");
        check(isGainSensitiveTypeIndex(4), "HighShelf is gain-sensitive");
        check(isGainSensitiveTypeIndex(6), "Tilt is gain-sensitive");
        check(!isGainSensitiveTypeIndex(1), "HighPass is not gain-sensitive");
        check(!isGainSensitiveTypeIndex(2), "LowPass is not gain-sensitive");
        check(!isGainSensitiveTypeIndex(5), "Notch is not gain-sensitive");
        check(!isGainSensitiveTypeIndex(7), "BandPass is not gain-sensitive");

        check(shouldResetGainOnTypeChange(0, 1), "Bell->HighPass resets gain");
        check(!shouldResetGainOnTypeChange(1, 0), "HighPass->Bell does not reset gain");
        check(!shouldResetGainOnTypeChange(0, 3), "Bell->LowShelf does not reset gain");

        check(std::abs(stepLogQ(1.0f, 0.30103f) - 2.0f) < 1e-3f, "stepLogQ doubles Q on +log10(2)");
        check(std::abs(stepLogQ(100.0f, -1.0f) - 10.0f) < 1e-3f, "stepLogQ clamps high to 10");
        check(std::abs(stepLogQ(0.1f, -1.0f) - 0.1f) < 1e-6f, "stepLogQ clamps low to 0.1");
    }

    // ---- Test 16: spring integrator converges and is deterministic ----
    {
        const float dt = 1.0f / 60.0f;
        const float wn2 = 400.0f;       // omega_n = 20 rad/s
        const float twoZetaWn = 20.0f;  // zeta = 0.5
        const float target = 100.0f;

        float pos = 0.0f, vel = 0.0f;
        for (int i = 0; i < 600; ++i)   // 10 s is far past the settling time
            stepSpring(pos, vel, dt, target, wn2, twoZetaWn);

        check(std::abs(pos - target) < 0.01f, "spring converges to target");

        // A single step toward a lower target moves position down (no NaN/overshoot explosion)
        pos = 100.0f; vel = 0.0f;
        stepSpring(pos, vel, dt, 0.0f, wn2, twoZetaWn);
        check(pos < 100.0f && std::isfinite(pos), "spring moves toward lower target");
    }

    // ---- Test 17: FilterType <-> choice-index mapping ----
    {
        bool roundTrip = true;
        for (int i = 0; i <= 7; ++i)
            if (filterTypeToInt(intToFilterType(i)) != i) roundTrip = false;
        check(roundTrip, "FilterType<->int round-trip for all 8 types");

        check(intToFilterType(-1) == FilterType::Bell, "intToFilterType(-1) falls back to Bell");
        check(intToFilterType(99) == FilterType::Bell, "intToFilterType(99) falls back to Bell");
        check(filterTypeToInt(FilterType::BandPass) == 7, "BandPass -> index 7");
    }

    // ---- Test 18: spectrum choice-index decoders ----
    {
        check(spectrumFftChoiceToOrder(0) == 12, "fft choice 0 -> order 12 (4096)");
        check(spectrumFftChoiceToOrder(1) == 13, "fft choice 1 -> order 13 (8192)");
        check(spectrumFftChoiceToOrder(2) == 14, "fft choice 2 -> order 14 (16384)");
        check(spectrumDensityChoiceToBands(0) == 61, "density choice 0 -> 61 bands");
        check(spectrumDensityChoiceToBands(1) == 121, "density choice 1 -> 121 bands");
        check(spectrumHopChoiceToSamples(0) == 512, "hop choice 0 -> 512");
        check(spectrumHopChoiceToSamples(1) == 1024, "hop choice 1 -> 1024");
        check(spectrumHopChoiceToSamples(2) == 2048, "hop choice 2 -> 2048");
        check(spectrumRefreshChoiceToHz(0) == 15, "refresh choice 0 -> 15 Hz");
        check(spectrumRefreshChoiceToHz(1) == 30, "refresh choice 1 -> 30 Hz");
        check(spectrumRefreshChoiceToHz(2) == 60, "refresh choice 2 -> 60 Hz");
        check(spectrumFftChoiceToOrder(99) == 13, "fft choice invalid -> order 13");
        check(spectrumDensityChoiceToBands(99) == 61, "density choice invalid -> 61");
        check(spectrumHopChoiceToSamples(99) == 512, "hop choice invalid -> 512");
        check(spectrumRefreshChoiceToHz(99) == 30, "refresh choice invalid -> 30 Hz");
    }

    // ---- Test 19: biquad coefficients / state / response ----
    {
        BiquadCoefficients ident;   // defaults: b0=1, everything else 0
        check(ident.isStable(), "identity biquad is stable");

        const std::complex<double> one(1.0, 0.0);
        check(std::abs(biquadResponse(ident, 0.0) - one) < 1e-12, "identity response at DC == 1");
        check(std::abs(biquadResponse(ident, 3.14159265358979323846) - one) < 1e-12, "identity response at Nyquist == 1");

        BiquadState st;
        check(std::abs(st.process(0.5, ident) - 0.5) < 1e-12, "identity biquad passes a sample through");

        BiquadCoefficients unstable;
        unstable.a2 = 1.5;
        check(!unstable.isStable(), "a2=1.5 is unstable");
        unstable.forceStable();
        check(unstable.isStable(), "forceStable corrects unstable coefficients");

        // forceStable 其余分支：a2 <= -1 与 |a1| >= 1 + a2
        BiquadCoefficients c2;
        c2.a1 = 0.0; c2.a2 = -1.5;
        c2.forceStable();
        check(c2.isStable() && c2.a2 > -1.0, "forceStable corrects a2 <= -1.0");
        BiquadCoefficients c3;
        c3.a1 = 2.5; c3.a2 = 0.5;
        c3.forceStable();
        check(c3.isStable(), "forceStable corrects |a1| >= 1 + a2");
    }

    // ---- Test 20: biquad coefficient design invariants ----
    {
        const double sr = 48000.0;
        BiquadCoefficients coeffs[2];
        const std::complex<double> one(1.0, 0.0);

        // Bell at 0 dB gain is a unity transfer function (b0=1 and b1=a1, b2=a2).
        int n = computeBiquadCoefficients(1000.0, 0.0, 0.707, FilterType::Bell, sr, coeffs);
        check(n == 1, "Bell uses 1 biquad");
        bool bellUnity = true;
        for (double w : { 0.0, 0.5, 1.0, 2.0, 3.0 })
            if (std::abs(biquadResponse(coeffs[0], w) - one) > 1e-6) bellUnity = false;
        check(bellUnity, "Bell @ 0 dB is unity across frequencies");

        // LowPass at a low cutoff: DC gain ~1, Nyquist gain ~0.
        n = computeBiquadCoefficients(100.0, 0.0, 0.707, FilterType::LowPass, sr, coeffs);
        check(n == 1, "LowPass uses 1 biquad");
        check(std::abs(std::abs(biquadResponse(coeffs[0], 0.0)) - 1.0) < 1e-6, "LowPass DC gain ~1");
        check(std::abs(biquadResponse(coeffs[0], 3.14159265358979323846)) < 1e-3, "LowPass Nyquist gain ~0");

        // Tilt cascades two biquads.
        n = computeBiquadCoefficients(100.0, 6.0, 0.707, FilterType::Tilt, sr, coeffs);
        check(n == 2, "Tilt uses 2 biquads");

        // BandPass 非增益敏感（与 SubEQ_NodeInteraction.h 的分类一致）：gain
        // 参数不得缩放通带峰值——0 dB 与 +12 dB 应得到完全相同的系数。
        BiquadCoefficients bp0[2], bp12[2];
        computeBiquadCoefficients(200.0, 0.0, 1.0, FilterType::BandPass, sr, bp0);
        computeBiquadCoefficients(200.0, 12.0, 1.0, FilterType::BandPass, sr, bp12);
        bool bpGainInsensitive = std::abs(bp0[0].b0 - bp12[0].b0) < 1e-15
                              && std::abs(bp0[0].b1 - bp12[0].b1) < 1e-15
                              && std::abs(bp0[0].b2 - bp12[0].b2) < 1e-15;
        check(bpGainInsensitive, "BandPass ignores gain parameter");

        // 标准 RBJ 带通中心增益 = 0 dB。
        const double wc = kTwoPi * 200.0 / sr;
        check(std::abs(std::abs(biquadResponse(bp0[0], wc)) - 1.0) < 1e-9, "BandPass center gain == 0 dB");
    }

    if (failures == 0)
    {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", failures);
    return 1;
}
