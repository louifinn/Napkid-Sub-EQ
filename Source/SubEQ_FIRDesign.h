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
#include <utility>
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

// 候选的抽头峰值 sanity gate：多节点传输零点（dc+nyq ≥ 4）时解析剥离后
// 剩余幅频 R(0) 的动态范围随阶数爆炸（≈ (1/w_c)^{2k}），截断 h_rest 再与
// 二项式因子做双精度卷积会因灾难性相消把有效位全部丢失，产生 1e4…1e50
// 的抽头尖峰。峰值超过该门限的候选一律丢弃（改由 legacy 候选兜底）；
// 合法目标（EQ 单节点增益 ≤ ±24 dB，级联后亦远低于该门限）不会被误杀。
constexpr double kMinPhaseMaxTapMagnitude = 1.0e3;

// 精确零点投影的安全阶数上限：单节点（dc/nyq 合计 2 阶）投影完全稳定；
// 多节点只做「H(0)/H(π) 一阶精确」修正（每个传输零点 1 条矩约束），阶数
// 天然 ≤ 2。整阶多节点投影（3×HP 的 6 阶矩）在接近网格分辨率的 cell
// （96k/N=4096/fc=30）会把通带误差从 0.8 dB 推到 6 dB——高阶矩修正侵入
// 拐点——故多节点不追求整阶精确零点（测试判据本身也是 legacy 近似）。
constexpr int kMaxExactZeroProjectionOrder = 2;

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

// 旧自动路径（0.4.4 行为）：无零点标注目标的完整倒谱最小相位设计。N 点网格
// 上探测最小幅度，低于 kMinPhaseDeepNullThreshold 时改用
// kMinPhaseLegacyOversampling 倍网格重做并截断回 firLength；良性目标走 1×。
// 该函数是「三参 designMinimumPhaseFIR 调用」与「带标注目标的 legacy 候选」
// 的唯一实现，行为与 0.4.4 位级一致（不产生精确 DC/Nyquist 结构零点）。
template <typename MagnitudeFn>
inline std::vector<double> designLegacyMinimumPhase(MagnitudeFn& magnitude, int firLength)
{
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

    return coeffsOut;
}

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

// 带部分主元的高斯消元，解 order×order 小线性系统（order ≤
// kMaxExactZeroProjectionOrder = 2）。就地改写 matrix / rhs，rhs 返回解向量；
// 奇异或非有限时返回 false。
inline bool solveSmallLinearSystem(std::vector<double>& matrix, std::vector<double>& rhs, int order)
{
    for (int col = 0; col < order; ++col)
    {
        int pivot = col;
        double best = std::abs(matrix[col * order + col]);
        for (int row = col + 1; row < order; ++row)
        {
            const double value = std::abs(matrix[row * order + col]);
            if (value > best)
            {
                best = value;
                pivot = row;
            }
        }
        if (!(best > 0.0) || !std::isfinite(best))
            return false;

        if (pivot != col)
        {
            for (int c = 0; c < order; ++c)
                std::swap(matrix[col * order + c], matrix[pivot * order + c]);
            std::swap(rhs[col], rhs[pivot]);
        }

        const double diagonal = matrix[col * order + col];
        for (int row = col + 1; row < order; ++row)
        {
            const double factor = matrix[row * order + col] / diagonal;
            if (factor == 0.0)
                continue;
            for (int c = col; c < order; ++c)
                matrix[row * order + c] -= factor * matrix[col * order + c];
            rhs[row] -= factor * rhs[col];
        }
    }

    for (int row = order - 1; row >= 0; --row)
    {
        double sum = rhs[row];
        for (int c = row + 1; c < order; ++c)
            sum -= matrix[row * order + c] * rhs[c];
        const double diagonal = matrix[row * order + row];
        if (!(std::abs(diagonal) > 0.0) || !std::isfinite(diagonal))
            return false;
        rhs[row] = sum / diagonal;
        if (!std::isfinite(rhs[row]))
            return false;
    }
    return true;
}

