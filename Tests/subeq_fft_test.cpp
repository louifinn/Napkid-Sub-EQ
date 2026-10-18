// Standalone regression tests for the shared standalone DSP (13 header-only
// modules, included directly — no copy drift):
//   - Source/SubEQ_DSPMath.h          (radix-2 FFT/IDFT, multi-slot twiddle cache)
//   - Source/SubEQ_FIRDesign.h        (linear / minimum phase FIR design)
//   - Source/SubEQ_FFTConvolver.h     (the shipped overlap-add convolution core)
//   - Source/SubEQ_Biquad.h           (double-precision biquad coeffs/state/response)
//   - Source/SubEQ_BiquadDesign.h     (RBJ coefficient design)
//   - Source/SubEQ_SpectrumMath.h     (octave band centres + display smoothing math)
//   - Source/SubEQ_SpectrumConfig.h   (spectrum choice-index decoders)
//   - Source/SubEQ_CoordinateMapper.h (response-plot coordinate mapping)
//   - Source/SubEQ_NodeInteraction.h  (gain sensitivity / type-switch / Q rules)
//   - Source/SubEQ_Spring.h           (underdamped spring integrator)
//   - Source/SubEQ_FilterType.h       (FilterType enum <-> int mapping)
//   - Source/SubEQ_WindowFunctions.h  (Hann/Blackman/BH-4/Nuttall window family)
//   - Source/SubEQ_VQTDesign.h        (multi-rate VQT tier/kernel design + offline engine math)
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
#include "../Source/SubEQ_WindowFunctions.h"
#include "../Source/SubEQ_VQTDesign.h"

#include <complex>
#include <vector>
#include <cmath>
#include <chrono>
#include <random>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>   // std::string / std::to_string（F-022：自包含性）

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

// ============================================================================
// 离线 VQT 原型引擎（v0.4.0 频谱分析内核原型）
// 与 SubEQ_Spectrum 运行时同一套交付数学：层布局/核/帧谱/段功率/抽取系数/DC
// 阻断/显示管线全部来自共享头（SubEQ_VQTDesign.h 与 SubEQ_SpectrumMath.h）——
// 抽取级联（vqtDecimationStep）、层环形缓冲装配（vqtPushTierSample）与显示
// 管线（vqtUpdateDisplayBands）都是直接调用，不再有逐行镜像。
// 本结构只负责组装（无原子、无 JUCE）与测试专有的读数（rawDb）。
// 校准 / 泄漏 / 封顶 / 性能断言都在这里跑；数值路径与交付代码零漂移。
// ============================================================================
struct OfflineVqtAnalyzer
{
    // 级数上界与运行时同源（SubEQ_Spectrum.h 的 MaxStages 用同一表达式）：
    // 顶层降采样指数上限 + 每层向下再 ÷2 的 9 级（STD-11）
    static constexpr int MaxStages = VQTMaxTopDecimationExponent + VQTNumTiers - 1;

    double sampleRate = 48000.0;
    int hop = 512;
    const VQTKernelSet* set = nullptr;

    // 0.05 Hz 一阶直流阻断
    double dcPole = 0.0, dcState = 0.0;

    // 抽取级联：stage k 输入率 fs/2^k，每 2 个输入向下一级发射 1 个样本。
    // 一级状态与运行时共用 VQTDecimationStage（SubEQ_VQTDesign.h）。
    VQTDecimationStage stages[MaxStages];
    int numStages = 0;

    // 各层状态：环形缓冲（容量 = 层帧长）+ 帧谱 scratch
    struct TierState
    {
        std::vector<double> ring;
        int write = 0;
        int sinceAnalysis = 0;
        std::vector<std::complex<double>> scratch;
    };
    TierState tier[VQTNumTiers];

    // 输出（与运行时相同的显示管线约定：频域平滑 + 功率域时间包络，见
    // SubEQ_SpectrumMath.h）：bandDb 为钳位 [-60, 0] 的显示值，rawDb 为
    // 未钳位的逐段瞬时真值（泄漏断言用，不含平滑/包络）
    double bandDb[121] = {};
    double rawDb[121] = {};
    double bandPowers[121] = {};
    double envPowers[121] = {};
    int numBands = 61;
    int smoothRadius = 0;   // 默认 Off（校准/泄漏断言看逐段原始行为）

    void prepare (double fs, const VQTKernelSet& kernelSet, int hopSamples,
                  int smoothingIndex = 0)
    {
        sampleRate = fs;
        set = &kernelSet;
        hop = hopSamples;

        // 段总数 = 顶层（最后一个 tier）的末段下标 + 1，与运行时 configure 相同
        numBands = kernelSet.tiers[VQTNumTiers - 1].firstBand
                 + kernelSet.tiers[VQTNumTiers - 1].numBandsInTier;
        smoothRadius = smoothingRadiusBands (smoothingIndex, numBands);

        dcPole = vqtDcBlockerPole (fs);
        dcState = 0.0;

        // 级数与运行时同源（SubEQ_Spectrum.cpp 的 prepare() 用同一表达式与
        // 同一钳位）：顶层降采样指数上限 + 每层向下再 ÷2 的 9 级（STD-11）
        const int wantedStages = vqtTopDecimationExponent (fs) + VQTNumTiers - 1;
        numStages = std::min (wantedStages, MaxStages);
        for (int k = 0; k < numStages; ++k)
        {
            vqtDecimatorCoefficients (fs / std::ldexp (1.0, k), stages[k].coeffs);
            for (auto& s : stages[k].state) s.reset();
            stages[k].phase = 0;
        }

        for (int t = 0; t < VQTNumTiers; ++t)
        {
            const int F = kernelSet.tiers[t].fftSize;
            tier[t].ring.assign (static_cast<size_t> (F), 0.0);
            tier[t].scratch.assign (static_cast<size_t> (F), {});
            tier[t].write = 0;
            tier[t].sinceAnalysis = 0;
        }

        for (int b = 0; b < 121; ++b)
        {
            bandDb[b] = kDisplayFloorDb;
            rawDb[b] = kDisplayFloorDb;      // 未钳位真值读数同从地板起步
            // 两个功率缓冲同从地板等效功率起步（与运行时复位同源，SPC-06）
            bandPowers[b] = kDisplayFloorPower;
            envPowers[b] = kDisplayFloorPower;
        }
    }

    void processSample (double input)
    {
        // 抽取级联（直流阻断 + 逐级 ↓2）与运行时 SpectrumAnalyzer::process 共用
        // SubEQ_VQTDesign.h 的 vqtDecimationStep——数值路径不再各写一份
        vqtDecimationStep (input, stages, numStages, dcPole, dcState,
                           [this] (int t, double sample) { pushTierSample (t, sample); });
    }

    // 最近频段下标（用于把测试音对准某个频段中心）
    static int bandIndexNear (double freqHz, int numBands)
    {
        const int div = octaveDivisorForBands (numBands);
        const int n = static_cast<int> (std::lround (std::log2 (freqHz / 0.5) * div));
        return std::min (std::max (n, 0), numBands - 1);
    }

private:
    void pushTierSample (int t, double y)
    {
        TierState& ts = tier[t];
        // 环形缓冲装配 + 步距触发（与运行时共用，见 SubEQ_VQTDesign.h）
        vqtPushTierSample (ts.ring.data(), set->tiers[t].fftSize, y,
                           ts.write, ts.sinceAnalysis,
                           vqtTierStride (set->tiers[t].decimation, hop),
                           [this, t] { analyzeTier (t); });
    }

    void analyzeTier (int t)
    {
        TierState& ts = tier[t];
        const VQTTier& T = set->tiers[t];
        vqtTierFrameSpectrum (ts.ring.data(), ts.write, set->frameWindow[t].data(),
                              ts.scratch.data(), T.fftSize);

        // 与运行时 analyzeTier 相同的时间归一化包络系数——折算逻辑收口在
        // 共享头（vqtTierEnvelopeAlphas），运行时与离线原型同调、零局部镜像。
        const int stride = vqtTierStride (T.decimation, hop);
        const VQTTierEnvelopeAlphas alphas = vqtTierEnvelopeAlphas (T.decimation, stride, sampleRate);

        // 显示管线（瞬时功率 → 频域平滑 → 功率域一阶包络 → dB/钳位）调用与
        // 运行时同一实现（SubEQ_SpectrumMath.h 的 vqtUpdateDisplayBands）——
        // 离线原型不再是手抄镜像，交付数学与回归数学零漂移（STD-04）。
        vqtUpdateDisplayBands (bandPowers, envPowers, T.firstBand, T.numBandsInTier,
                               numBands, smoothRadius, alphas.attackAlpha, alphas.releaseAlpha, bandDb,
                               [this, &ts, &T, t] (int i)
                               {
                                   return vqtBandPower (ts.scratch.data(),
                                       set->kernels[t][static_cast<size_t> (i)]);
                               });

        // rawDb（测试专有，泄漏断言要读逐段未钳位真值，不含平滑/包络）在共享
        // 调用之外单独算：读的是本次刚写入的瞬时功率，与旧镜像逐值一致。
        for (int i = 0; i < T.numBandsInTier; ++i)
        {
            const double p = bandPowers[T.firstBand + i];
            rawDb[T.firstBand + i] = (p > 1.0e-16) ? 10.0 * std::log10 (p) : -200.0;
        }
    }
};

// 喂一段合成信号（秒），返回喂入的样本数
static int feedSignal (OfflineVqtAnalyzer& a, double freqHz, double amplitude,
                       double seconds, double sampleRate)
{
    // 相位延续不必精确——测试只看稳态
    const int n = static_cast<int> (seconds * sampleRate);
    const double w = 2.0 * 3.14159265358979323846 * freqHz / sampleRate;
    for (int i = 0; i < n; ++i)
        a.processSample (amplitude * std::sin (w * i));
    return n;
}

static int failures = 0;

static void check(bool ok, const std::string& name)
{
    if (!ok) { std::printf("FAIL: %s\n", name.c_str()); ++failures; }
    else     std::printf("PASS: %s\n", name.c_str());
}

// ============================================================================
// Test 11c 辅助：任意频率处的直接 DTFT 幅值（相位递推，O(N) 每点）。
// 只用于回归测量，不参与交付设计路径。
// ============================================================================
static double firMagnitudeAt(const std::vector<double>& h, double w)
{
    const std::complex<double> step(std::cos(w), -std::sin(w));
    std::complex<double> phase(1.0, 0.0);
    std::complex<double> response(0.0, 0.0);
    for (double c : h)
    {
        response += c * phase;
        phase *= step;
    }
    return std::abs(response);
}

