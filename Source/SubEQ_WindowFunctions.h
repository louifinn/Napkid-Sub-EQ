/*
  ==============================================================================

    SubEQ_WindowFunctions.h
    频谱分析窗函数族纯数学（header-only、无 JUCE 依赖）：
      - 4 种可选窗：Hann / Blackman / Blackman–Harris(4 项) / Nuttall(4 项)
      - 采样值、解析均值（相干增益）、标称旁瓣 / ENBW（文献值，供文档与容差）

    VQT 频段核（SubEQ_VQTDesign.h）与回归测试共用——被测代码即交付代码。

    选型依据（完整对比见 Documentation/SubEQ_VQT_Feasibility.md 窗函数专章）：
    频谱显示动态范围为 −60 dB，Hann 的 −31.5 dB 首旁瓣会把强单音"涂"进
    远端频段（超低频段尤其致命：50 Hz 踢鼓可点亮 0.5~8 Hz 整段）；BH-4 /
    Nuttall 的 −92/−93 dB 旁瓣把泄漏压到显示底以下，代价是主瓣加宽约一倍
    （同等核长下分辨力减半，满分辨交叉频率上移）。取舍交给用户：窗型与
    核长（分辨率档）均为显示设置。

    系数来源：G. Heinzel et al., "Spectrum and spectral density estimation
    by the DFT, including a comprehensive list of window functions and some
    new flat-top windows", 2002, Table 1。

  ==============================================================================
*/

#pragma once

#include <array>
#include <cmath>

namespace SubEQ
{

// 显示设置可选窗型（choice 下标顺序：与 APVTS spectrum_window 一致）
enum class WindowType
{
    Hann = 0,
    Blackman = 1,
    BlackmanHarris = 2,   // 4 项 min Blackman–Harris
    Nuttall = 3           // 4 项 continuous Nuttall
};

constexpr int kNumWindowTypes  = 4;
constexpr int kMaxWindowTerms  = 4;
constexpr double kWindowTwoPi  = 6.2831853071795864769;

// 窗规格：cosine-sum 系数 + 有效项数 + 标称首旁瓣 + 等效噪声带宽。
// 全部窗属性的单一事实来源——新增窗型只需在 kWindowSpecs 加一行。
struct WindowSpec
{
    std::array<double, 4> a;   // a0..a3（未用的高次项为 0）
    int    terms;              // 有效项数（Hann 2、Blackman 3、BH-4/Nuttall 4）
    double sidelobeDb;         // 标称首旁瓣电平（dB，文献值）
    double enbwBins;           // 等效噪声带宽（DFT bin，文献值）
};

// 按 WindowType 下标索引（顺序与枚举、与 APVTS spectrum_window choice 一致）
inline constexpr WindowSpec kWindowSpecs[kNumWindowTypes] =
{
    { { 0.5, 0.5, 0.0, 0.0 },                     2, -31.5, 1.50 },   // Hann
    { { 0.42, 0.5, 0.08, 0.0 },                   3, -58.0, 1.73 },   // Blackman
    { { 0.35875, 0.48829, 0.14128, 0.01168 },     4, -92.0, 2.00 },   // BH-4
    { { 0.355768, 0.487396, 0.144232, 0.012604 }, 4, -93.3, 2.02 }    // Nuttall
};

// 规格查询（非法 WindowType 回落 Hann，与旧 switch 的 default 分支取值一致）
inline const WindowSpec& windowSpec (WindowType type) noexcept
{
    const int i = static_cast<int> (type);
    return kWindowSpecs[(i >= 0 && i < kNumWindowTypes) ? i : 0];
}

// Cosine-sum 系数：w[i] = a0 − a1·cos(2πi/(M−1)) + a2·cos(4πi/(M−1)) − a3·cos(6πi/(M−1))
// 未用的高次项为 0；返回有效项数（查 kWindowSpecs）。
inline int windowCoefficients (WindowType type, double a[kMaxWindowTerms]) noexcept
{
    const WindowSpec& s = windowSpec (type);
    for (int i = 0; i < kMaxWindowTerms; ++i)
        a[i] = s.a[static_cast<size_t> (i)];
    return s.terms;
}

// 窗样本值（对称窗，i ∈ [0, size)）。
inline double windowValue (WindowType type, int i, int size) noexcept
{
    double a[kMaxWindowTerms];
    windowCoefficients (type, a);

    const double x = kWindowTwoPi * static_cast<double> (i)
                   / static_cast<double> (size - 1);
    return a[0]
         - a[1] * std::cos (x)
         + a[2] * std::cos (2.0 * x)
         - a[3] * std::cos (3.0 * x);
}

// 解析均值（= a0，长窗极限下的相干增益 CG = Σw/M）。
inline double windowMean (WindowType type) noexcept
{
    return windowSpec (type).a[0];
}

// 标称首旁瓣电平（dB，文献值——描述窗的泄漏抑制能力，测试中实测校验）。
inline double windowSidelobeDb (WindowType type) noexcept
{
    return windowSpec (type).sidelobeDb;
}

// 等效噪声带宽（DFT bin 数，文献值）：ENBW = M·Σw² / (Σw)²。
// 决定核对宽带噪声的通过量——旁瓣越低、主瓣越宽，ENBW 越大。
inline double windowEnbwBins (WindowType type) noexcept
{
    return windowSpec (type).enbwBins;
}

} // namespace SubEQ