// 把候选系数精确投影到「DC 点若干阶矩 + Nyquist 点若干阶矩为零」的子空间：
// 最小范数修正 δ[n] = p(x[n]) + (-1)^n q(x[n])，x[n] = n/(N-1)，p/q 是
// (dcMoments-1) / (nyqMoments-1) 次多项式（该子空间正是卷积因子的正交补，
// 等价于最小二乘投影）。修正量由「修正后各阶矩为零」的小线性系统解出：
//   Σ h[n]·x^k = 0 (k<dcMoments)，Σ h[n]·(-1)^n·x^k = 0 (k<nyqMoments)。
// 传入 (dc, nyq) 即整阶结构零点；传入 (1, 1) 只保证 H(0)/H(π) 精确为零
// （多节点 legacy 候选的近似零点修正用）。
// 注意：cos 端点外推只保证幅频端的有限性，真正的结构零点（H(1)=0 等）
// 由这组矩约束给出；legacy 候选（近似零点）经此投影后与解析候选同精度。
inline bool enforceMinimumPhaseTransmissionZeros(std::vector<double>& coeffs,
                                                 int dcMoments, int nyqMoments)
{
    const int order = dcMoments + nyqMoments;
    if (order <= 0 || order > kMaxExactZeroProjectionOrder)
        return false;

    const int length = static_cast<int>(coeffs.size());
    if (length <= order)
        return false;

    const double invLast = 1.0 / static_cast<double>(length - 1);
    auto dcBasis = [&] (int k, int n)
    {
        double value = 1.0;
        for (int q = 0; q < k; ++q)
            value *= static_cast<double>(n) * invLast;
        return value;
    };
    auto nyqBasis = [&] (int k, int n)
    {
        double value = ((n & 1) != 0) ? -1.0 : 1.0;
        for (int q = 0; q < k; ++q)
            value *= static_cast<double>(n) * invLast;
        return value;
    };

    // 未知量：[a_0..a_{dc-1}, b_0..b_{nyq-1}]。
    // 第 k 个 DC 行：Σ (h[n] + δ[n])·x^k = 0；第 k 个 Nyquist 行：
    // Σ (h[n] + δ[n])·(-1)^n·x^k = 0。
    std::vector<double> gram(static_cast<size_t>(order * order), 0.0);
    std::vector<double> solution(static_cast<size_t>(order), 0.0);

    for (int k = 0; k < dcMoments; ++k)
    {
        double moment = 0.0;
        for (int n = 0; n < length; ++n)
            moment += coeffs[static_cast<size_t>(n)] * dcBasis(k, n);
        solution[static_cast<size_t>(k)] = -moment;

        for (int j = 0; j < dcMoments; ++j)
        {
            double sum = 0.0;
            for (int n = 0; n < length; ++n)
                sum += dcBasis(j, n) * dcBasis(k, n);
            gram[static_cast<size_t>(k * order + j)] = sum;
        }
        for (int j = 0; j < nyqMoments; ++j)
        {
            double sum = 0.0;
            for (int n = 0; n < length; ++n)
                sum += nyqBasis(j, n) * dcBasis(k, n);
            gram[static_cast<size_t>(k * order + dcMoments + j)] = sum;
        }
    }

    for (int k = 0; k < nyqMoments; ++k)
    {
        const int row = dcMoments + k;
        double moment = 0.0;
        for (int n = 0; n < length; ++n)
            moment += coeffs[static_cast<size_t>(n)] * nyqBasis(k, n);
        solution[static_cast<size_t>(row)] = -moment;

        for (int j = 0; j < dcMoments; ++j)
        {
            double sum = 0.0;
            for (int n = 0; n < length; ++n)
                sum += dcBasis(j, n) * nyqBasis(k, n);
            gram[static_cast<size_t>(row * order + j)] = sum;
        }
        for (int j = 0; j < nyqMoments; ++j)
        {
            double sum = 0.0;
            for (int n = 0; n < length; ++n)
                sum += nyqBasis(j, n) * nyqBasis(k, n);
            gram[static_cast<size_t>(row * order + dcMoments + j)] = sum;
        }
    }

    if (!solveSmallLinearSystem(gram, solution, order))
        return false;

    for (int n = 0; n < length; ++n)
    {
        double delta = 0.0;
        for (int j = 0; j < dcMoments; ++j)
            delta += solution[static_cast<size_t>(j)] * dcBasis(j, n);
        for (int j = 0; j < nyqMoments; ++j)
            delta += solution[static_cast<size_t>(dcMoments + j)] * nyqBasis(j, n);
        coeffs[static_cast<size_t>(n)] += delta;
    }
    return true;
}

