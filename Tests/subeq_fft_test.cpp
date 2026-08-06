// Standalone regression tests for the shared DSP math in
// Source/SubEQ_DSPMath.h (radix-2 FFT/IDFT, overlap-add convolution scheme
// used by FFTProcessor, conservative group-delay estimator, multi-slot
// per-size twiddle cache).
// The code under test is included directly — no copy drift.
//
// Build (Windows, VS dev prompt):
//   cl /EHsc /std:c++17 /O2 Tests/subeq_fft_test.cpp /Fe:Tests\subeq_fft_test.exe
#include "../Source/SubEQ_DSPMath.h"

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

// Overlap-add block convolution, mirroring FFTProcessor::process (block M=512,
// FFT size L = nextPow2(N + M - 1), output FIFO drains one sample per input).
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

    std::vector<double> overlap(L, 0.0);
    std::vector<std::complex<double>> work(L);
    std::vector<std::complex<double>> block(M);
    int pos = 0;
    std::vector<double> outQueue;

    for (double x : input)
    {
        block[pos] = { x, 0.0 };
        ++pos;

        if (pos == M)
        {
            for (int i = 0; i < M; ++i)
                work[i] = block[i];
            for (int i = M; i < L; ++i)
                work[i] = { 0.0, 0.0 };

            fftInPlace(work.data(), L);
            for (int i = 0; i < L; ++i)
                work[i] *= H[i];
            ifftInPlace(work.data(), L);

            for (int i = 0; i < M; ++i)
                outQueue.push_back(work[i].real() + overlap[i]);
            // Shift the accumulator and ADD this block's tail (a convolution
            // spans multiple blocks, so older tails must accumulate).
            for (int i = 0; i < L - M; ++i)
                overlap[i] = overlap[i + M] + work[M + i].real();
            for (int i = L - M; i < L; ++i)
                overlap[i] = 0.0;

            pos = 0;
        }

        if (!outQueue.empty())
        {
            output.push_back(outQueue.front());
            outQueue.erase(outQueue.begin());
        }
        else
        {
            output.push_back(0.0);   // initial block latency
        }
    }
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

    // ---- Test 3b: small-N specialization paths (len=2 / len=4 stages) ----
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

    // ---- Test 3: FFT vs direct DFT at N=64 ----
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

    // ---- Test 4: overlap-add convolution == direct convolution (N=4096) ----
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
        // valid only over complete blocks (9 blocks x 512 = 4608 samples).
        const int M = 512;
        const int offset = M - 1;
        const int numBlocks = (int)input.size() / M;
        const int validEnd = numBlocks * M;

        double maxErr = 0.0;
        for (int i = offset; i < validEnd; ++i)
            maxErr = std::max(maxErr, std::abs(outOla[i] - outDirect[i - offset]));
        check(maxErr < 1e-6, "overlap-add == direct convolution N=4096 (maxErr=" + std::to_string(maxErr) + ")");
    }

    // ---- Test 5: overlap-add with long FIR (N=16384) ----
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
        const int numBlocks = (int)input.size() / M;
        const int validEnd = numBlocks * M;

        double maxErr = 0.0;
        for (int i = offset; i < validEnd; ++i)
            maxErr = std::max(maxErr, std::abs(outOla[i] - outDirect[i - offset]));
        check(maxErr < 1e-6, "overlap-add == direct convolution N=16384 (maxErr=" + std::to_string(maxErr) + ")");
    }

    // ---- Test 6: impulse FIR reproduces input after block latency ----
    {
        std::mt19937 rng(99);
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        std::vector<double> fir(4096, 0.0);
        fir[0] = 1.0;   // identity

        std::vector<double> input(2000);
        for (auto& v : input) v = dist(rng);

        std::vector<double> out;
        olaProcess(fir, input, out);

        // Identity FIR: out[k] == input[k-511] over complete blocks
        const int M = 512;
        const int offset = M - 1;
        const int numBlocks = (int)input.size() / M;
        const int validEnd = numBlocks * M;

        double maxErr = 0.0;
        for (int i = offset; i < validEnd; ++i)
            maxErr = std::max(maxErr, std::abs(out[i] - input[i - offset]));
        check(maxErr < 1e-7, "impulse FIR = identity via overlap-add (maxErr=" + std::to_string(maxErr) + ")");
    }

    // ---- Test 7: computeMaxGroupDelay on a pure delay ----
    {
        const int N = 4096;
        std::vector<float> fir(N, 0.0f);
        const int delay = 1000;   // < N/2: unambiguous
        fir[delay] = 1.0f;
        int gd = computeMaxGroupDelay(fir);
        check(gd == delay, "group delay of delta[n-1000] == 1000 (got " + std::to_string(gd) + ")");
    }

    // ---- Test 8: computeMaxGroupDelay conservative fallback ----
    {
        const int N = 4096;
        std::vector<float> fir(N, 0.0f);
        fir[3000] = 1.0f;   // > N/2: ambiguous unwrap -> conservative full length
        int gd = computeMaxGroupDelay(fir);
        check(gd == N - 1, "group delay of delta[n-3000] conservative == N-1 (got " + std::to_string(gd) + ")");
    }

    // ---- Test 9: nextPowerOfTwo / convolutionFftSize ----
    {
        check(nextPowerOfTwo(4096) == 4096, "nextPow2(4096) == 4096");
        check(convolutionFftSize(4096, 512) == 8192, "convFftSize(4096,512) == 8192");
        check(convolutionFftSize(65536, 512) == 131072, "convFftSize(65536,512) == 131072");
        check(convolutionFftSize(16384, 512) == 32768, "convFftSize(16384,512) == 32768");
    }

    // ---- Test 10: multi-slot twiddle cache — alternating sizes stay correct ----
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

    // ---- Test 11: computeMaxGroupDelay boundary cases ----
    {
        // Tiny FIR: below the minimum analysable length
        std::vector<float> tiny(2, 1.0f);
        check(computeMaxGroupDelay(tiny) == 0, "group delay of 2-tap FIR == 0");

        // Gaussian-windowed symmetric FIR: perfectly linear phase with delay
        // exactly (N-1)/2 (the response has no zeros, so no unwrap ambiguity)
        const int N = 4096;
        std::vector<float> gauss(N);
        const double centre = (N - 1) / 2.0;
        const double sigma = N / 8.0;
        for (int i = 0; i < N; ++i)
        {
            const double t = (i - centre) / sigma;
            gauss[i] = static_cast<float>(std::exp(-0.5 * t * t));
        }
        int gd = computeMaxGroupDelay(gauss);
        check(std::abs(gd - (N - 1) / 2) <= 2,
              "group delay of symmetric Gaussian FIR ~= (N-1)/2 (got " + std::to_string(gd) + ")");

        // All-zero FIR: phase undefined everywhere -> conservative full length
        std::vector<float> zero(N, 0.0f);
        check(computeMaxGroupDelay(zero) == N - 1,
              "group delay of all-zero FIR conservative == N-1");

        // NaN-poisoned FIR: the estimator must never propagate NaN/inf into
        // PDC — the result must stay inside the valid [0, N-1] range
        std::vector<float> poison(N, 0.0f);
        poison[10] = std::numeric_limits<float>::quiet_NaN();
        int gdNan = computeMaxGroupDelay(poison);
        check(gdNan >= 0 && gdNan <= N - 1,
              "group delay of NaN FIR bounded to [0, N-1] (got " + std::to_string(gdNan) + ")");
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

    if (failures == 0)
    {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", failures);
    return 1;
}
