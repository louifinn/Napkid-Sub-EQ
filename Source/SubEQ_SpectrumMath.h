/*
  ==============================================================================

    SubEQ_SpectrumMath.h
    倍频程频谱的纯数学（header-only、无 JUCE 依赖）：
      - 倍频程频段中心频率
      - 频段密度轴（kBandsByDensity = {61, 121} 与段数→密度反查）——全仓段数的
        单一来源，运行时核组合索引、configure 归一与 choice 解码器都查它
      - 显示范围与地板（kDisplayRangeDb / kDisplayFloorDb / kDisplayFloorPower）——
        全仓 60 dB 显示范围与 -60 dB 地板的单一来源，运行时 RangeDb 镜像、GUI
        频谱初值与显示管线钳位都从它派生
      - 显示平滑（v0.4.1）：频域段间功率平滑 + 功率域时间包络，运行时分析器与
        离线原型共用同一份实现（见 vqtUpdateDisplayBands）

    SubEQ_Spectrum.cpp 与 GUI 共用，使频段布局只有一个事实来源，并可无头回归测试。

    （v0.4.0：Hann 窗与 16/N² bin 校准随 FFT 分析器退役——窗函数族移至
    SubEQ_WindowFunctions.h，校准改为 VQT 核内建的相干归一，见
    SubEQ_VQTDesign.h。）

  ==============================================================================
*/

#pragma once

#include <cmath>

#include "SubEQ_WindowFunctions.h"   // kWindowTwoPi（平滑核权重用，勿再硬写 π）