// 候选拟合误差：在 kMinPhaseFitPoints 个对数间隔角频率上直接 DTFT，取 FIR
// 与目标 dB 幅频差的最大绝对值，但只统计「实际可用带」内的采样点：
//   · 目标幅度不低于本次采样最大值的 1/2（−6 dB），排除阻带（+1e-12 的
//     −240 dB 地板会让阻带点主导 dB 误差）；
//   · 频率不超过 min(500 Hz, 0.45·fs)：本插件是 0.5–500 Hz 的超低频 EQ，
//     500 Hz 以上目标恒为 1，而解析剥离候选在高频的截断误差与本任务无关，
//     若纳入选优会压过真正的低通带精度（实测 96k 单 HP 会因此选中通带
//     差 2.3 dB 的 legacy 而非 0.29 dB 的解析候选）。
// sampleRateHz ≤ 0 时退回纯 −6 dB 门限（全带），供无采样率信息的调用方使用。
// 用相位递推（复步长连乘）代替每个抽头的 cos/sin，误差仅 ~1e-14 rad/65536
// 抽头，远低于 dB 判据。
template <typename MagnitudeFn>
inline double minimumPhaseFitError(const std::vector<double>& coeffs, MagnitudeFn& magnitude,
                                   double sampleRateHz)
{
    const double start = kPi / 65536.0;
    double end = 0.999 * kPi;
    if (sampleRateHz > 0.0)
    {
        const double highHz = std::min(500.0, 0.45 * sampleRateHz);
        const double bandEnd = 2.0 * kPi * highHz / sampleRateHz;
        if (bandEnd > start)
            end = std::min(end, bandEnd);
    }
    const double ratio = end / start;

    double targetMag[kMinPhaseFitPoints];
    double maxTarget = 0.0;
    for (int k = 0; k < kMinPhaseFitPoints; ++k)
    {
        const double w = start * std::pow(ratio, static_cast<double>(k) / (kMinPhaseFitPoints - 1));
        targetMag[k] = magnitude(w);
        if (targetMag[k] > maxTarget)
            maxTarget = targetMag[k];
    }

    const double passbandThreshold = 0.5 * maxTarget;   // −6 dB
    double maxError = 0.0;
    for (int k = 0; k < kMinPhaseFitPoints; ++k)
    {
        if (!(targetMag[k] >= passbandThreshold))
            continue;

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
        const double targetDb = 20.0 * std::log10(targetMag[k] + 1.0e-12);
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
//     旧自动路径（detail::designLegacyMinimumPhase，与旧 3 参调用位级一致）。
//   · 调用方按滤波器拓扑给出精确零点阶数（高通 DC=2、低通 Nyquist=2、
//     带通各 1、级联累加）：在三个候选间选优——
//       ① 解析剥离 os=1，② 解析剥离 os=4，③ legacy 自动路径（0.4.4）。
//     每个候选先做有限性检查与 max|h| ≤ kMinPhaseMaxTapMagnitude 的 sanity
//     gate（多节点零点时解析剥离会灾难性相消、产生 1e4…1e50 尖峰），通过
//     gate 后再做精确零点投影，最后用「可用带对齐」的拟合误差选最优。
//     · 单节点（dc+nyq == 2）：投影整阶结构零点（H(0)=H'(0)=0 或 Nyquist
//       对称），解析候选本身已精确、legacy 候选（近极限 cell）经投影后同样
//       精确——回归测试的 1e-9 断言对单节点始终成立。
//     · 多节点（dc+nyq ≥ 4）：解析候选被 gate 丢弃，legacy 候选兜底（实测
//       通带误差 0.01–3.7 dB）；只把每个传输零点的 H(0)/H(π) 修正到精确
//       零（一阶投影），更高阶矩保留 legacy 近似（测试判据本身即 <1e-3）。
//     没有任何可用候选时回退 legacy，legacy 仍无效则回退冲激。
//     sampleRateHz > 0 时把候选选优的拟合带限制在 min(500 Hz, 0.45·fs) 内
//     （本插件是 0.5–500 Hz 超低频 EQ；不传则用全带，行为退化为 0.4.5）。
//     解析路径的 latencyOut 仍为 0。
template <typename MagnitudeFn>
inline std::vector<double> designMinimumPhaseFIR(MagnitudeFn magnitude, int firLength, int& latencyOut,
                                                 int dcZeroOrder = 0, int nyquistZeroOrder = 0,
                                                 double sampleRateHz = 0.0)
{
    latencyOut = 0;

    // 防御性兜底：非法（负数）或超过输出长度的标注不进入解析路径，
    // 回落到旧自动路径而不是产生越界/空输出。
    if (dcZeroOrder < 0)
        dcZeroOrder = 0;
    if (nyquistZeroOrder < 0)
        nyquistZeroOrder = 0;
    const int zeroOrder = dcZeroOrder + nyquistZeroOrder;
    const bool hasAnnotatedZeros = zeroOrder > 0 && zeroOrder < firLength;

    if (!hasAnnotatedZeros)
        return detail::designLegacyMinimumPhase(magnitude, firLength);

    // 单节点追求整阶精确零点；多节点只做一阶（H(0)/H(π) = 0）修正。
    const bool singleNode = (zeroOrder <= 2);
    const int dcMoments = singleNode ? dcZeroOrder : (dcZeroOrder > 0 ? 1 : 0);
    const int nyqMoments = singleNode ? nyquistZeroOrder : (nyquistZeroOrder > 0 ? 1 : 0);

    auto applyZeroProjection = [&] (std::vector<double>& coeffs)
    {
        // 投影失败（小系统奇异）时保留候选的近似零点，不因此丢弃候选。
        detail::enforceMinimumPhaseTransmissionZeros(coeffs, dcMoments, nyqMoments);
        for (double value : coeffs)
            if (!std::isfinite(value))
                return false;
        return true;
    };

    std::vector<double> best;
    double bestFitError = std::numeric_limits<double>::infinity();

    auto consider = [&] (std::vector<double> coeffs)
    {
        if (static_cast<int>(coeffs.size()) != firLength)
            return;

        // 有限性 + 抽头峰值 sanity gate：丢弃灾难性相消的解析候选。
        double peak = 0.0;
        for (double value : coeffs)
        {
            if (!std::isfinite(value))
                return;
            peak = std::max(peak, std::abs(value));
        }
        if (peak > kMinPhaseMaxTapMagnitude)
            return;

        // 精确零点投影（阶数 ≤ kMaxExactZeroProjectionOrder；对已经精确的
        // 解析候选是 1e-15 量级的无操作修正）。
        if (!applyZeroProjection(coeffs))
            return;

        const double fitError = detail::minimumPhaseFitError(coeffs, magnitude, sampleRateHz);
        if (fitError < bestFitError)
        {
            bestFitError = fitError;
            best = std::move(coeffs);
        }
    };

    const int factoredOversamplings[2] = { 1, kMinPhaseFactoredOversampling };
    for (int oversampling : factoredOversamplings)
        consider(detail::designFactoredMinimumPhase(magnitude, firLength, dcZeroOrder,
                                                    nyquistZeroOrder, oversampling));

    std::vector<double> legacy = detail::designLegacyMinimumPhase(magnitude, firLength);
    consider(legacy);

    if (!best.empty())
        return best;

    // 三个候选都被 sanity gate 拒绝：回退 legacy（允许近似零点），仍无效则冲激。
    bool legacyUsable = static_cast<int>(legacy.size()) == firLength;
    for (double value : legacy)
        if (!std::isfinite(value))
            legacyUsable = false;

    if (legacyUsable && applyZeroProjection(legacy))
        return legacy;

    std::vector<double> impulse(static_cast<size_t>(firLength), 0.0);
    impulse[0] = 1.0;
    return impulse;
}

} // namespace SubEQ
