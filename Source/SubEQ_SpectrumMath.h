/*
  ==============================================================================

    SubEQ_SpectrumMath.h
    Pure octave-spectrum math (header-only, no JUCE dependency):
      - octave band centre frequencies
      - display smoothing (v0.4.1): inter-band power smoothing + power-domain
        time envelope, all shared by the analyser and the offline prototype

    Shared by SubEQ_Spectrum.cpp and the GUI so the band layout has a single
    source of truth and can be regression-tested headlessly.

    （v0.4.0：Hann 窗与 16/N² bin 校准随 FFT 分析器退役——窗函数族移至
    SubEQ_WindowFunctions.h，校准改为 VQT 核内建的相干归一，见
    SubEQ_VQTDesign.h。）

  ==============================================================================
*/

#pragma once

#include <cmath>

namespace SubEQ
{

// Octave divisor: 1/6 octave -> 61 bands, 1/12 octave -> 121 bands.
inline int octaveDivisorForBands(int numBands) noexcept
{
    return (numBands == 121) ? 12 : 6;
}

// Centre frequency of octave band `band` (0-based): fc = minFreq * 2^(band/div).
inline float octaveBandCenterFreq(float minFreq, int band, int numBands) noexcept
{
    const float div = static_cast<float>(octaveDivisorForBands(numBands));
    return minFreq * std::pow(2.0f, static_cast<float>(band) / div);
}

// ===== 显示平滑（v0.4.1）：频域段间平滑 + 时间包络，全部在功率域 =====
//
// 管线：段瞬时功率 → 频域平滑（Hann 权重核，边缘重归一化）→ 时间包络
// （一阶，系数按层真实分析间隔归一化）→ 10·log10 → 钳位。dB 转换只发生
// 在最后一步：dB 域平均对波动功率有系统性低估（log 的凹性 / Jensen
// 不等式：功率 {1, 100} 两段平均，功率域 17.0 dB vs dB 域 10 dB），且
// v0.4.0 的包络在 -60 dB 钳位之后进行，地板以下的功率差异全部塌缩。

// 平滑档 → 核半径（段数）。61 段（1/6 oct 网格）与 121 段（1/12 oct
// 网格）交付近似相同的倍频程平滑宽度（radius = 档位宽度 × 每倍频程段数）：
//   档位            61 段    121 段
//   0 Off           0        0
//   1 (1/6 oct)     1        2
//   2 (1/3 oct)     2        4
//   3 (2/3 oct)     4        8
inline int smoothingRadiusBands(int smoothingIndex, int numBands) noexcept
{
    constexpr int radius61[4]  = { 0, 1, 2, 4 };
    constexpr int radius121[4] = { 0, 2, 4, 8 };
    if (smoothingIndex <= 0)
        return 0;   // Off（含非法负值）
    if (smoothingIndex > 3)
        smoothingIndex = 3;
    return (numBands == 121) ? radius121[smoothingIndex] : radius61[smoothingIndex];
}

// Hann 型核权重：w(j) = cos²(π·j / (2·(r+1)))，j ∈ [-r, r]（中心 1）。
inline double smoothingKernelWeight(int offset, int radius) noexcept
{
    const double x = 3.14159265358979323846 * static_cast<double>(offset)
                   / (2.0 * static_cast<double>(radius + 1));
    const double c = std::cos(x);
    return c * c;
}

// 段功率平滑：对 powers[band-r .. band+r]（截到 [0, numBands)）做功率域
// 加权平均，按实际权重和重归一化（边缘段不衰减）。radius<=0 → 恒等。
// 读到的邻段功率是它们属主层最近一次分析值（增量更新，陈旧度 ≤ 一个
// 邻层步距——与 VQT 逐层触发模型一致）。
template <typename T>
inline T smoothBandPowerAt(const T* powers, int band, int numBands, int radius) noexcept
{
    if (radius <= 0)
        return powers[band];

    int lo = band - radius; if (lo < 0) lo = 0;
    int hi = band + radius; if (hi > numBands - 1) hi = numBands - 1;

    double acc = 0.0, wsum = 0.0;
    for (int j = lo; j <= hi; ++j)
    {
        const double w = smoothingKernelWeight(j - band, radius);
        acc  += w * static_cast<double>(powers[j]);
        wsum += w;
    }
    return static_cast<T>(acc / wsum);
}

// 显示包络物理常数（功率域一阶）。release 为"恒定 dB/s 下落速率"：功率域
// 指数衰减 P ∝ e^(−t/τ) 的 dB 轨迹斜率是常数 10/(ln10·τ)（频谱仪惯例的
// 匀速下落），比 v0.4.0 的 dB 域逼近（先快后慢）观感更均匀。
inline constexpr double SpectrumAttackSeconds      = 0.020;  // 快攻 τ
inline constexpr double SpectrumReleaseDbPerSecond = 180.0;  // 下落速率
inline constexpr double SpectrumReleaseTau         = 10.0 / 2.302585092994045684
                                                      / SpectrumReleaseDbPerSecond;

// 时间归一化的一阶平滑系数：α = 1 − e^(−dt/τ)。
// VQT 各层分析节奏不同（48k/hop512：顶层 ~93.7 Hz、最底层 ~5.9 Hz），包络
// 系数必须按各层真实分析间隔折算——否则同一迭代系数会让底层的物理释放
// 速率比顶层慢一个抽取因子倍数（v0.4.0 实测差 ~16×，底部拖尾）。
// τ<=0 → 1（立即跟随）；dt<=0 → 0。
inline double ballisticAlpha(double dtSeconds, double tauSeconds) noexcept
{
    if (tauSeconds <= 0.0)
        return 1.0;
    if (dtSeconds <= 0.0)
        return 0.0;
    const double a = 1.0 - std::exp(-dtSeconds / tauSeconds);
    return (a > 1.0) ? 1.0 : a;
}

} // namespace SubEQ
