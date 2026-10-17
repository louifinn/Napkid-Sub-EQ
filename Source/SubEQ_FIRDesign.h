/*
  ==============================================================================

    SubEQ_FIRDesign.h
    FIR coefficient design algorithms (Linear Phase + Minimum Phase), as pure
    template functions with no JUCE dependency.

    Both routines take a magnitude-response callback `magnitude(w)` returning
    the linear (non-dB) magnitude at normalised frequency w in [0, pi], plus
    the desired FIR length. They return the time-domain coefficients and the
    FIR bulk latency (samples) that the caller should report for PDC.

    Shared by SubEQ_FFTProcessor.cpp (background design thread) and the
    standalone regression tests so the tested code is exactly the shipped code.

  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

#include "SubEQ_DSPMath.h"

namespace SubEQ
{

constexpr double kPi = 3.14159265358979323846;

// 最小相位设计网格上出现「深零点」的判定门限（线性幅度，-120 dB）：
// 无标注目标（调用方未给出解析零点阶数）在 DC / Nyquist 的传输零点被
// epsilon 钳位后会造成倒谱时域混叠，故低于该门限时改用旧 8× 过采样网格。
constexpr double kMinPhaseDeepNullThreshold = 1.0e-6;

// 旧自动路径（0.4.4 行为）的深零点设计网格过采样倍率：设计网格取
// firLength × 该倍率，倒谱因果化 / DFT / exp / IDFT 全部在过采样网格上
// 完成后截断回 firLength。它只抑制零点尖峰的混叠，不产生精确零点。
constexpr int kMinPhaseLegacyOversampling = 8;

// 解析剥离路径的候选设计网格过采样倍率：对同一带零点目标分别生成
// os=1 与 os=4 两个候选，再用内部拟合误差选优。os=1 对高 Q / 窄特征
// 更精确（网格与输出长度一致、无额外截断），os=4 对高采样率下欠分辨
// 的拐点（低 fc 相对 fs）更稳健，二者各有用武之地。
constexpr int kMinPhaseFactoredOversampling = 4;

// 候选选优的拟合误差采样点数：在 w ∈ [pi/65536, 0.999*pi] 上对数间隔
// 取点，直接 DTFT 与目标 dB 幅频比较（不插值、不加窗）。
constexpr int kMinPhaseFitPoints = 192;

// Linear Phase FIR (Type-II even-length): sample the magnitude over [0, pi]
// with a linear phase term so the time-domain impulse is symmetric.
// Latency = (firLength - 1) / 2.
template <typename MagnitudeFn>
inline std::vector<double> designLinearPhaseFIR(MagnitudeFn magnitude, int firLength, int& latencyOut)
{
    std::vector<std::complex<double>> spectrum(firLength);
    const double delayPhaseFactor = -kPi * (firLength - 1) / firLength;

    // Step 1: magnitude response with linear phase term for strict symmetry.
    // For even-length Type-II FIR, the Nyquist response (i = N/2) must be 0.
    for (int i = 0; i < firLength / 2; ++i)
    {
        const double w = kPi * i / (firLength / 2);
        const double mag = magnitude(w);
        const double phase = delayPhaseFactor * i;
        spectrum[i] = { mag * std::cos(phase), mag * std::sin(phase) };
    }
    spectrum[firLength / 2] = { 0.0, 0.0 };   // Type-II FIR: H(pi) = 0

    // Conjugate symmetry for the second half
    for (int i = firLength / 2 + 1; i < firLength; ++i)
        spectrum[i] = std::conj(spectrum[firLength - i]);

    // Step 2: IDFT -> time-domain coefficients
    ifftInPlace(spectrum.data(), firLength);

    std::vector<double> coeffsOut(firLength);   // double 抽头：与 DSP 精度约定一致
    for (int i = 0; i < firLength; ++i)
        coeffsOut[i] = spectrum[i].real();

    latencyOut = (firLength - 1) / 2;
    return coeffsOut;
}

namespace detail
{

// 解析剥离 DC/Nyquist 零点后的单候选设计。
//
// 目标幅频 |H(w)| 已知含 (1-z^-1)^dc (1+z^-1)^nyq 的结构零点，先把因子
// 幅频 |1-e^-jw|^dc |1+e^-jw|^nyq 从目标中除掉，得到不含奇点的剩余幅频
// R(w)；对 log R(w) 做最小相位倒谱设计得到 M=os*firLength 点 h_rest，
// 再取前 L=firLength-(dc+nyq) 个抽头与长度 dc+nyq+1 的因子做线性卷积。
// L + (dc+nyq+1) - 1 = firLength，乘积天然等于输出长度，不截断。
template <typename MagnitudeFn>
inline std::vector<double> designFactoredMinimumPhase(MagnitudeFn& magnitude, int firLength,
                                                      int dcZeroOrder, int nyquistZeroOrder,
                                                      int oversampling)
{
    const int designLength = firLength * oversampling;
    const int half = designLength / 2;
    const int zeroOrder = dcZeroOrder + nyquistZeroOrder;
    const int restLength = firLength - zeroOrder;

    // 单缓冲：M 点复谱就地完成倒谱六步；logRest 只保留半个谱 + DC 项，
    // 不额外分配第二份 M 缓冲。
    std::vector<std::complex<double>> spectrum(static_cast<size_t>(designLength));
    std::vector<double> logRest(static_cast<size_t>(half + 1));

    for (int i = 0; i <= half; ++i)
    {
        const double w = kPi * i / half;
        const double mag = magnitude(w);
        // 解析剥离因子幅频：|1-e^-jw| = 2 sin(w/2)，|1+e^-jw| = 2 cos(w/2)。
        // 端点处 sin/cos 为 0，+1e-300 只用于避免 log(0) 产生 -inf；
        // 这两个端点样本随后会被有限延拓覆盖，不会进入设计。
        logRest[i] = std::log(mag + 1.0e-12)
                   - static_cast<double>(dcZeroOrder) * std::log(2.0 * std::sin(0.5 * w) + 1.0e-300)
                   - static_cast<double>(nyquistZeroOrder) * std::log(2.0 * std::cos(0.5 * w) + 1.0e-300);
    }

    // 精确零点处的 logRest 数学上有限（分子与因子同阶相消），但采样点恰
    // 落在 w=0 / w=pi 时直接计算会得到垃圾值：用相邻两点的线性外推取
    // 极限 logRest(0) = 2*logRest(1) - logRest(2)（剩余幅频在零点邻域平滑）。
    // 这一步是解析剥离取代 epsilon 钳位、避免 -240 dB 尖峰的关键。
    if (dcZeroOrder > 0 && half >= 2)
        logRest[0] = 2.0 * logRest[1] - logRest[2];
    if (nyquistZeroOrder > 0 && half >= 2)
        logRest[half] = 2.0 * logRest[half - 1] - logRest[half - 2];

    // 剩余幅频的绝对增益可能很大（低 fc 高通时 ~1/fc^2）：倒谱前先整体
    // 归一化到 O(1)，避免 exp / 卷积在巨大动态范围上做减法；该缩放因子
    // 在输出端精确乘回，不改变设计结果。
    const double logOffset = logRest[0];
    for (int i = 0; i <= half; ++i)
        logRest[i] -= logOffset;

    // 实偶对称 log 幅度谱（i=0 即 DFT 的均值项，i=M/2 为 Nyquist）
    for (int i = 0; i <= half; ++i)
        spectrum[i] = { logRest[i], 0.0 };
    for (int i = 1; i < half; ++i)
        spectrum[designLength - i] = { logRest[i], 0.0 };

    // Step: IDFT -> 实倒谱
    ifftInPlace(spectrum.data(), designLength);

    // Step: 因果化倒谱（折叠窗：c[0] 取 1、c[M/2] 取 1、正半轴其余 ×2、
    // 负半轴置 0；c[M/2] 必须保留，否则 Nyquist 幅频偏离目标）
    spectrum[0] = { spectrum[0].real(), 0.0 };
    for (int i = 1; i < half; ++i)
        spectrum[i] = { spectrum[i].real() * 2.0, 0.0 };
    spectrum[half] = { spectrum[half].real(), 0.0 };
    for (int i = half + 1; i < designLength; ++i)
        spectrum[i] = { 0.0, 0.0 };

    // Step: DFT -> 最小相位 log 复谱
    fftInPlace(spectrum.data(), designLength);

    // Step: exp() -> 最小相位复谱
    for (int i = 0; i < designLength; ++i)
    {
        const double re = spectrum[i].real();
        const double im = spectrum[i].imag();
        spectrum[i] = { std::exp(re) * std::cos(im), std::exp(re) * std::sin(im) };
    }

    // Step: IDFT -> h_rest（前 restLength 个抽头即所需）
    ifftInPlace(spectrum.data(), designLength);

    // 因子 (1-z^-1)^dc (1+z^-1)^nyq 的二项式系数（长度 dc+nyq+1）。
    // 逐次乘一阶因子：先用 (1-z^-1) 迭代 dc 次，再用 (1+z^-1) 迭代 nyq 次。
    const int factorLength = zeroOrder + 1;
    std::vector<double> factor(static_cast<size_t>(factorLength), 0.0);
    factor[0] = 1.0;
    int factorUsed = 1;
    for (int k = 0; k < dcZeroOrder; ++k)
    {
        for (int i = factorUsed; i >= 1; --i)
            factor[i] -= factor[i - 1];
        ++factorUsed;
    }
    for (int k = 0; k < nyquistZeroOrder; ++k)
    {
        for (int i = factorUsed; i >= 1; --i)
            factor[i] += factor[i - 1];
        ++factorUsed;
    }

    // ★ 关键：取 h_rest 前 restLength 个抽头，再与因子做线性卷积。
    // 输出长度 = restLength + (dc+nyq+1) - 1 = firLength，恰好填满且不
    // 截断乘积。若先卷积出长度 firLength+dc+nyq 的完整乘积再截回
    // firLength，DC/Nyquist 的相消会因丢弃尾部而失效——实测 96 kHz
    // HP30 在 0.5 Hz 漏 28 dB。
    std::vector<double> coeffsOut(static_cast<size_t>(firLength), 0.0);
    const double outputScale = std::exp(logOffset);
    for (int n = 0; n < firLength; ++n)
    {
        const int firstTap = std::max(0, n - (restLength - 1));
        const int lastTap = std::min(factorLength - 1, n);
        double sum = 0.0;
        for (int j = firstTap; j <= lastTap; ++j)
            sum += spectrum[n - j].real() * factor[j];
        coeffsOut[n] = sum * outputScale;
    }

    return coeffsOut;
}

// 候选拟合误差：在 kMinPhaseFitPoints 个对数间隔角频率上直接 DTFT，
// 取 FIR 与目标 dB 幅频差的最大绝对值。用相位递推（复步长连乘）代替
// 每个抽头的 cos/sin，误差仅 ~1e-14 rad/65536 抽头，远低于 dB 判据。
template <typename MagnitudeFn>
inline double factoredFitError(const std::vector<double>& coeffs, MagnitudeFn& magnitude)
{
    const double start = kPi / 65536.0;
    const double end = 0.999 * kPi;
    const double ratio = end / start;

    double maxError = 0.0;
    for (int k = 0; k < kMinPhaseFitPoints; ++k)
    {
        const double w = start * std::pow(ratio, static_cast<double>(k) / (kMinPhaseFitPoints - 1));
        const std::complex<double> step(std::cos(w), -std::sin(w));
        std::complex<double> phase(1.0, 0.0);
        std::complex<double> response(0.0, 0.0);
        for (double c : coeffs)
        {
            response += c * phase;
            phase *= step;
        }

        const double firDb = 20.0 * std::log10(std::abs(response) + 1.0e-12);
        const double targetDb = 20.0 * std::log10(magnitude(w) + 1.0e-12);
        maxError = std::max(maxError, std::abs(firDb - targetDb));
    }
    return maxError;
}

} // namespace detail

// Minimum Phase FIR via the cepstral method: magnitude -> log spectrum ->
// causalised cepstrum -> exp -> minimum-phase complex spectrum -> IDFT.
// The direct sound lands at tap 0, so no FIR bulk delay is reported.
//
// 两条路径：
//   · dcZeroOrder == nyquistZeroOrder == 0（调用方未标注零点）：保持 0.4.4
//     旧自动路径。深零点目标（高通 / 带通在 DC 或 Nyquist 的传输零点）在
//     N 点网格上被 epsilon 钳位成 -240 dB 的孤立尖峰，N 点倒谱把它当成
//     宽带特征做时域混叠，会在通带内造出假陷波（v0.4.3 实测 HP30 Q0.9
//     N=4096 通带最深 -31 dB）。故先探测网格最小幅度，低于
//     kMinPhaseDeepNullThreshold 时按 kMinPhaseLegacyOversampling 倍网格
//     重做同一倒谱法并截断回 firLength；良性目标仍走 1× 路径。
//   · 调用方按滤波器拓扑给出精确零点阶数（高通 DC=2、低通 Nyquist=2、
//     带通各 1）：走解析剥离路径。旧 8× 只是抑制混叠、并未真正消除
//     DC/Nyquist 奇点；解析剥离把已知因子从幅频中除掉，对剩余平滑幅频
//     做倒谱设计，再与短因子卷积——因子的零点在时域是精确的相消结构，
//     DC 处 Σh=0、Nyquist 处 Σh(-1)^n=0，不依赖 epsilon 钳位。
//     候选 os=1 与 os=4 各设计一次，用 detail::factoredFitError 选优。
template <typename MagnitudeFn>
inline std::vector<double> designMinimumPhaseFIR(MagnitudeFn magnitude, int firLength, int& latencyOut,
                                                 int dcZeroOrder = 0, int nyquistZeroOrder = 0)
{
    latencyOut = 0;

    // 防御性兜底：非法（负数）或超过输出长度的标注不进入解析路径，
    // 回落到旧自动路径而不是产生越界/空输出。
    if (dcZeroOrder < 0)
        dcZeroOrder = 0;
    if (nyquistZeroOrder < 0)
        nyquistZeroOrder = 0;
    const bool hasAnnotatedZeros = (dcZeroOrder > 0 || nyquistZeroOrder > 0)
                                   && (dcZeroOrder + nyquistZeroOrder < firLength);

    if (hasAnnotatedZeros)
    {
        std::vector<double> best;
        double bestFitError = std::numeric_limits<double>::infinity();
        const int candidates[2] = { 1, kMinPhaseFactoredOversampling };

        for (int oversampling : candidates)
        {
            std::vector<double> h = detail::designFactoredMinimumPhase(
                magnitude, firLength, dcZeroOrder, nyquistZeroOrder, oversampling);

            bool finite = true;
            for (double v : h)
            {
                if (std::isnan(v) || std::isinf(v))
                {
                    finite = false;
                    break;
                }
            }
            if (!finite)
                continue;

            const double fitError = detail::factoredFitError(h, magnitude);
            if (fitError < bestFitError)
            {
                bestFitError = fitError;
                best = h;
            }
        }

        if (!best.empty())
            return best;

        // 两个候选都产生非有限系数：回退冲激（与旧路径一致），
        // 不让 NaN/Inf 进入卷积。
        std::vector<double> impulse(static_cast<size_t>(firLength), 0.0);
        impulse[0] = 1.0;
        return impulse;
    }

    const double epsilon = 1.0e-12;
    std::vector<std::complex<double>> spectrum(firLength);   // 单缓冲：过采样时仅扩容一次

    // Step 1: log magnitude spectrum（同时记录网格上的最小幅度）
    double minMag = std::numeric_limits<double>::infinity();
    auto fillLogSpectrum = [&] (int n)
    {
        minMag = std::numeric_limits<double>::infinity();
        for (int i = 0; i <= n / 2; ++i)
        {
            const double w = kPi * i / (n / 2);
            const double mag = magnitude(w);
            // NaN 比较恒为假：NaN 目标不会命中过采样分支，仍走原回退路径
            if (mag < minMag)
                minMag = mag;
            spectrum[i] = { std::log(mag + epsilon), 0.0 };
        }
        for (int i = n / 2 + 1; i < n; ++i)
            spectrum[i] = { spectrum[n - i].real(), 0.0 };
    };

    int designLength = firLength;
    fillLogSpectrum(designLength);

    if (minMag < kMinPhaseDeepNullThreshold)
    {
        designLength = firLength * kMinPhaseLegacyOversampling;
        spectrum.resize(static_cast<size_t>(designLength));
        fillLogSpectrum(designLength);
    }

    // Step 2: IDFT -> cepstrum
    ifftInPlace(spectrum.data(), designLength);

    // Step 3: causalise the cepstrum
    spectrum[0] = { spectrum[0].real(), 0.0 };
    for (int i = 1; i < designLength / 2; ++i)
        spectrum[i] = { spectrum[i].real() * 2.0, 0.0 };
    spectrum[designLength / 2] = { spectrum[designLength / 2].real(), 0.0 };   // 折叠窗在 Nyquist 项取 1（保留）
    for (int i = designLength / 2 + 1; i < designLength; ++i)
        spectrum[i] = { 0.0, 0.0 };

    // Step 4: DFT -> minimum-phase log spectrum
    fftInPlace(spectrum.data(), designLength);

    // Step 5: exp() -> minimum-phase complex spectrum
    for (int i = 0; i < designLength; ++i)
    {
        const double re = spectrum[i].real();
        const double im = spectrum[i].imag();
        spectrum[i] = { std::exp(re) * std::cos(im), std::exp(re) * std::sin(im) };
    }

    // Step 6: IDFT -> minimum-phase FIR coefficients（过采样网格取前 firLength 个抽头）
    ifftInPlace(spectrum.data(), designLength);

    std::vector<double> coeffsOut(firLength);   // double 抽头：与 DSP 精度约定一致
    bool hasInvalid = false;
    for (int i = 0; i < firLength; ++i)
    {
        const double val = spectrum[i].real();
        if (std::isnan(val) || std::isinf(val))
            hasInvalid = true;
        coeffsOut[i] = val;
    }

    // Fallback to impulse if the cepstral method produced invalid coefficients
    if (hasInvalid)
    {
        std::fill(coeffsOut.begin(), coeffsOut.end(), 0.0);
        coeffsOut[0] = 1.0;
    }

    latencyOut = 0;
    return coeffsOut;
}

} // namespace SubEQ