// [lowHz, highHz] 上 points 个对数间隔点，FIR 与目标的 max |dB 误差|。
template <typename TargetFn>
static double maxFirDbError(const std::vector<double>& h, TargetFn target,
                            double lowHz, double highHz, double sampleRate, int points)
{
    double maxError = 0.0;
    for (int i = 0; i < points; ++i)
    {
        const double fraction = (points > 1) ? static_cast<double>(i) / (points - 1) : 0.0;
        const double f = lowHz * std::pow(highHz / lowHz, fraction);
        const double w = 2.0 * kPi * f / sampleRate;
        const double firDb = 20.0 * std::log10(firMagnitudeAt(h, w) + 1.0e-12);
        const double targetDb = 20.0 * std::log10(target(w) + 1.0e-12);
        maxError = std::max(maxError, std::abs(firDb - targetDb));
    }
    return maxError;
}

// 全带幅频误差（v0.4.5 复审 P1 锁定）：与 maxFirDbError 相同，但只统计
// targetDb >= floorDb 的点——深阻带（-240 dB 地板）的 dB 误差没有物理意义。
// 用于锁定解析候选在 500 Hz 以上至 Nyquist 的高频截断振铃：带内拟合看不到
// 这些点，0.4.5 复审实测偏差 28–97 dB。
template <typename TargetFn>
static double maxFirDbErrorAboveFloor(const std::vector<double>& h, TargetFn target,
                                      double lowHz, double highHz, double sampleRate,
                                      int points, double floorDb)
{
    double maxError = 0.0;
    for (int i = 0; i < points; ++i)
    {
        const double fraction = (points > 1) ? static_cast<double>(i) / (points - 1) : 0.0;
        const double f = lowHz * std::pow(highHz / lowHz, fraction);
        const double w = 2.0 * kPi * f / sampleRate;
        const double targetDb = 20.0 * std::log10(target(w) + 1.0e-12);
        if (targetDb < floorDb)
            continue;
        const double firDb = 20.0 * std::log10(firMagnitudeAt(h, w) + 1.0e-12);
        maxError = std::max(maxError, std::abs(firDb - targetDb));
    }
    return maxError;
}

// 全带断言的物理带宽：上限 0.45·fs（Nyquist 安全上界）；下限取 0.5 Hz 与
// 一个设计 bin（fs/N）的较大者——低于 1 bin 时目标形状（拐点/深衰减的 dB
// 斜率）比设计网格还快，任何 N 抽头 FIR 都无法复现：对 3×HP30
// 96 kHz/N=4096 连「理想 IIR 截断」在 6.5 Hz 都有 47 dB 偏差（矩阵里该
// cell 的拐点本身只有 1.28 bin）。这与矩阵 cell 的「拐点 <1 bin」跳过是
// 同一物理判据，不是新增的 bespoke cell 跳过。
static double fullBandLowHz(double sampleRate, int firLength)
{
    return std::max(0.5, sampleRate / static_cast<double>(firLength));
}

static double fullBandHighHz(double sampleRate)
{
    return 0.45 * sampleRate;
}

// 时域结构零点断言：H(0) = Σ h[n]，H(pi) = Σ h[n](-1)^n。
static double dcTapSumAbs(const std::vector<double>& h)
{
    double sum = 0.0;
    for (double v : h)
        sum += v;
    return std::abs(sum);
}

static double nyquistTapSumAbs(const std::vector<double>& h)
{
    double sum = 0.0;
    for (int i = 0; i < static_cast<int>(h.size()); ++i)
        sum += ((i & 1) != 0) ? -h[static_cast<size_t>(i)] : h[static_cast<size_t>(i)];
    return std::abs(sum);
}

