/*
  ==============================================================================

    SubEQ_VQTDesign.h
    多采样率 VQT（Variable-Q Transform）频谱分析纯数学（header-only、无 JUCE 依赖）：

      - 层结构：0.5 Hz 起 10 个倍频程层，顶层（256~512 Hz）从全率降到 ~3 kHz，
        向下每层再 ÷2（Schörkhuber–Klapuri / librosa 的倍频程分层多采样率方案；
        层内归一化坐标恒定——恒 Q 的尺度不变性）。
      - 帧长（= 核窗长）：分辨率需求 F_res = 分辨率倍率·filterScale·R/段间距，
        其中分辨率倍率 = 窗主瓣零点宽/2（Hann 1、Blackman 1.5、BH-4/Nuttall 2
        ——低旁瓣窗的主瓣代价诚实计入）；底部用 ≥2 s 的 2 的幂（48 kHz 约
        2.73 s）。下限生效的层（约 4~16 Hz 以下，随窗型/档位）等效带宽加宽
        ——VQT 的 gamma 行为。
      - 帧加窗 + 满帧匹配核：帧乘选定窗型（长度 = 帧长）后 FFT；每频段核 =
        同窗复指数的频谱，taps = conj(K)/Σ|K|²（分数 bin 匹配估计）。满幅正弦
        落段中心 → |r| = 1 → 功率 0 dB（与旧 FFT 分析器幅值约定一致）。帧加窗
        是支撑截断正确性的前提：未加窗帧的矩形谱裙（−13 dB）会淹没低旁瓣窗
        的泄漏抑制（v0.4.0 原型实测教训，见调研报告）。
      - 辅助：↓2 抗混叠 12 阶 Butterworth 系数、0.05 Hz 直流阻断极点（底部
        核主瓣覆盖直流，必须有 DC 阻断）。

    SubEQ_Spectrum.cpp（运行时分析器）与 Tests/subeq_fft_test.cpp（离线
    原型）共用——被测代码即交付代码。

  ==============================================================================
*/

#pragma once

#include <cmath>
#include <complex>
#include <vector>

#include "SubEQ_DSPMath.h"
#include "SubEQ_WindowFunctions.h"
#include "SubEQ_SpectrumMath.h"
#include "SubEQ_BiquadDesign.h"