namespace SubEQ
{

// 频段密度轴（顺序 = APVTS spectrum_band_density choice：0=61 段 / 1=121 段）。
// 段数 → 密度轴的反查供运行时装配（核组合索引、configure 归一）与解码器共用，
// 全仓不再各写一份 61/121 条件式。
constexpr int kNumBandDensities = 2;
constexpr int kBandsByDensity[kNumBandDensities] = { 61, 121 };

inline int bandDensityForBands(int numBands) noexcept
{
    return (numBands == kBandsByDensity[1]) ? 1 : 0;
}

// Octave divisor: 1/6 octave -> 61 bands, 1/12 octave -> 121 bands.
inline int octaveDivisorForBands(int numBands) noexcept
{
    return (bandDensityForBands (numBands) == 1) ? 12 : 6;
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
    return (bandDensityForBands (numBands) == 1) ? radius121[smoothingIndex]
                                                 : radius61[smoothingIndex];
}

// Hann 型核权重：w(j) = cos²(π·j / (2·(r+1)))，j ∈ [-r, r]（中心 1）。
inline double smoothingKernelWeight(int offset, int radius) noexcept
{
    const double x = (kWindowTwoPi / 2.0) * static_cast<double>(offset)
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
// kLn10 为 ln10（10·log10→自然对数换算的分子系数，勿再写匿名字面量）。
inline constexpr double kLn10                       = 2.302585092994045684;
inline constexpr double SpectrumAttackSeconds      = 0.020;  // 快攻 τ
inline constexpr double SpectrumReleaseDbPerSecond = 180.0;  // 下落速率
inline constexpr double SpectrumReleaseTau         = 10.0 / kLn10
                                                      / SpectrumReleaseDbPerSecond;

// 显示动态范围与地板（全仓单一来源，R3-F5）：运行时 SpectrumAnalyzer::RangeDb
// （= 本范围的 float 镜像，见 SubEQ_Spectrum.h 的 static_assert）、GUI 频谱
// 缓冲区初值与显示管线的钳位下限全部从这里的 kDisplayRangeDb / kDisplayFloorDb
// 派生——地板与范围互为反号，任一处单独改动都会立刻在同一来源内可见。
// kDisplayPowerGuard 为 log10 的数值守卫功率（自身换算值远低于下限，只防
// log(0)，不参与显示语义）；kDisplayFloorPower 为地板的等效功率（包络状态
// 复位初值），同样由地板派生。运行时与离线原型共用。
inline constexpr double kDisplayRangeDb     = 60.0;
inline constexpr double kDisplayFloorDb     = -kDisplayRangeDb;
inline constexpr double kDisplayPowerGuard  = 1.0e-12;

// 10 的整数次幂（C++17 的 std::pow 不是 constexpr，故手工展开）
constexpr double pow10Int (int exponent) noexcept
{
    double r = 1.0;
    for (int i = 0; i < exponent; ++i)
        r *= 10.0;
    return r;
}

// 地板档数（10 dB 的整数倍计数）：地板与功率展开的唯一换算。断言要求
// -kDisplayFloorDb 恰等于 10·kDisplayFloorSteps（往返相等，非截断后取模）——
// 故 10k+0.x 型分数地板（如 -60.5：截断商为 6，但 6×10 = 60 ≠ 60.5）会被
// 直接拦下，而不是静默用截断后的商去展开功率、让地板与功率初值悄悄失配。
// 改地板而忘了同步功率初值同样在这条断言上暴露（非恒真式）。
inline constexpr int kDisplayFloorSteps = static_cast<int> (-kDisplayFloorDb / 10.0);
static_assert (kDisplayFloorDb <= 0.0
               && -kDisplayFloorDb == 10.0 * static_cast<double> (kDisplayFloorSteps),
               "display floor must be a negative multiple of 10 dB so the "
               "exact power form below holds");
inline constexpr double kDisplayFloorPower = 1.0 / pow10Int (kDisplayFloorSteps);

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

// 一层的显示包络系数折算（运行时分析器与离线原型共用，收口此前的局部镜像）：
// 本层真实分析间隔 dt = 步距 × 抽取因子 / 采样率（48k/hop512 下顶层 ~10.7 ms、
// 最底层 ~170 ms），attack/release 系数按 dt 归一化后传入 vqtUpdateDisplayBands
// ——全频段统一物理速率（理由见 ballisticAlpha 说明）。
struct VQTTierEnvelopeAlphas
{
    double dt = 0.0;            // 本层分析间隔（秒）
    double attackAlpha = 1.0;   // 1 − e^(−dt/τ_attack)
    double releaseAlpha = 1.0;  // 1 − e^(−dt/τ_release)
};

inline VQTTierEnvelopeAlphas vqtTierEnvelopeAlphas (int decimation, int stride,
                                                    double sampleRate) noexcept
{
    VQTTierEnvelopeAlphas out;
    if (sampleRate > 0.0)
        out.dt = static_cast<double> (stride) * decimation / sampleRate;
    out.attackAlpha  = ballisticAlpha (out.dt, SpectrumAttackSeconds);
    out.releaseAlpha = ballisticAlpha (out.dt, SpectrumReleaseTau);
    return out;
}

// ===== 一层频段的显示更新（运行时分析器与离线原型共用同一实现）=====
//
// 内部顺序固定为：本层各段瞬时功率 → smoothBandPowerAt（频域段间平滑）→
// 功率域一阶包络 → 10·log10 → 钳 [kDisplayFloorDb, 0]（dB 转换只发生在最后
// 一步，理由见本文件顶部管线注释）。
//
// 入参：powers / envelopeState 为按全局段下标索引的功率缓冲（前者写瞬时功率、
// 后者为包络状态）；[firstBand, firstBand + numBandsInTier) 为本层属主段区间；
// numBands 为总段数（平滑核的截断边界）；radius 为平滑核半径（段）；
// attack/releaseAlpha 为调用方按本层真实分析间隔折算的包络系数（dt 各层不同，
// 见 vqtTierEnvelopeAlphas / ballisticAlpha）；outDb 为按全局段下标写的 dB 输出
// （仅本层属主段被写）；
// powerAtBand(i) 返回本层第 i 段的瞬时功率（i 为层内下标）。
//
// 只更新本层属主段——邻层段由其属主层按自己的 dt 更新，杜绝跨层双计。
template <typename T, typename PowerAtBandFn>
inline void vqtUpdateDisplayBands (T* powers, T* envelopeState,
                                   int firstBand, int numBandsInTier, int numBands,
                                   int radius, T attackAlpha, T releaseAlpha,
                                   T* outDb, PowerAtBandFn&& powerAtBand) noexcept
{
    // 第一遍：本层各段瞬时功率先整层落盘——邻段平滑读到的是同层的新值。
    // 非有限功率按 0 处理：否则一次 NaN 会永久污染包络并让频段卡在地板。
    for (int i = 0; i < numBandsInTier; ++i)
    {
        const T p = static_cast<T> (powerAtBand (i));
        powers[firstBand + i] = std::isfinite (p) ? p : static_cast<T> (0);
    }

    // 第二遍：频域平滑（功率域）→ 时间包络（功率域一阶）→ dB/钳位/输出
    for (int i = 0; i < numBandsInTier; ++i)
    {
        const int b = firstBand + i;
        const T pSmooth = smoothBandPowerAt (powers, b, numBands, radius);

        T& env = envelopeState[b];
        env += ((pSmooth > env) ? attackAlpha : releaseAlpha) * (pSmooth - env);

        T db = (env > static_cast<T> (kDisplayPowerGuard))
                   ? static_cast<T> (10.0 * std::log10 (static_cast<double> (env)))
                   : static_cast<T> (kDisplayFloorDb);
        if (db < static_cast<T> (kDisplayFloorDb)) db = static_cast<T> (kDisplayFloorDb);
        if (db > static_cast<T> (0.0))             db = static_cast<T> (0.0);

        outDb[b] = db;
    }
}

} // namespace SubEQ
