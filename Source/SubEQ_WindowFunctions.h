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

// Cosine-sum 系数：w[i] = a0 − a1·cos(2πi/(M−1)) + a2·cos(4πi/(M−1)) − a3·cos(6πi/(M−1))
// 未用的高次项为 0；返回有效项数（Hann 2、Blackman 3、BH-4/Nuttall 4）。
inline int windowCoefficients (WindowType type, double a[kMaxWindowTerms]) noexcept
{
    switch (type)
    {
        case WindowType::Hann:
            a[0] = 0.5;      a[1] = 0.5;      a[2] = 0.0;     a[3] = 0.0;
            return 2;
        case WindowType::Blackman:
            a[0] = 0.42;     a[1] = 0.5;      a[2] = 0.08;    a[3] = 0.0;
            return 3;
        case WindowType::BlackmanHarris:
            a[0] = 0.35875;  a[1] = 0.48829;  a[2] = 0.14128; a[3] = 0.01168;
            return 4;
        case WindowType::Nuttall:
            a[0] = 0.355768; a[1] = 0.487396; a[2] = 0.144232; a[3] = 0.012604;
            return 4;
    }
    a[0] = 0.5; a[1] = 0.5; a[2] = 0.0; a[3] = 0.0;
    return 2;
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
    double a[kMaxWindowTerms];
    windowCoefficients (type, a);
    return a[0];
}

// 标称首旁瓣电平（dB，文献值——描述窗的泄漏抑制能力，测试中实测校验）。
inline double windowSidelobeDb (WindowType type) noexcept
{
    switch (type)
    {
        case WindowType::Hann:           return -31.5;
        case WindowType::Blackman:       return -58.0;
        case WindowType::BlackmanHarris: return -92.0;
        case WindowType::Nuttall:        return -93.3;
    }
    return -31.5;
}

// 等效噪声带宽（DFT bin 数，文献值）：ENBW = M·Σw² / (Σw)²。
// 决定核对宽带噪声的通过量——旁瓣越低、主瓣越宽，ENBW 越大。
inline double windowEnbwBins (WindowType type) noexcept
{
    switch (type)
    {
        case WindowType::Hann:           return 1.50;
        case WindowType::Blackman:       return 1.73;
        case WindowType::BlackmanHarris: return 2.00;
        case WindowType::Nuttall:        return 2.02;
    }
    return 1.50;
}

} // namespace SubEQ