namespace SubEQ
{

//==============================================================================
// 常量
//==============================================================================

constexpr int    VQTNumTiers        = 10;     // 0.5 Hz 起 10 层倍频程 → 顶层 256~512 Hz
constexpr double VQTTierCapSeconds  = 2.0;    // 底部核时域长度下限（≥2 s 的 2 的幂）
constexpr double VQTTopRateTargetHz = 3072.0; // 顶层目标采样率（~6× 过采样于 512 Hz）
constexpr int    VQTMinTierFftSize  = 8;      // 极小帧防御下限
constexpr double VQTDcBlockerHz     = 0.05;   // 直流阻断转折（0.5 Hz 处 −0.04 dB）
constexpr double VQTSupportFloorDb  = -65.0;  // 频域支撑截断门限（相对峰值）

// 顶层降采样指数上限（见 vqtTopDecimationExponent 的钳位）。抽取链级数
// numStages = 该上限 + VQTNumTiers − 1（每层向下再 ÷2），音频线程的
// stages[] 数组界必须覆盖它——运行时与离线原型共用该表达式（STD-11）。
constexpr int    VQTMaxTopDecimationExponent = 16;

//==============================================================================
// 层结构与帧长
//==============================================================================

// 分辨率档（原 spectrum_fft_size choice 下标 0/1/2 → Fast/Balanced/Fine）：
// 滤波器尺度，直接乘进帧长需求（librosa filter_scale 语义）。
inline double vqtFilterScale (int resolutionChoice) noexcept
{
    return resolutionChoice == 0 ? 0.7 : (resolutionChoice == 2 ? 1.5 : 1.0);
}

// 恒 Q 有效品质因数：Q_eff = 1/(2^(1/div) − 1)。1/6 倍频程 ≈ 8.40，1/12 ≈ 16.82。
// 相邻段间距 = f/Q_eff。
inline double vqtQeff (int octaveDivisor) noexcept
{
    return 1.0 / (std::pow (2.0, 1.0 / static_cast<double> (octaveDivisor)) - 1.0);
}

// 窗的分辨率倍率 = 主瓣零点宽（bin）/2：Hann 1、Blackman 1.5、BH-4/Nuttall 2。
// 帧长需求 = 倍率 × R/段间距——低旁瓣窗的主瓣加宽诚实计入满分辨成本。
// switch 显式覆盖四种窗型（新增窗型时编译器可提示遗漏），switch 后给兜底值。
inline double vqtWindowResolutionMultiplier (WindowType type) noexcept
{
    switch (type)
    {
        case WindowType::Hann:           return 1.0;
        case WindowType::Blackman:       return 1.5;
        case WindowType::BlackmanHarris: return 2.0;
        case WindowType::Nuttall:        return 2.0;
    }
    return 2.0;   // 非法 WindowType：按最宽主瓣处理（保守帧长）
}

// 真实的主瓣零点半宽与镜像项不再用于运行时保护：低频频段的镜像支撑是
// 0.5 Hz 帧内不足两个周期的物理限制（见 Documentation/SubEQ_VQT_Feasibility.md
// §8）。这里不声称做了镜像抑制；支撑仍取连续区间 [lo,hi]。

// 顶层降采样指数 d：R9 = fs/2^d 落在 ~3 kHz 附近（round），且 fs 必须被 2^d
// 整除（44.1 kHz → d=2 即 11025 Hz，48k → 16 倍 = 3000 Hz，96k → 32 倍）。
inline int vqtTopDecimationExponent (double sampleRateHz) noexcept
{
    if (sampleRateHz <= 0.0)
        return 4;

    int d = static_cast<int> (std::lround (std::log2 (sampleRateHz / VQTTopRateTargetHz)));
    if (d < 1) d = 1;
    if (d > VQTMaxTopDecimationExponent) d = VQTMaxTopDecimationExponent;
    while (d > 1 && std::fmod (sampleRateHz, std::ldexp (1.0, d)) != 0.0)
        --d;
    return d;
}

// 频段 → 层：段 n 的中心 0.5·2^(n/div) 落在层 o = floor(n/div)（顶层多收一段）。
inline int vqtBandTier (int band, int octaveDivisor) noexcept
{
    const int t = band / octaveDivisor;
    return t >= VQTNumTiers ? VQTNumTiers - 1 : t;
}

// 一层的静态布局（采样率、抽取因子、帧长、频段子区间、帧窗表）。
struct VQTTier
{
    double rateHz = 0.0;         // 层采样率 R_o = fs / decimation
    int    decimation = 1;       // 相对全率的抽取因子（2 的幂）
    int    fftSize = VQTMinTierFftSize; // 帧长 F_o（= 核窗长）
    int    firstBand = 0;        // 层内第一个频段下标
    int    numBandsInTier = 6;   // 层内频段数（顶层多收 512 Hz 一段）
    double windowSeconds = 0.0;  // F_o / R_o（诊断/测试：层分析窗时长）
};

inline VQTTier vqtTierLayout (int tier, double sampleRateHz, int numBands,
                              double filterScale, WindowType win) noexcept
{
    const int div = octaveDivisorForBands (numBands);
    const int d9 = vqtTopDecimationExponent (sampleRateHz);
    const double qEff = vqtQeff (div);

    VQTTier t;
    t.decimation = 1 << (d9 + (VQTNumTiers - 1 - tier));
    t.rateHz = sampleRateHz / static_cast<double> (t.decimation);
    t.firstBand = tier * div;
    t.numBandsInTier = (tier == VQTNumTiers - 1) ? div + 1 : div;

    // 分辨率需求：层底段（最宽段间距处）主瓣零点对齐段间距
    const double lowestBandHz = octaveBandCenterFreq (0.5f, t.firstBand, numBands);
    const double spacingHz = lowestBandHz / qEff;
    const double resMult = vqtWindowResolutionMultiplier (win) * filterScale;
    const int fRes = nextPowerOfTwo (static_cast<int> (std::ceil (resMult * t.rateHz / spacingHz)) + 1);

    // 至少 2 s 的帧长：向上取到 2 的幂，避免“标称 2 s、实际 1.0–1.5 s”的
    // 底部层 gamma 区量化损失。极低采样率下可能略超 2 s（例如 4 kHz 为
    // 2.048 s），这是分辨率优先的显式取舍。
    const double capExact = t.rateHz * VQTTierCapSeconds;
    const int capPow2 = nextPowerOfTwo (std::max (1, static_cast<int> (std::ceil (capExact))));
    const int fCap = std::max (VQTMinTierFftSize, capPow2);

    t.fftSize = std::max (VQTMinTierFftSize, std::min (fRes, fCap));
    t.windowSeconds = static_cast<double> (t.fftSize) / t.rateHz;
    return t;
}

// 层分析步距（层样本数）：与顶层 hop 实时锁步（每 hop 个全率样本推进一次
// 分析）；底层 D_o 大于 hop 时一个新样本即触发，钳到 ≥1。
inline int vqtTierStride (int tierDecimation, int hopSamples) noexcept
{
    const double s = static_cast<double> (hopSamples) / static_cast<double> (tierDecimation);
    return (s <= 1.0) ? 1 : static_cast<int> (std::lround (s));
}

//==============================================================================
// 频域核（满帧窗 + 分数 bin 匹配估计）
//==============================================================================

// 单频段核：ψ[n] = w[n]·e^{−j2πf n/R}（n ∈ [0,F)，满帧长）经 F 点 FFT 后，
// 在频域支撑 [loBin, hiBin] 上存 taps[m] = conj(K[m])·2/S，S = Σ支撑|K|²。
// 运行时帧同样乘 w 后 FFT 得 X，响应 r = Σ X[m]·taps[m]：落段中心的满幅
// 正弦 → |r| = 1（功率 0 dB，与旧分析器幅值约定一致；×2 因子补回实信号
// 正弦镜像损失的能量一半）；对偏离频率的抑制由帧窗谱裙决定（所选窗型
// 的旁瓣特性）。
struct VQTKernel
{
    int fftSize = 0;
    int loBin = 0;
    int hiBin = 0;
    std::vector<std::complex<double>> taps;  // hi−lo+1 个 conj(K[m])/S
};

inline VQTKernel buildVQTKernel (double freqHz, double tierRateHz, int fftSize, WindowType win)
{
    VQTKernel k;
    k.fftSize = fftSize;

    std::vector<std::complex<double>> psi (static_cast<size_t> (fftSize));
    const double w0 = kWindowTwoPi * freqHz / tierRateHz;
    for (int n = 0; n < fftSize; ++n)
    {
        const double w = windowValue (win, n, fftSize);
        psi[static_cast<size_t> (n)] = { w * std::cos (w0 * n), -w * std::sin (w0 * n) };
    }
    fftInPlace (psi.data(), fftSize);

    // 频域支撑：|K| 高于门限（相对峰值）的连续区间（自峰向两侧扩展）+1 bin
    int pk = 0;
    double peak = -1.0;
    for (int m = 0; m < fftSize; ++m)
    {
        const double a = std::abs (psi[static_cast<size_t> (m)]);
        if (a > peak) { peak = a; pk = m; }
    }
    const double floorLevel = peak * std::pow (10.0, VQTSupportFloorDb / 20.0);
    int lo = pk, hi = pk;
    while (lo > 0           && std::abs (psi[static_cast<size_t> (lo - 1)]) > floorLevel) --lo;
    while (hi < fftSize - 1 && std::abs (psi[static_cast<size_t> (hi + 1)]) > floorLevel) ++hi;
    lo = std::max (0, lo - 1);
    hi = std::min (fftSize - 1, hi + 1);

    double s = 0.0;
    for (int m = lo; m <= hi; ++m)
        s += std::norm (psi[static_cast<size_t> (m)]);

    k.loBin = lo;
    k.hiBin = hi;
    k.taps.resize (static_cast<size_t> (hi - lo + 1));
    const double invS = 2.0 / s;   // ×2：实信号正弦的匹配估计 |r| = A/2 → 归一回 A
    for (int m = lo; m <= hi; ++m)
        k.taps[static_cast<size_t> (m - lo)] = std::conj (psi[static_cast<size_t> (m)]) * invS;

    return k;
}

// 一帧层谱上计算频段响应幅值平方（功率）。满幅正弦落段中心 → 1.0。
inline double vqtBandPower (const std::complex<double>* frameSpectrum,
                            const VQTKernel& k) noexcept
{
    double re = 0.0, im = 0.0;
    const std::complex<double>* t = k.taps.data();
    for (int m = k.loBin; m <= k.hiBin; ++m)
    {
        const std::complex<double>& x = frameSpectrum[m];
        const std::complex<double>& w = t[m - k.loBin];
        re += x.real() * w.real() - x.imag() * w.imag();
        im += x.real() * w.imag() + x.imag() * w.real();
    }
    return re * re + im * im;
}

// 从层环形缓冲（容量 = fftSize，writeIndex 指向下一个写入位置 = 最老样本）
// 组装最近 F 个样本的加窗帧谱：frame = ring × w_F，再 FFT。帧窗与核窗同型
// 同长——这是支撑截断正确性的前提（未加窗帧的矩形谱裙会淹没低旁瓣窗）。
inline void vqtTierFrameSpectrum (const double* ring, int writeIndex,
                                  const double* windowTable,
                                  std::complex<double>* frameX, int fftSize) noexcept
{
    for (int i = 0; i < fftSize; ++i)
    {
        const double s = ring[(writeIndex + i) % fftSize] * windowTable[i];
        frameX[i] = { s, 0.0 };
    }
    fftInPlace (frameX, fftSize);
}

// 一整套（窗型 × 密度 × 分辨率）核：层布局 + 帧窗表 + 每层每频段核。
// 仅在 prepare/setup 路径构建（内部分配 vector），音频线程只读。
struct VQTKernelSet
{
    VQTTier tiers[VQTNumTiers];
    std::vector<double> frameWindow[VQTNumTiers];        // 长度 = F_o 的窗表
    std::vector<VQTKernel> kernels[VQTNumTiers];         // [tier][bandInTier]
};

inline VQTKernelSet buildVQTKernelSet (double sampleRateHz, int numBands,
                                       double filterScale, WindowType win)
{
    VQTKernelSet set;
    for (int t = 0; t < VQTNumTiers; ++t)
    {
        set.tiers[t] = vqtTierLayout (t, sampleRateHz, numBands, filterScale, win);
        const int F = set.tiers[t].fftSize;

        set.frameWindow[t].resize (static_cast<size_t> (F));
        for (int n = 0; n < F; ++n)
            set.frameWindow[t][static_cast<size_t> (n)] = windowValue (win, n, F);

        set.kernels[t].resize (static_cast<size_t> (set.tiers[t].numBandsInTier));
        for (int i = 0; i < set.tiers[t].numBandsInTier; ++i)
        {
            const double f = octaveBandCenterFreq (0.5f, set.tiers[t].firstBand + i, numBands);
            set.kernels[t][static_cast<size_t> (i)] =
                buildVQTKernel (f, set.tiers[t].rateHz, F, win);
        }
    }
    return set;
}

//==============================================================================
// 抽取链辅助
//==============================================================================

// 一级 ↓2 抗混叠低通：12 阶 Butterworth（6 个二阶节级联），截止 0.28×输出率。
// 折叠带（≥0.5×输出率）压制 ≥60 dB（0.55×输出率处 ≥70 dB）；频段内容最高
// ~0.17×层率（顶层 512 Hz @3000 Hz），通带衰减可忽略。
// 注意：更强抑制的代价是低频显示群延迟增大（0.5 Hz 附近整条链约 1.4 s），
// 这是分析显示延迟，不影响音频处理路径。
inline constexpr int kVqtDecimatorBiquads = 6;

inline void vqtDecimatorCoefficients (double stageInputRateHz,
                                      BiquadCoefficients out[kVqtDecimatorBiquads]) noexcept
{
    const double cutoffHz = 0.28 * stageInputRateHz * 0.5;

    // 12th-order Butterworth pole Qs: Q_k = 1 / (2 cos((2k+1)π / 24)).
    for (int k = 0; k < kVqtDecimatorBiquads; ++k)
    {
        const double angle = 3.14159265358979323846 * (2.0 * k + 1.0) / 24.0;
        const double q = 1.0 / (2.0 * std::cos(angle));
        BiquadCoefficients tmp[2];
        computeBiquadCoefficients (cutoffHz, 0.0, q, FilterType::LowPass,
                                   stageInputRateHz, tmp);
        out[k] = tmp[0];
    }
}

// 抽取链的一级状态：两个二阶节 + ↓2 相位计数。运行时分析器与离线原型共用
// 同一布局（coeffs 在 prepare 路径写、state/phase 在音频线程写）。
struct VQTDecimationStage
{
    BiquadCoefficients coeffs[kVqtDecimatorBiquads];
    BiquadState state[kVqtDecimatorBiquads];
    int phase = 0;
};

// 单个样本推进整条抽取链（运行时 SpectrumAnalyzer::process 与离线原型
// OfflineVqtAnalyzer::processSample 共用同一实现——抽取级联决定进入分析的
// 样本序列与速率，属数值路径，不得各写一份）：
//   0.05 Hz 一阶直流阻断 → 逐级 ↓2 抗混叠（每级两个二阶节，每 2 个输入向下一级
//   发射 1 个样本）。层 o 的抽取因子 D_o 对应 stage 下标 k = numStages-1-o，
//   即只有 k ≥ numStages-VQTNumTiers 的级输出才进入某层（层号 = numStages-1-k）。
// pushTierSample(tier, sample) 由调用方提供（层环形缓冲装配）。
template <typename PushTierSampleFn>
inline void vqtDecimationStep (double input, VQTDecimationStage* stages, int numStages,
                               double dcPole, double& dcState,
                               PushTierSampleFn&& pushTierSample) noexcept
{
    // 0.05 Hz 一阶直流阻断
    dcState = dcPole * dcState + (1.0 - dcPole) * input;
    double x = input - dcState;

    const int firstTierStage = numStages - VQTNumTiers;
    for (int k = 0; k < numStages; ++k)
    {
        VQTDecimationStage& st = stages[k];
        double y = x;
        for (int b = 0; b < kVqtDecimatorBiquads; ++b)
            y = st.state[b].process (y, st.coeffs[b]);
        if (++st.phase >= 2)
        {
            st.phase = 0;
            if (k >= firstTierStage)
                pushTierSample (numStages - 1 - k, y);
            x = y;
        }
        else
        {
            break;   // 本样本未到达更深层
        }
    }
}

// 层环形缓冲的样本装配 + 步距触发（运行时 SpectrumAnalyzer::pushTierSample 与
// 离线原型 OfflineVqtAnalyzer::pushTierSample 共用同一实现）：写入环形缓冲，
// 累计到 vqtTierStride(decimation, hop) 个层样本时触发一次 analyzeTier()。
// stride 由调用方按本层抽取因子与顶层步距算好传入（两侧 hop 来源不同：运行时
// 读原子量 hopSize，离线原型读成员 hop）。
template <typename AnalyzeTierFn>
inline void vqtPushTierSample (double* ring, int fftSize, double sample,
                               int& write, int& sinceAnalysis, int stride,
                               AnalyzeTierFn&& analyzeTier) noexcept
{
    ring[static_cast<size_t> (write)] = sample;
    if (++write >= fftSize)
        write = 0;

    if (++sinceAnalysis >= stride)
    {
        sinceAnalysis = 0;
        analyzeTier();
    }
}

// 0.05 Hz 一阶直流阻断极点：y = x − s，s ← pole·s + (1−pole)·x。
inline double vqtDcBlockerPole (double sampleRateHz) noexcept
{
    return std::exp (-kWindowTwoPi * VQTDcBlockerHz / sampleRateHz);
}

} // namespace SubEQ