// 抽头峰值：多节点候选的 sanity gate（灾难性相消会产生 1e4…1e50 尖峰）。
static double maxTapMagnitude(const std::vector<double>& h)
{
    double peak = 0.0;
    for (double v : h)
        peak = std::max(peak, std::abs(v));
    return peak;
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
        // 注意（F-024）：此处的 h 是未经 FFT 的频域系数（[1,0,0,...]），
        // 实际为 DC-only 滤波器（每块输出 = 块直流分量/L），并非恒等
        // 滤波器——恒等路径由 Test 7 覆盖。其作用是快速建立非零卷积尾。
        h[0] = { 1.0, 0.0 };
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

    // ---- Test 5c: F6——有限但超出输出类型范围的卷积值不得变成 float inf ----
    {
        const int M = 512, L = 1024;
        OlaChannelState st;
        st.inputBlock.assign(M, { 0.0, 0.0 });
        st.pos = 0;
        st.overlap.assign(L, 0.0);
        st.outQueue.reserve(M * 2);
        st.outRead = 0;

        std::vector<std::complex<double>> h(L, { 0.0, 0.0 });
        h[0] = { 1.0e39, 0.0 };   // 有限 double，但远超 float 上限
        std::vector<std::complex<double>> scratch(L);
        std::vector<float> in(static_cast<size_t>(M * 2), 1.0f);
        std::vector<float> out(static_cast<size_t>(M * 2), 0.0f);

        processOlaChannel(st, in.data(), out.data(), M * 2, M, L, h.data(), scratch);

        bool finite = true;
        bool allZero = true;
        for (float v : out)
        {
            if (!std::isfinite(v))
                finite = false;
            if (v != 0.0f)
                allZero = false;
        }
        check(finite, "F6: finite over-range convolution output is contained (no float inf)");
        check(allZero, "F6: over-range block output is zeroed and accumulator flushed");
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

    // ---- Test 11b: minimum phase FIR — 低频高通目标的带内假陷波回归（F1） ----
    // 高通目标在 DC 的传输零点被 epsilon=1e-12 钳位成 -240 dB 孤立尖峰，N 点
    // 倒谱把它当宽带特征做时域混叠，在通带内造出假陷波（v0.4.3 实测 HP30 Q0.9
    // N=4096 @41.59 Hz：目标 +0.59 dB / FIR -19.48 dB，全带最深 -31 dB）。修复
    // 为按需 8× 过采样设计网格后，此处用直接 DTFT 核对通带幅频误差。
    {
        const double fs = 48000.0;
        struct HpTarget { const char* name; double cutoffHz; };
        const HpTarget targets[] = { { "HP30", 30.0 }, { "HP80", 80.0 }, { "HP120", 120.0 } };

        for (const HpTarget& t : targets)
        {
            BiquadCoefficients c[2];
            computeBiquadCoefficients(t.cutoffHz, 0.0, 0.9, FilterType::HighPass, fs, c);
            auto target = [&c] (double w) { return std::abs(biquadResponse(c[0], w)); };

            int mpLatency = -1;
            const std::vector<double> h = designMinimumPhaseFIR(target, 4096, mpLatency);

            check(mpLatency == 0,
                  std::string("min phase FIR passband: ") + t.name + " latency == 0");

            bool mpFinite = true;
            for (double v : h)
                if (std::isnan(v) || std::isinf(v))
                    mpFinite = false;
            check(mpFinite,
                  std::string("min phase FIR passband: ") + t.name + " coefficients finite");

            // 通带 [fc, 500] Hz 上 200 个对数点逐点算 dB 误差（直接 DTFT，不做插值）
            double maxDbErr = 0.0, minDbErr = 0.0;
            for (int i = 0; i < 200; ++i)
            {
                const double f = t.cutoffHz * std::pow(500.0 / t.cutoffHz, i / 199.0);
                const double w = 2.0 * kPi * f / fs;
                std::complex<double> acc(0.0, 0.0);
                for (int n = 0; n < 4096; ++n)
                {
                    const double ang = -w * n;
                    acc += h[n] * std::complex<double>(std::cos(ang), std::sin(ang));
                }
                const double firdB = 20.0 * std::log10(std::abs(acc));
                const double targetdB = 20.0 * std::log10(std::abs(biquadResponse(c[0], w)));
                const double err = firdB - targetdB;
                maxDbErr = std::max(maxDbErr, std::abs(err));
                minDbErr = std::min(minDbErr, err);
            }

            std::printf("      min phase FIR passband %s: max |dB err| %.3f dB, deepest %.3f dB\n",
                        t.name, maxDbErr, minDbErr);

            check(maxDbErr < 0.6,
                  std::string("min phase FIR passband: ") + t.name + " max |dB err| < 0.6 dB (err="
                      + std::to_string(maxDbErr) + ")");
            check(minDbErr > -1.0,
                  std::string("min phase FIR passband: ") + t.name + " no in-band notch (deepest="
                      + std::to_string(minDbErr) + " dB)");
        }
    }

    // ---- Test 11c: 解析 DC/Nyquist 零点矩阵（v0.4.5） ----
    // 单节点：高通目标在 DC 有 2 阶传输零点，候选选优 / 精确零点投影后 h 的
    // DC 响应必须是精确 0（Σh = 0 的时域相消结构，而非 epsilon 钳位近似）。
    // 三条断言（每个 cell）：
    //   1) 精确结构零点 |Σh| < 1e-9（LP 用 |Σh(-1)^n|，BP 两者）；
    //   2) 带内 [fc, min(500, fs/2)] 相对 baseline：selectedErr ≤
    //      max(linearErr, legacyErr, 1.0) + 0.5 dB。legacyErr 用 3 参
    //      designMinimumPhaseFIR（= 0.4.4 自动路径）同目标实测，因此既不允许
    //      比 0.4.4 差，也不需要任何 bespoke bins / Q 跳过；
    //   3) 全带 [max(0.5, fs/N), 0.45·fs] 200 个对数点，target ≥ -80 dB 的
    //      点 max |dB 误差| ≤ 6.0 dB（v0.4.5 复审 P1 锁定：解析候选 h_rest
    //      截断在 >500 Hz 至 Nyquist 产生 28–97 dB 偏差，带内拟合看不到）。
    //      下限取一个设计 bin：低于 1 bin 的目标形状物理不可分辨（与 cell 的
    //      「拐点 <1 bin」跳过同一判据；3×HP30 96 kHz/4096 连理想 IIR 截断
    //      在 6.5 Hz 都有 47 dB 偏差）。
    // 矩阵覆盖 fs × N × fc × Q（含 96 kHz/N=4096/fc=30/Q=2 近极限 cell，
    // 由 legacy 候选 + 零点投影通过，不再跳过），65536 点再抽查三个场景。
    // 多节点矩阵（级联 4–6 阶零点）与单节点 LP/BP 见下方独立子块。
    {
        const double sampleRates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
        const int firLengths[] = { 4096, 16384 };
        const double cutoffs[] = { 30.0, 100.0, 400.0 };
        const double qValues[] = { 0.707, 2.0 };

        int cellsRun = 0;
        int cellsSkippedUnderOneBin = 0;
        double worstMinPhaseErr = 0.0;
        double worstLinearPhaseErr = 0.0;
        double worstLegacyErr = 0.0;
        double worstFullBandErr = 0.0;
        double worstMargin = -1.0e9;   // minErr - (max(linErr, legacyErr, 1) + 0.5)
        std::string worstCell;
        std::string worstFullCell;

        for (double fs : sampleRates)
        for (int N : firLengths)
        for (double fc : cutoffs)
        for (double q : qValues)
        {
            const double binsPerCutoff = fc * static_cast<double>(N) / fs;

            // 拐点不足 1 个设计 bin（设计网格分辨率 fs/N）：目标变化比网格
            // 还快，线性相位已无法采样拐点，该 cell 物理不可分辨。
            if (binsPerCutoff < 1.0)
            {
                ++cellsSkippedUnderOneBin;
                continue;
            }

            ++cellsRun;

            BiquadCoefficients c[2];
            computeBiquadCoefficients(fc, 0.0, q, FilterType::HighPass, fs, c);
            auto target = [&c] (double w) { return std::abs(biquadResponse(c[0], w)); };

            int mpLatency = -1;
            const std::vector<double> hMin = designMinimumPhaseFIR(target, N, mpLatency, 2, 0, fs);
            int lpLatency = -1;
            const std::vector<double> hLin = designLinearPhaseFIR(target, N, lpLatency);
            int legacyLatency = -1;
            // 3 参调用 = 0.4.4 自动路径（无零点标注），作为回归 baseline。
            const std::vector<double> hLegacy = designMinimumPhaseFIR(target, N, legacyLatency);

            const std::string cellName =
                "fs=" + std::to_string(static_cast<int>(fs))
                + " N=" + std::to_string(N)
                + " fc=" + std::to_string(static_cast<int>(fc))
                + " Q=" + std::to_string(q);

            bool finite = (mpLatency == 0);
            for (double v : hMin)
                if (!std::isfinite(v))
                    finite = false;
            check(finite, "min phase factored matrix: " + cellName + " coefficients finite / latency 0");

            const double dcLeak = dcTapSumAbs(hMin);
            check(dcLeak < 1.0e-9,
                  "min phase factored matrix: " + cellName + " exact DC zero |sum h|="
                      + std::to_string(dcLeak));

            const double highHz = std::min(500.0, fs * 0.5);
            const double minErr = maxFirDbError(hMin, target, fc, highHz, fs, 100);
            const double linErr = maxFirDbError(hLin, target, fc, highHz, fs, 100);
            const double legacyErr = maxFirDbError(hLegacy, target, fc, highHz, fs, 100);
            const double allowance = std::max(std::max(linErr, legacyErr), 1.0) + 0.5;
            const double margin = minErr - allowance;

            worstMinPhaseErr = std::max(worstMinPhaseErr, minErr);
            worstLinearPhaseErr = std::max(worstLinearPhaseErr, linErr);
            worstLegacyErr = std::max(worstLegacyErr, legacyErr);
            if (margin > worstMargin)
            {
                worstMargin = margin;
                worstCell = cellName;
            }

            check(minErr <= allowance,
                  "min phase factored matrix: " + cellName + " min-phase "
                      + std::to_string(minErr) + " dB <= max(linear " + std::to_string(linErr)
                      + " dB, legacy " + std::to_string(legacyErr) + " dB, 1 dB) + 0.5 dB");

            // 全带（>500 Hz 至 0.45·fs，target ≥ -80 dB 的点）。
            const double fullLowHz = fullBandLowHz(fs, N);
            const double fullErr =
                maxFirDbErrorAboveFloor(hMin, target, fullLowHz, fullBandHighHz(fs), fs, 200, -80.0);
            worstFullBandErr = std::max(worstFullBandErr, fullErr);
            if (fullErr > 6.0)
                worstFullCell = cellName;
            check(fullErr <= 6.0,
                  "min phase factored matrix: " + cellName + " full-band max |dB err| "
                      + std::to_string(fullErr) + " dB <= 6 dB [" + std::to_string(fullLowHz)
                      + " Hz, " + std::to_string(fullBandHighHz(fs)) + " Hz]");
        }

        std::printf("      Test 11c single-node matrix: %d cells, %d skipped (<1 bin), "
                    "worst min-phase %.3f dB, worst linear-phase %.3f dB, worst legacy %.3f dB, "
                    "worst full-band %.3f dB, tightest %s (margin %.3f dB)\n",
                    cellsRun, cellsSkippedUnderOneBin,
                    worstMinPhaseErr, worstLinearPhaseErr, worstLegacyErr,
                    worstFullBandErr, worstCell.c_str(), worstMargin);
        if (!worstFullCell.empty())
            std::printf("      Test 11c single-node: full-band >6 dB cells (last) %s\n",
                        worstFullCell.c_str());

        // ---- Test 11c 单节点 LP / BP 矩阵：Nyquist 与 DC+Nyquist 零点 ----
        // 补齐高通之外的拓扑：LowPass 在 Nyquist 有 2 阶传输零点；BandPass
        // 在 DC 与 Nyquist 各 1 阶（合计 2 阶，仍按整阶精确投影）。断言与
        // HP 单节点相同（精确零点 1e-9 / 带内相对 baseline / 全带 6 dB），
        // 覆盖 fs{48k,96k} × N{4096,16384}。
        {
            struct SingleNodeSpec
            {
                const char* name;
                double cutoffHz;
                double q;
                FilterType type;
                int dcOrder;
                int nyqOrder;
            };
            const SingleNodeSpec singleSpecs[] =
            {
                { "LP200 Q0.707", 200.0, 0.707, FilterType::LowPass,  0, 2 },
                { "BP100 Q2",     100.0, 2.0,   FilterType::BandPass, 1, 1 },
            };

            int singleRun = 0;
            int singleSkipped = 0;
            double worstSingleIn = 0.0;
            double worstSingleLin = 0.0;
            double worstSingleLegacy = 0.0;
            double worstSingleFull = 0.0;

            for (const SingleNodeSpec& spec : singleSpecs)
            for (double fs : { 48000.0, 96000.0 })
            for (int N : { 4096, 16384 })
            {
                if (spec.cutoffHz * static_cast<double>(N) / fs < 1.0)
                {
                    ++singleSkipped;
                    continue;
                }
                ++singleRun;

                BiquadCoefficients c[2];
                computeBiquadCoefficients(spec.cutoffHz, 0.0, spec.q, spec.type, fs, c);
                auto target = [&c] (double w) { return std::abs(biquadResponse(c[0], w)); };

                int mpLatency = -1;
                const std::vector<double> hMin =
                    designMinimumPhaseFIR(target, N, mpLatency, spec.dcOrder, spec.nyqOrder, fs);
                int lpLatency = -1;
                const std::vector<double> hLin = designLinearPhaseFIR(target, N, lpLatency);
                int legacyLatency = -1;
                const std::vector<double> hLegacy = designMinimumPhaseFIR(target, N, legacyLatency);

                const std::string cellName =
                    std::string("min phase single-node ") + spec.name
                    + " fs=" + std::to_string(static_cast<int>(fs))
                    + " N=" + std::to_string(N);

                bool finite = (mpLatency == 0);
                for (double v : hMin)
                    if (!std::isfinite(v))
                        finite = false;
                check(finite, cellName + " coefficients finite / latency 0");

                if (spec.dcOrder > 0)
                {
                    const double dcLeak = dcTapSumAbs(hMin);
                    check(dcLeak < 1.0e-9,
                          cellName + " exact DC zero |sum h|=" + std::to_string(dcLeak));
                }
                if (spec.nyqOrder > 0)
                {
                    const double nyqLeak = nyquistTapSumAbs(hMin);
                    check(nyqLeak < 1.0e-9,
                          cellName + " exact Nyquist zero |sum h(-1)^n|=" + std::to_string(nyqLeak));
                }

                const double highHz = std::min(500.0, fs * 0.5);
                const double minErr = maxFirDbError(hMin, target, spec.cutoffHz, highHz, fs, 100);
                const double linErr = maxFirDbError(hLin, target, spec.cutoffHz, highHz, fs, 100);
                const double legacyErr = maxFirDbError(hLegacy, target, spec.cutoffHz, highHz, fs, 100);
                const double allowance = std::max(std::max(linErr, legacyErr), 1.0) + 0.5;
                worstSingleIn = std::max(worstSingleIn, minErr);
                worstSingleLin = std::max(worstSingleLin, linErr);
                worstSingleLegacy = std::max(worstSingleLegacy, legacyErr);
                check(minErr <= allowance,
                      cellName + " min-phase " + std::to_string(minErr) + " dB <= max(linear "
                          + std::to_string(linErr) + " dB, legacy " + std::to_string(legacyErr)
                          + " dB, 1 dB) + 0.5 dB");

                const double fullErr = maxFirDbErrorAboveFloor(
                    hMin, target, fullBandLowHz(fs, N), fullBandHighHz(fs), fs, 200, -80.0);
                worstSingleFull = std::max(worstSingleFull, fullErr);
                check(fullErr <= 6.0,
                      cellName + " full-band max |dB err| " + std::to_string(fullErr) + " dB <= 6 dB");
            }

            std::printf("      Test 11c single-node LP/BP matrix: %d cells, %d skipped (<1 bin), "
                        "worst min-phase %.3f dB, worst linear-phase %.3f dB, worst legacy %.3f dB, "
                        "worst full-band %.3f dB\n",
                        singleRun, singleSkipped, worstSingleIn, worstSingleLin,
                        worstSingleLegacy, worstSingleFull);
        }

        // ---- Test 11c 多节点矩阵：级联传输零点（dc+nyq ≥ 4） ----
        // 多个 HP / LP / BP 节点把 DC/Nyquist 零点推到 4–6 阶。解析剥离的
        // 剩余幅频 R = target/factor 在该阶数下动态范围 ~(1/w_c)^{2k}，截断
        // h_rest 后与二项式因子卷积的截断误差被放大：实测多节点候选要么被
        // max|h| sanity gate 丢弃，要么（HP30+BP100）在高频偏 50–109 dB。
        // v0.4.5 复审 P1 修复后，候选先过全带 eligibility（6 dB / 峰值过冲
        // 3 dB）再按带内 fit 选优，全部不合格时回退 legacy——因此这里不再
        // 为 legacy 近似放宽判据：零点断言收紧到 |H(0)| / |H(π)| < 1e-9
        // （一阶投影实测 1e-17…1e-15），带内与单节点相同用相对 baseline
        // （legacyErr 由 3 参调用实测，故不会比 0.4.4 差），并新增全带
        // [max(0.5, fs/N), 0.45·fs] 的 6 dB 断言。矩阵覆盖
        // {2×HP(20+40), 3×HP30, 2×LP200, HP30+BP100(dc=3,nyq=1)}
        // × fs{48k, 96k} × N{4096, 16384}。
        {
            struct MultiNodeStage { double cutoffHz; double q; FilterType type; };
            struct MultiNodeSpec
            {
                const char* name;
                const MultiNodeStage* stages;
                int stageCount;
                int dcOrder;
                int nyqOrder;
                double passLowHz;
                double passHighHz;
                double minCutoffHz;
            };

            const MultiNodeStage hp20plus40[] =
            {
                { 20.0, 0.707, FilterType::HighPass },
                { 40.0, 0.707, FilterType::HighPass },
            };
            const MultiNodeStage hp30x3[] =
            {
                { 30.0, 0.707, FilterType::HighPass },
                { 30.0, 0.707, FilterType::HighPass },
                { 30.0, 0.707, FilterType::HighPass },
            };
            const MultiNodeStage lp200x2[] =
            {
                { 200.0, 0.707, FilterType::LowPass },
                { 200.0, 0.707, FilterType::LowPass },
            };
            const MultiNodeStage hp30bp100[] =
            {
                { 30.0, 0.707, FilterType::HighPass },
                { 100.0, 2.0,   FilterType::BandPass },
            };

            const MultiNodeSpec multiSpecs[] =
            {
                { "2xHP20+40",  hp20plus40, 2, 4, 0,  40.0, 500.0, 20.0 },
                { "3xHP30",     hp30x3,     3, 6, 0,  30.0, 500.0, 30.0 },
                { "2xLP200",    lp200x2,    2, 0, 4,  20.0, 200.0, 200.0 },
                { "HP30+BP100", hp30bp100, 2, 3, 1,  50.0, 200.0, 30.0 },
            };

            const double multiRates[] = { 48000.0, 96000.0 };
            const int multiLengths[] = { 4096, 16384 };

            int multiRun = 0;
            int multiSkipped = 0;
            double worstMultiErr = 0.0;
            double worstMultiLinErr = 0.0;
            double worstMultiLegacyErr = 0.0;
            double worstMultiFullErr = 0.0;
            double worstMultiPeak = 0.0;

            for (const MultiNodeSpec& spec : multiSpecs)
            for (double fs : multiRates)
            for (int N : multiLengths)
            {
                // 与单节点一致的分辨率门限：最小拐点不足 1 个设计 bin 时
                // 目标变化比设计网格还快，物理不可分辨（仅 2×HP 20 Hz @
                // 96 kHz/N=4096 命中，bins ≈ 0.85）。
                const double binsPerCutoff = spec.minCutoffHz * static_cast<double>(N) / fs;
                if (binsPerCutoff < 1.0)
                {
                    ++multiSkipped;
                    continue;
                }
                ++multiRun;

                BiquadCoefficients stages[3];
                for (int i = 0; i < spec.stageCount; ++i)
                {
                    BiquadCoefficients c[2];
                    computeBiquadCoefficients(spec.stages[i].cutoffHz, 0.0, spec.stages[i].q,
                                              spec.stages[i].type, fs, c);
                    stages[i] = c[0];
                }
                auto target = [&stages, &spec] (double w)
                {
                    double mag = 1.0;
                    for (int i = 0; i < spec.stageCount; ++i)
                        mag *= std::abs(biquadResponse(stages[i], w));
                    return mag;
                };

                int mpLatency = -1;
                const std::vector<double> hMin =
                    designMinimumPhaseFIR(target, N, mpLatency, spec.dcOrder, spec.nyqOrder, fs);
                int lpLatency = -1;
                const std::vector<double> hLin = designLinearPhaseFIR(target, N, lpLatency);
                int legacyLatency = -1;
                const std::vector<double> hLegacy = designMinimumPhaseFIR(target, N, legacyLatency);

                const std::string cellName =
                    std::string("min phase multi-node matrix: ") + spec.name
                    + " fs=" + std::to_string(static_cast<int>(fs))
                    + " N=" + std::to_string(N);

                bool finite = (mpLatency == 0);
                for (double v : hMin)
                    if (!std::isfinite(v))
                        finite = false;
                check(finite, cellName + " coefficients finite / latency 0");

                const double peak = maxTapMagnitude(hMin);
                worstMultiPeak = std::max(worstMultiPeak, peak);
                check(peak < 1.0e3, cellName + " max|h| < 1e3 (peak=" + std::to_string(peak) + ")");

                if (spec.dcOrder > 0)
                {
                    const double dcLeak = dcTapSumAbs(hMin);
                    check(dcLeak < 1.0e-9,
                          cellName + " DC zero |sum h| < 1e-9 (" + std::to_string(dcLeak) + ")");
                }
                if (spec.nyqOrder > 0)
                {
                    const double nyqLeak = nyquistTapSumAbs(hMin);
                    check(nyqLeak < 1.0e-9,
                          cellName + " Nyquist zero |sum h(-1)^n| < 1e-9 ("
                              + std::to_string(nyqLeak) + ")");
                }

                const double highHz = std::min(spec.passHighHz, fs * 0.5);
                const double minErr = maxFirDbError(hMin, target, spec.passLowHz, highHz, fs, 100);
                const double linErr = maxFirDbError(hLin, target, spec.passLowHz, highHz, fs, 100);
                const double legacyErr = maxFirDbError(hLegacy, target, spec.passLowHz, highHz, fs, 100);
                const double allowance = std::max(std::max(linErr, legacyErr), 1.0) + 0.5;
                worstMultiErr = std::max(worstMultiErr, minErr);
                worstMultiLinErr = std::max(worstMultiLinErr, linErr);
                worstMultiLegacyErr = std::max(worstMultiLegacyErr, legacyErr);
                check(minErr <= allowance,
                      cellName + " min-phase " + std::to_string(minErr) + " dB <= max(linear "
                          + std::to_string(linErr) + " dB, legacy " + std::to_string(legacyErr)
                          + " dB, 1 dB) + 0.5 dB");

                const double fullErr = maxFirDbErrorAboveFloor(
                    hMin, target, fullBandLowHz(fs, N), fullBandHighHz(fs), fs, 200, -80.0);
                worstMultiFullErr = std::max(worstMultiFullErr, fullErr);
                check(fullErr <= 6.0,
                      cellName + " full-band max |dB err| " + std::to_string(fullErr) + " dB <= 6 dB");
            }

            std::printf("      Test 11c multi-node matrix: %d cells, %d skipped (<1 bin), "
                        "worst min-phase %.3f dB, worst linear-phase %.3f dB, worst legacy %.3f dB, "
                        "worst full-band %.3f dB, worst peak %.3e\n",
                        multiRun, multiSkipped, worstMultiErr, worstMultiLinErr,
                        worstMultiLegacyErr, worstMultiFullErr, worstMultiPeak);
        }

        // ---- 65536 点抽查：高采样率低频 HP、低 Q HP、Nyquist 零点 LP ----
        struct SpotCheck
        {
            const char* name;
            double fs;
            double fc;
            double q;
            FilterType type;
            int dcOrder;
            int nyqOrder;
        };

        const SpotCheck spots[] =
        {
            { "48k HP30 Q0.707 N=65536",  48000.0,  30.0, 0.707, FilterType::HighPass, 2, 0 },
            { "192k HP100 Q2 N=65536",   192000.0, 100.0, 2.0,   FilterType::HighPass, 2, 0 },
            { "48k LP200 Q0.707 N=65536", 48000.0, 200.0, 0.707, FilterType::LowPass,  0, 2 },
        };
        const int spotN = 65536;

        for (const SpotCheck& s : spots)
        {
            BiquadCoefficients c[2];
            computeBiquadCoefficients(s.fc, 0.0, s.q, s.type, s.fs, c);
            auto target = [&c] (double w) { return std::abs(biquadResponse(c[0], w)); };

            int mpLatency = -1;
            const std::vector<double> hMin =
                designMinimumPhaseFIR(target, spotN, mpLatency, s.dcOrder, s.nyqOrder, s.fs);
            int lpLatency = -1;
            const std::vector<double> hLin = designLinearPhaseFIR(target, spotN, lpLatency);
            int legacyLatency = -1;
            const std::vector<double> hLegacy = designMinimumPhaseFIR(target, spotN, legacyLatency);

            const std::string spotName = std::string("min phase factored 65536: ") + s.name;

            bool finite = (mpLatency == 0);
            for (double v : hMin)
                if (!std::isfinite(v))
                    finite = false;
            check(finite, spotName + " coefficients finite / latency 0");

            if (s.type == FilterType::HighPass)
            {
                const double dcLeak = dcTapSumAbs(hMin);
                check(dcLeak < 1.0e-9, spotName + " exact DC zero |sum h|=" + std::to_string(dcLeak));
            }
            else
            {
                const double nyqLeak = nyquistTapSumAbs(hMin);
                check(nyqLeak < 1.0e-9,
                      spotName + " exact Nyquist zero |sum h[n](-1)^n|=" + std::to_string(nyqLeak));
            }

            const double highHz = std::min(500.0, s.fs * 0.5);
            const double minErr = maxFirDbError(hMin, target, s.fc, highHz, s.fs, 100);
            const double linErr = maxFirDbError(hLin, target, s.fc, highHz, s.fs, 100);
            const double legacyErr = maxFirDbError(hLegacy, target, s.fc, highHz, s.fs, 100);
            const double allowance = std::max(std::max(linErr, legacyErr), 1.0) + 0.5;
            check(minErr <= allowance,
                  spotName + " min-phase " + std::to_string(minErr) + " dB <= max(linear "
                      + std::to_string(linErr) + " dB, legacy " + std::to_string(legacyErr)
                      + " dB, 1 dB) + 0.5 dB");

            const double fullErr = maxFirDbErrorAboveFloor(
                hMin, target, fullBandLowHz(s.fs, spotN), fullBandHighHz(s.fs), s.fs, 200, -80.0);
            check(fullErr <= 6.0,
                  spotName + " full-band max |dB err| " + std::to_string(fullErr) + " dB <= 6 dB");
        }
    }

    // ---- Test 11d: 合法高增益目标不得被 max|h| sanity gate 误杀 ----
    // 回归 v0.4.5 round 3 复审 P2：`max|h| ≤ 1e3` 的 sanity gate 本意是探
    // 测多节点解析剥离的灾难性相消，却同时作用到 legacy 兜底候选——一个
    // 合法的高增益级联（HP30 + 2×LowShelf(+24@200) + 2×HighShelf(+24@200)
    // + master +24，通带 +72 dB ⇒ 峰值 ≈4e3）会让三个候选全被拒绝，最终
    // 返回单位冲激：peak=1、Σh=1（HP 的 DC 零点丢失）、100 Hz/1 kHz 偏 −72 dB。
    // 0.4.4 三参路径同目标返回 peak≈3966.86、误差 −0.08/0.00 dB。
    // 修复后 legacy 兜底候选不受峰值 gate 约束（它是 0.4.4 安全基线），且
    // factored 候选不合格时直接返回它。
    {
        const double fs = 48000.0;
        const int N = 4096;

        BiquadCoefficients hp[2];
        computeBiquadCoefficients(30.0, 0.0, 0.707, FilterType::HighPass, fs, hp);
        BiquadCoefficients lowShelf[2];
        computeBiquadCoefficients(200.0, 24.0, 0.707, FilterType::LowShelf, fs, lowShelf);
        BiquadCoefficients highShelf[2];
        computeBiquadCoefficients(200.0, 24.0, 0.707, FilterType::HighShelf, fs, highShelf);
        const double masterGain = dbToGain(24.0);   // +24 dB master

        // HP30 × LowShelf(+24)² × HighShelf(+24)² × master(+24) = 合法 +72 dB 级联。
        auto target = [&] (double w)
        {
            double mag = std::abs(biquadResponse(hp[0], w));
            mag *= std::abs(biquadResponse(lowShelf[0], w));
            mag *= std::abs(biquadResponse(lowShelf[0], w));
            mag *= std::abs(biquadResponse(highShelf[0], w));
            mag *= std::abs(biquadResponse(highShelf[0], w));
            return mag * masterGain;
        };

        int mpLatency = -1;
        const std::vector<double> hMin = designMinimumPhaseFIR(target, N, mpLatency, 2, 0, fs);
        int legacyLatency = -1;
        // 3 参调用 = 0.4.4 自动路径（无零点标注），作为高增益兜底 baseline。
        const std::vector<double> hLegacy = designMinimumPhaseFIR(target, N, legacyLatency);
        int lpLatency = -1;
        const std::vector<double> hLin = designLinearPhaseFIR(target, N, lpLatency);

        const std::string cellName = "min phase high-gain +72 dB: fs=48000 N=4096 dc=2";

        bool finite = (mpLatency == 0);
        bool hasInf = false;
        for (double v : hMin)
        {
            if (!std::isfinite(v))
                finite = false;
            if (std::isinf(v))
                hasInf = true;
        }
        check(finite && !hasInf, cellName + " coefficients finite (no NaN/Inf) / latency 0");

        const double peak = maxTapMagnitude(hMin);
        check(peak > 1.0,
              cellName + " not a unit impulse (max|h|=" + std::to_string(peak) + " > 1)");

        // h[0] 冲激判据的显式复核：单位冲激 h = [1,0,0,...]（修复前实测）。
        bool impulseLike = std::abs(hMin[0] - 1.0) < 1e-12;
        for (int i = 1; i < N && impulseLike; ++i)
            if (std::abs(hMin[static_cast<size_t>(i)]) > 1e-12)
                impulseLike = false;
        check(!impulseLike, cellName + " h[0] does not satisfy the unit-impulse criterion");

        // HP 的 DC 传输零点：结构相消必须精确（修复前 h 是冲激，Σh = 1）。
        const double dcLeak = dcTapSumAbs(hMin);
        check(dcLeak < 1.0e-6,
              cellName + " exact DC zero |sum h|=" + std::to_string(dcLeak) + " < 1e-6");

        // 通带 [fc, 500 Hz]：相对线性相位 / 3 参 legacy 的 max + 0.5 dB，且
        // 绝对不超过 2 dB（高增益目标不应因兜底而损失通带精度）。
        const double highHz = std::min(500.0, fs * 0.5);
        const double minErr = maxFirDbError(hMin, target, 30.0, highHz, fs, 100);
        const double linErr = maxFirDbError(hLin, target, 30.0, highHz, fs, 100);
        const double legacyErr = maxFirDbError(hLegacy, target, 30.0, highHz, fs, 100);
        const double allowance = std::max(linErr, legacyErr) + 0.5;

        std::printf("      Test 11d high-gain +72 dB: peak %.6f, |sum h| %.3e, "
                    "passband err %.4f dB (linear %.4f, legacy %.4f)\n",
                    peak, dcLeak, minErr, linErr, legacyErr);

        check(minErr <= allowance,
              cellName + " passband " + std::to_string(minErr) + " dB <= max(linear "
                  + std::to_string(linErr) + ", legacy " + std::to_string(legacyErr)
                  + ") + 0.5 dB");
        check(minErr <= 2.0,
              cellName + " passband " + std::to_string(minErr) + " dB <= 2 dB");
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

    // ---- Test 13: spectrum math (octave bands / Hann window basics) ----
    {
        check(octaveDivisorForBands(61) == 6, "octave divisor 61 bands == 6");
        check(octaveDivisorForBands(121) == 12, "octave divisor 121 bands == 12");
        check(std::abs(octaveBandCenterFreq(0.5f, 0, 61) - 0.5f) < 1e-6f, "octave band 0 center == 0.5 Hz");
        check(std::abs(octaveBandCenterFreq(0.5f, 6, 61) - 1.0f) < 1e-6f, "octave band 6 (1/6 oct) == 1.0 Hz");
        check(std::abs(octaveBandCenterFreq(0.5f, 12, 121) - 1.0f) < 1e-6f, "octave band 12 (1/12 oct) == 1.0 Hz");

        const int N = 1025;   // odd: the exact centre sample lands on i=(N-1)/2
        check(std::abs(windowValue(WindowType::Hann, 0, N)) < 1e-9, "hann window[0] == 0");
        check(std::abs(windowValue(WindowType::Hann, N - 1, N)) < 1e-9, "hann window[N-1] == 0");
        check(std::abs(windowValue(WindowType::Hann, (N - 1) / 2, N) - 1.0) < 1e-9, "hann window[mid] == 1");
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
        check(spectrumResolutionChoiceToIndex(0) == 0, "resolution choice 0 -> Fast (0)");
        check(spectrumResolutionChoiceToIndex(1) == 1, "resolution choice 1 -> Balanced (1)");
        check(spectrumResolutionChoiceToIndex(2) == 2, "resolution choice 2 -> Fine (2)");
        check(spectrumWindowChoiceToType(0) == WindowType::Hann, "window choice 0 -> Hann");
        check(spectrumWindowChoiceToType(1) == WindowType::Blackman, "window choice 1 -> Blackman");
        check(spectrumWindowChoiceToType(2) == WindowType::BlackmanHarris, "window choice 2 -> Blackman-Harris");
        check(spectrumWindowChoiceToType(3) == WindowType::Nuttall, "window choice 3 -> Nuttall");
        check(spectrumDensityChoiceToBands(0) == 61, "density choice 0 -> 61 bands");
        check(spectrumDensityChoiceToBands(1) == 121, "density choice 1 -> 121 bands");
        check(spectrumHopChoiceToSamples(0) == 512, "hop choice 0 -> 512");
        check(spectrumHopChoiceToSamples(1) == 1024, "hop choice 1 -> 1024");
        check(spectrumHopChoiceToSamples(2) == 2048, "hop choice 2 -> 2048");
        check(spectrumRefreshChoiceToHz(0) == 15, "refresh choice 0 -> 15 Hz");
        check(spectrumRefreshChoiceToHz(1) == 30, "refresh choice 1 -> 30 Hz");
        check(spectrumRefreshChoiceToHz(2) == 60, "refresh choice 2 -> 60 Hz");
        check(spectrumResolutionChoiceToIndex(99) == 1, "resolution choice invalid -> Balanced");
        check(spectrumWindowChoiceToType(99) == WindowType::BlackmanHarris, "window choice invalid -> BH-4");
        check(spectrumDensityChoiceToBands(99) == 61, "density choice invalid -> 61");
        check(spectrumHopChoiceToSamples(99) == 512, "hop choice invalid -> 512");
        check(spectrumRefreshChoiceToHz(99) == 30, "refresh choice invalid -> 30 Hz");
        check(spectrumSmoothingChoiceToIndex(0) == 0, "smoothing choice 0 -> Off");
        check(spectrumSmoothingChoiceToIndex(1) == 1, "smoothing choice 1 -> 1/6 oct");
        check(spectrumSmoothingChoiceToIndex(2) == 2, "smoothing choice 2 -> 1/3 oct");
        check(spectrumSmoothingChoiceToIndex(3) == 3, "smoothing choice 3 -> 2/3 oct");
        check(spectrumSmoothingChoiceToIndex(99) == 1, "smoothing choice invalid -> 1/6 oct");
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

        // 非法 FilterType：default 分支写单位系数并返回 1（F-023），
        // 调用方不会读到陈旧系数。
        BiquadCoefficients bad[2];
        bad[0].b0 = 99.0; bad[0].b1 = 77.0; bad[0].a1 = -0.5;
        const int nb = computeBiquadCoefficients(100.0, 3.0, 0.7,
                                                 static_cast<FilterType>(99), sr, bad);
        const bool identOut = (nb == 1)
                           && std::abs(bad[0].b0 - 1.0) < 1e-15
                           && std::abs(bad[0].b1) < 1e-15
                           && std::abs(bad[0].b2) < 1e-15
                           && std::abs(bad[0].a1) < 1e-15
                           && std::abs(bad[0].a2) < 1e-15;
        check(identOut, "invalid FilterType returns identity biquad (default branch)");
    }

    // ---- Test 21: biquad cascade fade plan (Tilt <-> Bell state machine, F-007) ----
    {
        // Bell -> Tilt：第二 biquad 自 identity 淡入
        auto a = computeBiquadFadePlan(1, 2, false);
        check(!a.fadeOutExtra && !a.startFromCurrent, "Bell->Tilt fades 2nd biquad in from identity");

        // Tilt -> Bell：第二 biquad 向 identity 淡出
        auto b = computeBiquadFadePlan(2, 1, false);
        check(b.fadeOutExtra && b.startFromCurrent, "Tilt->Bell fades 2nd biquad out to identity");

        // Tilt->Bell 淡出窗口内再次 update()（numBiquads==1, prev==1）：
        // 必须继续从当前半淡出系数淡出，而不是瞬间丢弃其贡献（F-007）
        auto c = computeBiquadFadePlan(1, 1, true);
        check(c.fadeOutExtra && c.startFromCurrent, "mid-fade-out re-update keeps fading from current coeffs");

        // 淡出窗口内切回 Tilt：从当前半淡出系数继续淡入
        auto d = computeBiquadFadePlan(1, 2, true);
        check(!d.fadeOutExtra && d.startFromCurrent, "Bell->Tilt during fade-out continues from current coeffs");

        // 单 biquad 常态：无额外淡化状态
        auto e = computeBiquadFadePlan(1, 1, false);
        check(!e.fadeOutExtra && !e.startFromCurrent, "single-biquad steady state has no extra fade");
    }

    // ---- Test 22: window family (mean / ENBW / sidelobe) ----
    {
        const int M = 8192;
        const WindowType types[] = { WindowType::Hann, WindowType::Blackman,
                                     WindowType::BlackmanHarris, WindowType::Nuttall };

        for (WindowType wt : types)
        {
            std::vector<double> w(static_cast<size_t> (M));
            double wSum = 0.0, w2Sum = 0.0;
            for (int i = 0; i < M; ++i)
            {
                w[static_cast<size_t> (i)] = windowValue (wt, i, M);
                wSum  += w[static_cast<size_t> (i)];
                w2Sum += w[static_cast<size_t> (i)] * w[static_cast<size_t> (i)];
            }

            // 均值 → 解析相干增益 a0
            check(std::abs(wSum / M - windowMean(wt)) < 3.0e-3, "window mean ~= analytic a0");

            // ENBW = M·Σw²/(Σw)² → 文献值
            const double enbw = M * w2Sum / (wSum * wSum);
            check(std::abs(enbw - windowEnbwBins(wt)) < 0.05, "window ENBW ~= datasheet");

            // 端点：cosine-sum 窗端点 = a0 − a1 + a2 − a3（Hann 精确为 0；
            // Blackman/Nuttall 的系数和在浮点下可能产生 ~1e-17 的负零）
            check(std::abs(w.front()) < 1.0e-3, "window endpoint ~0");
            check(std::abs(w.front() - w.back()) < 1.0e-12, "window symmetric endpoints");

            // 实测首旁瓣（16384 点 FFT，跳过主瓣区）：不得比标称差 3 dB 以上
            const int Nfft = 16384;
            std::vector<std::complex<double>> spec (static_cast<size_t> (Nfft), {0.0, 0.0});
            for (int i = 0; i < M; ++i)
                spec[static_cast<size_t> (i)] = { w[static_cast<size_t> (i)], 0.0 };
            fftInPlace (spec.data(), Nfft);

            int pk = 0;
            double peak = -1.0;
            for (int m = 0; m < Nfft / 2; ++m)
            {
                const double a = std::abs (spec[static_cast<size_t> (m)]);
                if (a > peak) { peak = a; pk = m; }
            }
            // 主瓣半宽（bin）：Hann 2·N/M，BH/Nuttall ~4·N/M（Heinzel 表）
            const int halfMainLobe = static_cast<int> (1.6 * 4.0 * Nfft / M);
            double sidelobe = 0.0;
            for (int m = pk + halfMainLobe; m < Nfft / 2; ++m)
                sidelobe = std::max (sidelobe, std::abs (spec[static_cast<size_t> (m)]));
            const double sidelobeDb = 20.0 * std::log10 (sidelobe / peak);
            check(sidelobeDb <= windowSidelobeDb(wt) + 3.0, "window sidelobe within +3 dB of datasheet");
            std::printf ("      window %-14s ENBW=%.2f bins  sidelobe=%.1f dB (nominal %.1f)\n",
                         wt == WindowType::Hann ? "Hann" :
                         wt == WindowType::Blackman ? "Blackman" :
                         wt == WindowType::BlackmanHarris ? "BlackmanHarris" : "Nuttall",
                         enbw, sidelobeDb, windowSidelobeDb (wt));
        }
    }

    // ---- Test 23: VQT tier layout & kernel design ----
    {
        check(vqtTopDecimationExponent(48000.0) == 4,  "48k top decimation -> 16 (3 kHz)");
        check(vqtTopDecimationExponent(44100.0) == 2,  "44.1k top decimation -> 4 (11.025 kHz, 16 not integral)");
        check(vqtTopDecimationExponent(96000.0) == 5,  "96k top decimation -> 32 (3 kHz)");
        check(vqtTopDecimationExponent(192000.0) == 6, "192k top decimation -> 64 (3 kHz)");

        check(vqtTierStride(16, 512) == 32, "tier stride D=16 hop512 -> 32");
        check(vqtTierStride(256, 512) == 2, "tier stride D=256 hop512 -> 2");
        check(vqtTierStride(2048, 512) == 1, "tier stride D=2048 hop512 -> 1 (clamped)");

        const double fs = 48000.0;
        for (int numBands : { 61, 121 })
        {
            for (double scale : { 0.7, 1.0, 1.5 })
            {
                const VQTKernelSet set = buildVQTKernelSet (fs, numBands, scale, WindowType::BlackmanHarris);

                // 层率：顶层 fs/2^d9，向下逐层 ÷2；频段划分总和 == numBands
                int totalBands = 0;
                bool ladderOk = true, pow2Ok = true, capOk = true;
                const int d9 = vqtTopDecimationExponent (fs);
                for (int t = 0; t < VQTNumTiers; ++t)
                {
                    const VQTTier& T = set.tiers[t];
                    if (T.decimation != (1 << (d9 + VQTNumTiers - 1 - t))) ladderOk = false;
                    if (T.fftSize < nextPowerOfTwo(T.fftSize)) pow2Ok = false;
                    if (T.windowSeconds > VQTTierCapSeconds + 1.0e-9) capOk = false;
                    if (T.fftSize < VQTMinTierFftSize) pow2Ok = false;
                    totalBands += T.numBandsInTier;
                }
                check(ladderOk, "tier decimation ladder");
                check(pow2Ok, "tier fft size is pow2 (>= guard)");
                check(capOk, "tier windows respect 2 s cap (pow2-quantised)");
                check(totalBands == numBands, "tier band partition sums to band count");

                // 核支撑必须落在帧谱范围内且非空
                bool supportOk = true;
                for (int t = 0; t < VQTNumTiers; ++t)
                    for (const auto& k : set.kernels[t])
                        if (k.loBin < 0 || k.hiBin >= set.tiers[t].fftSize
                            || k.hiBin < k.loBin || k.taps.empty())
                            supportOk = false;
                check(supportOk, "kernel spectral support within frame");
            }
        }

        // 层窗时长表（informational）：封顶层与满分辨交叉点，供调研报告引用。
        for (WindowType wt : { WindowType::Hann, WindowType::BlackmanHarris, WindowType::Nuttall })
        {
            const VQTKernelSet set = buildVQTKernelSet (fs, 121, 1.0, wt);
            std::printf ("      window %-14s tier windows (s):", 
                         wt == WindowType::Hann ? "Hann" :
                         wt == WindowType::BlackmanHarris ? "BlackmanHarris" : "Nuttall");
            for (int t = 0; t < VQTNumTiers; ++t)
                std::printf (" %.3f", set.tiers[t].windowSeconds);
            std::printf ("\n");
        }
    }

    // ---- Test 24: offline VQT engine (calibration / leakage / DC / benchmark) ----
    {
        const double fs = 48000.0;
        const int numBands = 121;
        const int hop = 512;

        // 校准：满幅正弦落段中心 -> 0 dB（逐窗型、跨十倍频程的频段抽样）
        const double freqs[] = { 0.5, 1.0, 4.0, 16.0, 63.5, 200.0, 453.0 };
        for (WindowType wt : { WindowType::Hann, WindowType::Blackman,
                               WindowType::BlackmanHarris, WindowType::Nuttall })
        {
            const VQTKernelSet set = buildVQTKernelSet (fs, numBands, 1.0, wt);
            OfflineVqtAnalyzer a;
            bool ok = true;
            double worst = 0.0;
            for (double f : freqs)
            {
                a.prepare (fs, set, hop);
                const int band = OfflineVqtAnalyzer::bandIndexNear (f, numBands);
                // 喂"段中心频率"而非名义频率：段间裙响应（scalloping）不是校准
                // 误差——0.27 个段距的偏移本就应读 ~-2 dB（BH 主瓣更宽，裙更沉）。
                const double fc = octaveBandCenterFreq (0.5f, band, numBands);
                const int t = vqtBandTier (band, 12);
                const double settle = 3.0 * set.tiers[t].windowSeconds + 1.0;
                feedSignal (a, fc, 1.0, settle, fs);
                // v0.4.1：显示包络对底层帧摆动近乎透明（层 dt >> 包络 τ，读数
                // 诚实跟随每帧功率），gamma 段馈末读数依赖摆动相位——校准改为对
                // 尾段做功率域时间平均：测的是核的无偏性，与摆动相位解耦。
                const double measure = 3.0 * set.tiers[t].windowSeconds + 2.0;
                const double w = 2.0 * 3.14159265358979323846 * fc / fs;
                const int measureN = static_cast<int> (measure * fs);
                double pAcc = 0.0;
                int pCount = 0;
                for (int i = 0; i < measureN; ++i)
                {
                    a.processSample (std::sin (w * i));
                    if ((i & 511) == 511)   // 每 ~10.7 ms 采样一次显示值
                    {
                        pAcc += std::pow (10.0, a.bandDb[band] / 10.0);
                        ++pCount;
                    }
                }
                const double reading = 10.0 * std::log10 (pAcc / pCount);
                // gamma 区容差：窗内不足 ~2 个整周期的频段（2 s 封顶下的 0.5 Hz
                // 段），实信号镜像项落在主瓣裙内，读数有 ±2 dB 量级的相位相依摆
                // 动（物理后果，见调研报告）；其余频段容差 0.75 dB。
                const double cycles = fc * set.tiers[t].windowSeconds;
                const double tol = (cycles < 2.0) ? 3.5 : 0.75;
                const double err = std::abs (reading);
                std::printf ("      calib %-14s %7.2f Hz (band %3d, %.2f cyc/win): %+6.2f dB\n",
                             wt == WindowType::Hann ? "Hann" :
                             wt == WindowType::Blackman ? "Blackman" :
                             wt == WindowType::BlackmanHarris ? "BlackmanHarris" : "Nuttall",
                             fc, band, cycles, reading);
                worst = std::max (worst, err);
                if (err > tol) ok = false;
            }
            check(ok, "full-scale sine at band centre reads 0 dB (+/-0.75)");
            std::printf ("      window %-14s worst calibration error %.2f dB\n",
                         wt == WindowType::Hann ? "Hann" :
                         wt == WindowType::Blackman ? "Blackman" :
                         wt == WindowType::BlackmanHarris ? "BlackmanHarris" : "Nuttall",
                         worst);
        }

        // 泄漏（窗函数专项基准）：~100 Hz 满幅单音，3 个频段之外必须安静。
        // 读未钳位的 rawDb：BH-4 旁瓣 -92 dB -> +3 段 <= -70 dB；Hann 裙摆
        // -31.5 dB 衰减 18 dB/oct（打印对照）。
        for (WindowType wt : { WindowType::BlackmanHarris, WindowType::Hann })
        {
            const VQTKernelSet set = buildVQTKernelSet (fs, numBands, 1.0, wt);
            OfflineVqtAnalyzer a;
            a.prepare (fs, set, hop);
            const int band = OfflineVqtAnalyzer::bandIndexNear (100.0, numBands);
            feedSignal (a, octaveBandCenterFreq (0.5f, band, numBands), 1.0, 2.0, fs);

            const double b3 = a.rawDb[band + 3];
            const double b5 = a.rawDb[band + 5];
            if (wt == WindowType::BlackmanHarris)
            {
                check(b3 <= -70.0, "BH-4: tone 3 bands away suppressed below -70 dB");
                check(b5 <= -80.0, "BH-4: tone 5 bands away suppressed below -80 dB");
            }
            std::printf ("      leakage (100 Hz tone): %-14s +3 bands %.1f dB, +5 bands %.1f dB\n",
                         wt == WindowType::BlackmanHarris ? "BlackmanHarris" : "Hann", b3, b5);
        }

        // 直流抑制：0.3 常量偏置，DC 阻断 + 核旁瓣联合作用下所有频段 <= -30 dB
        {
            const VQTKernelSet set = buildVQTKernelSet (fs, numBands, 1.0, WindowType::BlackmanHarris);
            OfflineVqtAnalyzer a;
            a.prepare (fs, set, hop);
            for (int i = 0; i < static_cast<int> (12.0 * fs); ++i)
                a.processSample (0.3);
            bool ok = true;
            for (int b = 0; b < numBands; ++b)
                if (a.bandDb[b] > -30.0) ok = false;
            check(ok, "DC offset 0.3 keeps all bands below -30 dB");
        }

        // 双音分辨：28 / 34 Hz 各 0.5 幅度 -> 两个中心段各读 ~ -6 dB
        {
            const VQTKernelSet set = buildVQTKernelSet (fs, numBands, 1.5, WindowType::BlackmanHarris);
            OfflineVqtAnalyzer a;
            a.prepare (fs, set, hop);
            const double w1 = 2.0 * 3.14159265358979323846 * 28.0 / fs;
            const double w2 = 2.0 * 3.14159265358979323846 * 34.0 / fs;
            for (int i = 0; i < static_cast<int> (8.0 * fs); ++i)
                a.processSample (0.5 * std::sin (w1 * i) + 0.5 * std::sin (w2 * i));

            const int b28 = OfflineVqtAnalyzer::bandIndexNear (28.0, numBands);
            const int b34 = OfflineVqtAnalyzer::bandIndexNear (34.0, numBands);
            check(a.bandDb[b28] > -8.0 && a.bandDb[b28] < -4.0, "28 Hz tone reads ~-6 dB");
            check(a.bandDb[b34] > -8.0 && a.bandDb[b34] < -4.0, "34 Hz tone reads ~-6 dB");
            check(b28 != b34, "28/34 Hz fall into distinct bands");
        }

        // 性能基准（informational + 断言）：5 s @48k 单分析器。
        // 旧方案 = 每 512 样本一次 8192 点 double FFT；VQT 应明显更快。
        {
            const VQTKernelSet set = buildVQTKernelSet (fs, numBands, 1.0, WindowType::BlackmanHarris);
            OfflineVqtAnalyzer a;
            a.prepare (fs, set, hop);
            const int n = static_cast<int> (5.0 * fs);
            const double w = 2.0 * 3.14159265358979323846 * 55.0 / fs;

            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < n; ++i)
                a.processSample (std::sin (w * i));
            auto t1 = std::chrono::steady_clock::now();
            const double vqtMs = std::chrono::duration<double, std::milli> (t1 - t0).count();

            std::vector<std::complex<double>> legacy (8192);
            auto t2 = std::chrono::steady_clock::now();
            int legacyFfts = 0;
            for (int i = 0; i < n; i += 512)
            {
                for (int j = 0; j < 8192; ++j)
                    legacy[static_cast<size_t> (j)] = { std::sin (w * (i + j)), 0.0 };
                fftInPlace (legacy.data(), 8192);
                ++legacyFfts;
            }
            auto t3 = std::chrono::steady_clock::now();
            const double legacyMs = std::chrono::duration<double, std::milli> (t3 - t2).count();

            std::printf ("BENCH: 5 s @48k  VQT(121 bands)=%.1f ms  legacy(8192-pt FFT/hop512)=%.1f ms  (%d FFTs)\n",
                         vqtMs, legacyMs, legacyFfts);
            check(vqtMs < legacyMs, "VQT pipeline cheaper than legacy per-hop FFT");
        }
    }

    // ---- Test 25: display smoothing math (v0.4.1, power domain) ----
    {
        // 半径查表：61/121 段各自交付近似相同的倍频程平滑宽度
        check(smoothingRadiusBands(0, 61) == 0 && smoothingRadiusBands(0, 121) == 0,
              "smoothing radius: Off -> 0 for both densities");
        check(smoothingRadiusBands(1, 61) == 1 && smoothingRadiusBands(1, 121) == 2,
              "smoothing radius: 1/6 oct -> r1(61) / r2(121)");
        check(smoothingRadiusBands(2, 61) == 2 && smoothingRadiusBands(2, 121) == 4,
              "smoothing radius: 1/3 oct -> r2(61) / r4(121)");
        check(smoothingRadiusBands(3, 61) == 4 && smoothingRadiusBands(3, 121) == 8,
              "smoothing radius: 2/3 oct -> r4(61) / r8(121)");
        check(smoothingRadiusBands(-3, 121) == 0 && smoothingRadiusBands(99, 61) == 4,
              "smoothing radius: index clamped (<0 -> Off, >3 -> strongest)");

        // 恒等与归一化：radius 0 原样返回；全等功率向量平滑后不变
        //（证明权重和 == 1，含边缘截断重归一）
        double p[8] = { 1e-9, 0.02, 1.0, 0.5, 0.25, 1e-7, 3.0, 0.001 };
        check(smoothBandPowerAt(p, 3, 8, 0) == 0.5, "smoothing radius 0 is identity");
        double eq[8] = { 0.7, 0.7, 0.7, 0.7, 0.7, 0.7, 0.7, 0.7 };
        bool eqOk = true;
        for (int b = 0; b < 8; ++b)
            for (int r = 1; r <= 4; ++r)
                if (std::abs(smoothBandPowerAt(eq, b, 8, r) - 0.7) > 1e-12) eqOk = false;
        check(eqOk, "uniform powers smooth to themselves (renormalized, incl. edges)");

        // 功率域而非 dB 域（Jensen 偏差）：{0.001, 1.0} 两段，band 0 截断
        // 重归一（权重 {1, 0.5}/1.5）：功率域 = (0.001 + 0.5)/1.5 = 0.334 ->
        // -4.76 dB；dB 域平均 = (-30 + 0)/2 = -15 dB，差 10 dB。
        double two[2] = { 0.001, 1.0 };
        const double s0 = smoothBandPowerAt(two, 0, 2, 1);
        check(std::abs(s0 - (0.001 + 0.5) / 1.5) < 1e-12,
              "edge band renormalized power-domain weighted average");
        check(10.0 * std::log10(s0) - (-15.0) > 8.0,
              "power-domain averaging beats dB-domain averaging by >8 dB on this vector");

        // 峰展宽：单段冲激（其余近零）——峰值随半径单调下降、不抬升，邻段抬升
        double imp[16] = {};
        imp[7] = 1.0;
        bool monoOk = true;
        double prevPeak = imp[7];
        for (int r = 1; r <= 4; ++r)
        {
            const double peak = smoothBandPowerAt(imp, 7, 16, r);
            if (peak >= prevPeak || peak > imp[7]) monoOk = false;
            if (smoothBandPowerAt(imp, 8, 16, r) <= imp[8]) monoOk = false;
            prevPeak = peak;
        }
        check(monoOk, "impulse peak drops monotonically with radius, neighbour rises");
        check(std::abs(10.0 * std::log10(smoothBandPowerAt(imp, 7, 16, 2)) - 10.0 * std::log10(1.0 / 3.0)) < 0.05,
              "impulse peak drop at r=2 matches Hann kernel theory (-4.77 dB = 1/{0.25,0.75,1,0.75,0.25} sum 3)");

        // 地板与功率初值必须互为精确换算（R4-F4）：头文件里的 static_assert 只在
        // 编译期把关「地板是 10 dB 整数倍」，此处再按运行时读取的常量值核对一次
        // 二者确实相等——10·log10(kDisplayFloorPower) 必须落在 kDisplayFloorDb 上。
        check(kDisplayFloorSteps * 10 == static_cast<int>(-kDisplayFloorDb)
              && std::abs(10.0 * std::log10(kDisplayFloorPower) - kDisplayFloorDb) < 1e-9,
              "display floor dB and floor power are exact reciprocals");

        // 弹道系数：α = 1 - e^(-dt/τ)
        check(ballisticAlpha(0.0, 0.02) == 0.0, "ballisticAlpha dt=0 -> 0");
        check(ballisticAlpha(1.0, 0.0) == 1.0, "ballisticAlpha tau<=0 -> 1 (instant)");
        check(std::abs(ballisticAlpha(0.02, 0.02) - (1.0 - std::exp(-1.0))) < 1e-12,
              "ballisticAlpha dt==tau -> 1-e^-1");
        check(ballisticAlpha(100.0, 0.02) == 1.0, "ballisticAlpha clamps to 1");

        // 时间归一化的恒 dB/s 释放（v0.4.1 核心修复的单元级锁）：顶层 dt≈10.7 ms
        // 与最底层 dt≈170.7 ms 必须以同一物理速率下落——v0.4.0 的按迭代系数
        // 在这两层差 ~16 倍。1-α = e^(-dt/τ) 精确成立，故下落时间无离散误差。
        {
            const double fs = 48000.0;
            const double dtTop = 512.0 / fs;        // 顶层 6 层：每 hop 分析一次
            const double dtBottom = 8192.0 / fs;    // 最底层：步距钳 1，每层样本一次
            auto fallTime20dB = [](double dt)
            {
                double env = 1.0;                    // 0 dB 起步
                const double alpha = ballisticAlpha(dt, SpectrumReleaseTau);
                double t = 0.0;
                while (env > 0.01 && t < 10.0)       // 跌到 -20 dB
                {
                    env += alpha * (0.0 - env);
                    t += dt;
                }
                return t;
            };
            const double expect = 20.0 / SpectrumReleaseDbPerSecond;   // = 111 ms
            const double tTop = fallTime20dB(dtTop);
            const double tBottom = fallTime20dB(dtBottom);
            check(tTop >= expect && tTop <= expect + dtTop + 1e-9,
                  "top-tier envelope falls 20 dB at 180 dB/s");
            check(tBottom >= expect && tBottom <= expect + dtBottom + 1e-9,
                  "bottom-tier envelope falls 20 dB at the same physical rate (16x fix)");
        }
    }

    // ---- Test 26: smoothing pipeline end-to-end (v0.4.1, via offline prototype) ----
    {
        const double fs = 48000.0;
        const int hop = 512, numBands = 121;

        // 峰展宽预算：~200 Hz 段中心满幅正弦（高层，段间独立——最坏情形）。
        // 各档峰值单调不升；1/6 档（r=2）>= -5.5 dB（Hann 核冲激理论精确值
        // 10·log10(1/3) = -4.77 dB：核权重 {0.25,0.75,1,0.75,0.25} 和为 3，
        // 200 Hz 邻段几乎无裙能量，实测即理论值；低频核跨段重叠，损失更小）。
        double peak[4] = {};
        for (int setting = 0; setting <= 3; ++setting)
        {
            const VQTKernelSet set = buildVQTKernelSet(fs, numBands, 1.0, WindowType::BlackmanHarris);
            OfflineVqtAnalyzer a;
            a.prepare(fs, set, hop, setting);
            const int band = OfflineVqtAnalyzer::bandIndexNear(200.0, numBands);
            feedSignal(a, octaveBandCenterFreq(0.5f, band, numBands), 1.0, 2.0, fs);
            peak[setting] = a.bandDb[band];
        }
        check(peak[0] > -0.75 && peak[0] < 0.75, "e2e smoothing: Off keeps tone at ~0 dB");
        check(peak[1] > -5.5 && peak[1] <= peak[0] + 0.05,
              "e2e smoothing: 1/6 oct peak within -5.5 dB and no higher than Off");
        check(peak[2] <= peak[1] + 0.05 && peak[3] <= peak[2] + 0.05,
              "e2e smoothing: peak drop monotonic with smoothing depth");
        // 各档数值预算（±0.75 dB，与 Off 档同口径）：核权重和 r+1（r=1/2/4/8 →
        // 2/3/5/9），冲激峰理论值 10·log10(1/(r+1)) = -3.01 / -4.77 / -6.99 /
        // -9.54 dB（附录 A.3；200 Hz 邻段几乎无裙能量，实测即理论值）。
        check(std::abs(peak[2] - 10.0 * std::log10(1.0 / 5.0)) < 0.75,
              "e2e smoothing: 1/3 oct peak within 0.75 dB of kernel theory (-6.99 dB = 1/5)");
        check(std::abs(peak[3] - 10.0 * std::log10(1.0 / 9.0)) < 0.75,
              "e2e smoothing: 2/3 oct peak within 0.75 dB of kernel theory (-9.54 dB = 1/9)");
        std::printf("      smoothing peak, 200 Hz tone (121 bands): Off %.2f / 1/6 %.2f / 1/3 %.2f / 2/3 %.2f dB\n",
                    peak[0], peak[1], peak[2], peak[3]);

        // 噪声驯服：白噪声下顶层（band 109..120，同层段独立）的段间读数起伏
        // 随 1/6 oct 平滑显著下降。
        auto noiseSpread = [&](int setting)
        {
            const VQTKernelSet set = buildVQTKernelSet(fs, numBands, 1.0, WindowType::BlackmanHarris);
            OfflineVqtAnalyzer a;
            a.prepare(fs, set, hop, setting);
            std::mt19937 rng(12345);
            std::normal_distribution<double> noise(0.0, 0.2);
            for (int i = 0; i < static_cast<int>(4.0 * fs); ++i)
                a.processSample(noise(rng));
            const int lo = 109, hi = 120;
            double mean = 0.0;
            for (int b = lo; b <= hi; ++b) mean += a.bandDb[b];
            mean /= (hi - lo + 1);
            double var = 0.0;
            for (int b = lo; b <= hi; ++b) var += (a.bandDb[b] - mean) * (a.bandDb[b] - mean);
            return std::sqrt(var / (hi - lo + 1));
        };
        const double spreadOff = noiseSpread(0);
        const double spreadOn = noiseSpread(1);
        check(spreadOn < spreadOff * 0.75, "e2e smoothing: 1/6 oct tames inter-band noise spread");
        std::printf("      noise band spread (top tier): Off %.2f dB -> 1/6 oct %.2f dB\n",
                    spreadOff, spreadOn);

        // 时间归一化（端到端）：200 Hz 段关断后 -10 -> -50 dB 的下落。包络期望
        // 40/180 s = 222 ms，叠加顶层帧退出的涂抹（~341 ms 帧滑出）——预算放宽
        // 到 [120, 600] ms：足以抓住包络失效/过慢（v0.4.0 底层式 ~3 s 拖尾）。
        {
            const VQTKernelSet set = buildVQTKernelSet(fs, numBands, 1.0, WindowType::BlackmanHarris);
            OfflineVqtAnalyzer a;
            a.prepare(fs, set, hop, 0);
            const int band = OfflineVqtAnalyzer::bandIndexNear(200.0, numBands);
            feedSignal(a, octaveBandCenterFreq(0.5f, band, numBands), 1.0, 2.0, fs);
            double tCross10 = -1.0, tCross50 = -1.0;
            for (int i = 0; i < static_cast<int>(2.5 * fs); ++i)
            {
                a.processSample(0.0);
                if (tCross10 < 0.0 && a.bandDb[band] < -10.0) tCross10 = i / fs;
                if (tCross50 < 0.0 && a.bandDb[band] < -50.0) { tCross50 = i / fs; break; }
            }
            const double fall = tCross50 - tCross10;
            const double expect = 40.0 / SpectrumReleaseDbPerSecond;
            check(fall > 0.12 && fall < 0.60,
                  "e2e envelope: 40 dB fall lands in [120, 600] ms window");
            std::printf("      fall time -10 -> -50 dB: %.0f ms (envelope expectation %.0f ms + frame smear)\n",
                        fall * 1000.0, expect * 1000.0);
        }

        // 低层段端到端关断（SPC-01）：上面 200 Hz 落在 tier 8（帧 0.341 s），
        // 覆盖不到「低层帧远长于包络时间常数」的区间——那里的下落由帧滑出主导。
        // band 31（≈3.00 Hz，属主层 tier 2，帧 1.365 s）是低层代表：
        //   - 总下落（-10 → -50 dB）≤ windowSeconds + 2·dt（帧滑出是主导项，
        //     该界宽松，只抓多秒级拖尾）；
        //   - 尾段（-40 → -50 dB，此时帧已全零、纯包络穿越）≤ 10/180 + 3·dt——
        //     界由两个物理量相加而成（不是拍出来的常数）：包络以 180 dB/s 匀速
        //     下落，故 -40 → -50 dB 的 10 dB 至少需要 10/180 = 55.6 ms；加上
        //     穿越点的检测量化——读数只在分析步上更新，每次更新可能把两个穿越
        //     各推迟一步，故容 3·dt（48k/hop512 下 tier 2 为 3×42.7 = 128 ms，
        //     合计 183.6 ms）。界内实测 43~85 ms（检测量化 1~2 步），留有一整个
        //     分析步的余量；而「包络系数未按各层真实 dt 折算」（v0.4.0 式回归）
        //     时低层释放慢一个抽取因子倍数，实测尾段 213 ms（检测量化 5 步）被
        //     拒绝——顶层段与单元级断言都抓不到该回归。
        // 界全部由 set.tiers[]、hop/fs 与 SpectrumReleaseDbPerSecond 推导，不硬编码。
        {
            const VQTKernelSet set = buildVQTKernelSet(fs, numBands, 1.0, WindowType::BlackmanHarris);
            const int band = OfflineVqtAnalyzer::bandIndexNear(3.0, numBands);
            const int tier = vqtBandTier(band, octaveDivisorForBands(numBands));
            const VQTTier& T = set.tiers[tier];
            const double dt = static_cast<double>(vqtTierStride(T.decimation, hop))
                            * static_cast<double>(T.decimation) / fs;
            const double totalBound = T.windowSeconds + 2.0 * dt;
            const double tailBound = 10.0 / SpectrumReleaseDbPerSecond + 3.0 * dt;

            OfflineVqtAnalyzer a;
            a.prepare(fs, set, hop, 0);
            feedSignal(a, octaveBandCenterFreq(0.5f, band, numBands), 1.0, 2.0, fs);

            double tCross10 = -1.0, tCross40 = -1.0, tCross50 = -1.0;
            for (int i = 0; i < static_cast<int>(6.0 * fs); ++i)
            {
                a.processSample(0.0);
                if (tCross10 < 0.0 && a.bandDb[band] < -10.0) tCross10 = i / fs;
                if (tCross40 < 0.0 && a.bandDb[band] < -40.0) tCross40 = i / fs;
                if (tCross50 < 0.0 && a.bandDb[band] < -50.0) { tCross50 = i / fs; break; }
            }
            // 三个穿越都必须真的发生：tCrossXX 仍为 -1 哨兵时差值会退化成 0 而
            // 从下方越过 `<= bound`（空过），故显式要求每个哨兵都已被替换。
            const bool allCrossed = (tCross10 >= 0.0 && tCross40 >= 0.0 && tCross50 >= 0.0);
            const double totalFall = tCross50 - tCross10;
            const double tailFall  = tCross50 - tCross40;
            check(allCrossed, "e2e envelope (low tier): -10/-40/-50 dB crossings all observed");
            check(allCrossed && totalFall > 0.0 && totalFall <= totalBound + 1.0e-9,
                  "e2e envelope (low tier): total 40 dB fall within window + 2*dt");
            check(allCrossed && tailFall > 0.0 && tailFall <= tailBound + 1.0e-9,
                  "e2e envelope (low tier): -40 -> -50 dB tail within 10/180 + 3*dt (per-tier dt fix)");
            std::printf("      low-tier fall (band %d, tier %d, %.2f Hz): total %.0f ms <= %.0f ms, tail %.0f ms <= %.0f ms (dt %.1f ms)\n",
                        band, tier, octaveBandCenterFreq(0.5f, band, numBands),
                        totalFall * 1000.0, totalBound * 1000.0,
                        tailFall * 1000.0, tailBound * 1000.0, dt * 1000.0);
        }
    }

    if (failures == 0)
    {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", failures);
    return 1;
}
