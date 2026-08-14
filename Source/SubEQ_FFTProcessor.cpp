/*
  ==============================================================================

    SubEQ_FFTProcessor.cpp
    FIR coefficient design (Linear Phase + Minimum Phase) and FFT overlap-add
    convolution. Coefficient design runs on a background thread with the shared
    radix-2 FFT (SubEQ_DSPMath.h) — JUCE FFT wrapper is unreliable here.

  ==============================================================================
*/

#include "SubEQ_FFTProcessor.h"
#include "SubEQ_DSPMath.h"
#include "SubEQ_Parameters.h"
#include "SubEQ_FIRDesign.h"

namespace SubEQ
{

//==============================================================================
FFTProcessor::FFTProcessor()  : juce::Thread ("SubEQ FIR Designer")
{
    // The background design engine must always evaluate exact target
    // coefficients, so disable smoothing on it.
    designEngine.setSmoothingEnabled(false);
}

FFTProcessor::~FFTProcessor()
{
    // Generous join timeout: the worker loop polls threadShouldExit every
    // 50 ms and a single design (longest FIR) completes in well under a
    // second, so 5 s leaves ample margin against scheduler stalls.
    if (!stopThread (5000))
    {
        jassertfalse;   // worker did not exit in time — would be a lifetime bug
        // Release 兜底（F-005）：继续阻塞等待线程退出。相比 std::terminate
        // 拖垮宿主，宁可延长退出时间，也绝不让 run() 在 this 销毁后继续
        // 访问成员（use-after-free）。
        while (isThreadRunning())
            wait (50);
    }
}

//==============================================================================
void FFTProcessor::prepare(double sr, int maxBlockSize, int channels)
{
    sampleRate.store(sr, std::memory_order_release);

    // 块尺寸增大时仅重分配交叉淡化临时缓冲（F-003）：卷积状态与已发布
    // 设计全部保留。process() 的淡化分支要求 numSamples <= xfadeNew 容量，
    // 容量不足会被静默跳过淡化——参数编辑的输出阶跃随之回归。
    const int xfadeSamples = std::max(1, maxBlockSize);
    if (xfadeNew.getNumSamples() < xfadeSamples)
    {
        xfadeOld.setSize(std::max(1, numChannels), xfadeSamples);
        xfadeNew.setSize(std::max(1, numChannels), xfadeSamples);
    }

    // Same session parameters: keep the buffers and any published design.
    // Hosts commonly re-call prepareToPlay (transport stop/start, loop
    // points) with an unchanged setup; dropping the published design would
    // mute the output until the background redesign completes. The design
    // epoch is deliberately NOT bumped here: an in-flight design was built
    // from the same sample rate and is still valid — discarding it would
    // silently revert the user's latest parameter edit.
    if (sr == preparedSampleRate && channels == numChannels && !fftWork.empty())
        return;

    // Any design started before this prepare is stale (it may have snapshotted
    // a previous sample rate) and must not be published.
    designEpoch.fetch_add(1, std::memory_order_release);

    preparedSampleRate = sr;
    numChannels = channels;

    // Pre-allocate for the longest FIR so the audio thread never allocates
    // when a new design is published mid-stream.
    const int maxL = convolutionFftSize(MaxFirLength, ConvBlockLen);

    channelStates.resize(numChannels);
    for (auto& ch : channelStates)
    {
        ch.inputBlock.assign(ConvBlockLen, { 0.0, 0.0 });
        ch.pos = 0;
        ch.overlap.assign(maxL, 0.0);
        ch.outQueue.clear();
        ch.outQueue.reserve(ConvBlockLen * 2);
        ch.outRead = 0;
    }

    // 交叉淡化用的旧设计累加器：与 channelStates 同构预分配，拷贝赋值时
    // 容量足够、不会在音频线程分配。
    oldChannelStates.resize(numChannels);
    for (auto& ch : oldChannelStates)
    {
        ch.inputBlock.assign(ConvBlockLen, { 0.0, 0.0 });
        ch.pos = 0;
        ch.overlap.assign(maxL, 0.0);
        ch.outQueue.clear();
        ch.outQueue.reserve(ConvBlockLen * 2);
        ch.outRead = 0;
    }
    xfadeOld.setSize(numChannels, std::max(1, maxBlockSize));
    xfadeNew.setSize(numChannels, std::max(1, maxBlockSize));
    crossfadeRemaining = 0;
    processingState = nullptr;

    fftWork.assign(maxL, { 0.0, 0.0 });
    activeConvFFTSize = 0;
    pendingState = nullptr;
    twiddlePrewarmedSize = 0;

    // Pre-build the twiddle tables for every selectable convolution size on
    // this thread, so the first real-time FFT does not allocate (the cache
    // is thread_local; prepareToPlay normally runs on the audio thread).
    for (int i = 0; i < NumFirLengthChoices; ++i)
        prewarmTwiddleTable<double>(convolutionFftSize(FirLengthChoices[i], ConvBlockLen));

    // Drop any previously published design: the engine will be redesigned
    // once the mode/parameters demand it. 全部移交退役环（仅指针搬移），
    // ~2.3 MB 的 FIRState 析构由后台线程在锁外执行（F-002）。
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (currentState != nullptr)
        {
            pendingDestroy[pendingDestroyHead] = std::move (currentState);
            pendingDestroyHead = (pendingDestroyHead + 1) % NumRetireSlots;
        }
        if (oldState != nullptr)
        {
            pendingDestroy[pendingDestroyHead] = std::move (oldState);
            pendingDestroyHead = (pendingDestroyHead + 1) % NumRetireSlots;
        }
    }
    stateVersion.fetch_add(1, std::memory_order_release);

    // The published design was just dropped. If the plugin is in a FIR mode
    // the audio thread would otherwise output silence (or stale coefficients)
    // until the user touches a parameter again — request a redesign right
    // away. Non-blocking and audio-thread safe; the worker ignores the
    // request in Zero Latency mode.
    requestRedesign();
}

void FFTProcessor::reset()
{
    for (auto& ch : channelStates)
    {
        std::fill(ch.inputBlock.begin(), ch.inputBlock.end(), std::complex<double>(0.0, 0.0));
        ch.pos = 0;
        std::fill(ch.overlap.begin(), ch.overlap.end(), 0.0);
        ch.outQueue.clear();
        ch.outRead = 0;
    }
    for (auto& ch : oldChannelStates)
    {
        std::fill(ch.inputBlock.begin(), ch.inputBlock.end(), std::complex<double>(0.0, 0.0));
        ch.pos = 0;
        std::fill(ch.overlap.begin(), ch.overlap.end(), 0.0);
        ch.outQueue.clear();
        ch.outRead = 0;
    }
    crossfadeRemaining = 0;
    processingState = nullptr;
    pendingState = nullptr;
    retireState (oldState);
}

void FFTProcessor::clearPublishedState()
{
    crossfadeRemaining = 0;
    processingState = nullptr;
    pendingState = nullptr;
    retireState (currentState);
    retireState (oldState);
    stateVersion.fetch_add(1, std::memory_order_release);
}

void FFTProcessor::setParameterSource(juce::AudioProcessorValueTreeState* source)
{
    parameterSource.store(source, std::memory_order_release);
    startThread();
}

void FFTProcessor::requestRedesign()
{
    redesignRequested.store(true, std::memory_order_release);
    redesignEvent.signal();
}

//==============================================================================
// Background thread: wait for a redesign request, build a design-time engine
// snapshot from the parameter source, design the FIR + frequency coefficients
// and publish them.
void FFTProcessor::run()
{
    while (!threadShouldExit())
    {
        // 排空退役环：先把槽位移出临界区再析构（F-002）——~2.3 MB 的
        // FIRState 释放不占用 stateMutex，音频线程的过渡期锁点不会被
        // 放大为"等待后台线程完成大块释放"。
        std::shared_ptr<const FIRState> toFree[NumRetireSlots];
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            for (int i = 0; i < NumRetireSlots; ++i)
                toFree[i] = std::move (pendingDestroy[i]);
        }

        redesignEvent.wait(50);

        if (!redesignRequested.exchange(false, std::memory_order_acquire))
            continue;

        auto* source = parameterSource.load(std::memory_order_acquire);
        if (source == nullptr)
            continue;

        const auto mode = static_cast<EQMode> (
            static_cast<int> (source->getRawParameterValue ("eq_mode")->load()));

        if (mode == EQMode::ZeroLatency)
            continue;

        const int firChoice = static_cast<int> (
            source->getRawParameterValue ("fir_length")->load());
        const int firLength = (firChoice >= 0 && firChoice < NumFirLengthChoices)
                                  ? FirLengthChoices[firChoice]
                                  : DefaultFirLength;

        // Capture the design epoch before snapshotting anything: if prepare()
        // runs while this design is in flight, the design is stale (e.g. it
        // snapshotted a previous sample rate) and must be discarded, not
        // published — otherwise the audio thread would consume coefficients
        // computed for the wrong sample rate until the next redesign.
        const int epoch = designEpoch.load(std::memory_order_acquire);

        // Build an engine snapshot owned by this thread; the parameter values
        // are read atomically from the APVTS, so no lock is needed.
        const double sr = sampleRate.load(std::memory_order_acquire);
        if (sr != designSampleRate)
        {
            designEngine.prepare(sr, 512);
            designSampleRate = sr;
        }
        applyParametersToEngine(*source, designEngine);

        auto newState = std::make_shared<FIRState>();
        newState->firLength = firLength;

        auto magnitude = [&](double w) { return designEngine.getMagnitudeLinear(w); };
        if (mode == EQMode::LinearPhase)
            newState->coeffs = designLinearPhaseFIR(magnitude, firLength, newState->groupDelay);
        else
            newState->coeffs = designMinimumPhaseFIR(magnitude, firLength, newState->groupDelay);

        // Pre-compute the frequency-domain filter for overlap-add convolution
        newState->convFFTSize = convolutionFftSize(firLength, ConvBlockLen);
        std::vector<std::complex<double>> spec(newState->convFFTSize);
        for (int i = 0; i < firLength; ++i)
            spec[i] = { newState->coeffs[i], 0.0 };
        for (int i = firLength; i < newState->convFFTSize; ++i)
            spec[i] = { 0.0, 0.0 };
        fftInPlace(spec.data(), newState->convFFTSize);

        newState->freqCoeffs = spec;   // double precision frequency response

        // Publish (release) so the audio thread sees a fully-formed state.
        // epoch 复检与发布必须在同一临界区内（F-001）：prepare() 先 bump
        // epoch、再在锁内清空 currentState——若校验在锁外、发布在锁内，
        // 采样率切换可在二者之间交错，把按旧采样率设计的系数写回。
        // 被替换的状态移出临界区析构（若为最后引用），大块释放不占用
        // 锁（F-002）。
        std::shared_ptr<const FIRState> replaced;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (epoch != designEpoch.load(std::memory_order_acquire))
                continue;   // 过期设计：丢弃（newState 在锁外析构）

            replaced = std::move (currentState);
            currentState = newState;
        }
        stateVersion.fetch_add(1, std::memory_order_release);
    }
}

//==============================================================================
void FFTProcessor::retireState(std::shared_ptr<const FIRState>& stateToRetire)
{
    if (stateToRetire == nullptr)
        return;

    // 只做指针搬移：FIRState 的析构由后台线程在锁外执行（F-002）。
    std::lock_guard<std::mutex> lock(stateMutex);
    pendingDestroy[pendingDestroyHead] = std::move (stateToRetire);
    pendingDestroyHead = (pendingDestroyHead + 1) % NumRetireSlots;
}

void FFTProcessor::flushConvolvers()
{
    // 冲刷新侧卷积累加器与输出队列：旧设计的频率域尾部立即终止，输出
    // 从下一块起纯为新设计（延迟不同的设计切换路径，见 process()）。
    for (auto& ch : channelStates)
    {
        std::fill(ch.inputBlock.begin(), ch.inputBlock.end(), std::complex<double>(0.0, 0.0));
        ch.pos = 0;
        std::fill(ch.overlap.begin(), ch.overlap.end(), 0.0);
        ch.outQueue.clear();
        ch.outRead = 0;
    }
}

//==============================================================================
std::shared_ptr<const FFTProcessor::FIRState> FFTProcessor::getPublishedStateForAudioThread() const
{
    // Fast path: no new publish since we last looked — zero locks.
    if (stateVersion.load(std::memory_order_acquire) != lastSeenVersion)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        // 被替换的状态移交退役环，而不是在音频线程析构（~2.3 MB 释放）。
        if (localState != nullptr)
        {
            pendingDestroy[pendingDestroyHead] = std::move (localState);
            pendingDestroyHead = (pendingDestroyHead + 1) % NumRetireSlots;
        }
        localState = currentState;
        lastSeenVersion = stateVersion.load(std::memory_order_relaxed);
    }

    return localState;
}

//==============================================================================
void FFTProcessor::process(juce::AudioBuffer<float>& buffer)
{
    auto state = getPublishedStateForAudioThread();
    if (state == nullptr || state->freqCoeffs.empty())
        return;

    const int version = stateVersion.load(std::memory_order_acquire);

    // ---- 发布-订阅接管（F-004）：淡化期间发布的新设计先挂起 ----
    bool deferred = false;   // 本块挂起新设计、继续推进当前淡化
    if (crossfadeRemaining > 0 && version != processedVersion)
    {
        // 延迟不同的设计（FIR 长度/模式变化）必须立即收尾淡化并冲刷切换：
        // PDC 在发布当块已经翻转，继续淡化会让旧延迟尾音按新延迟补偿
        // （可闻前回声）。同延迟设计挂起，等淡化收尾后再切换——新侧保持
        // 原设计，不会在混合权重下中途跳变。
        if (processingState != nullptr && state->groupDelay != processingState->groupDelay)
        {
            crossfadeRemaining = 0;
            retireState (oldState);
            flushConvolvers();
        }
        else if (processingState != nullptr)
        {
            pendingState = state;
            state = processingState;
            deferred = true;   // 跳过下方版本切换分支：processedVersion 保持淡化新侧版本
        }
    }
    else if (crossfadeRemaining <= 0 && pendingState != nullptr)
    {
        // 淡化已收尾：采纳挂起的最新设计（下方版本切换分支开启新淡化/冲刷）
        state = pendingState;
        pendingState = nullptr;
    }

    const int L = state->convFFTSize;
    if (L <= 0 || L > static_cast<int>(fftWork.size()))
        return;

    // 音频线程惰性预热 twiddle（F-010）：宿主可能在非音频线程调用
    // prepareToPlay，thread_local 缓存在本线程未命中；命中后为廉价查找。
    if (L != twiddlePrewarmedSize)
    {
        prewarmTwiddleTable<double>(L);
        twiddlePrewarmedSize = L;
    }

    // 新发布设计的切换策略（F-006）：同延迟设计走 CrossfadeLen 样本交叉
    // 淡化（两侧管线延迟一致，混合无时间错位，参数编辑无输出阶跃）；延迟
    // 不同的设计（FIR 长度/模式变化）冲刷累加器立即切换——旧设计尾音若
    // 按新延迟补偿会变成可闻前回声，宁可瞬时切换也要保证 PDC 与实际输出
    // 始终一致。
    if (!deferred && (version != processedVersion || L != activeConvFFTSize))
    {
        const std::shared_ptr<const FIRState> prev = processingState;

        if (prev != nullptr && crossfadeRemaining <= 0)
        {
            if (prev->groupDelay == state->groupDelay)
            {
                // 同延迟交叉淡化（参数编辑、最小相位换 FIR 长度等）
                const int oldL = activeConvFFTSize;
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    oldChannelStates[ch] = channelStates[ch];
                    // FIR 长度变化时，旧累加器在 [oldL, newL) 区域的数据在新
                    // 尺寸下会被读取——先清零，避免陈旧的未初始化数据混入。
                    if (L > oldL)
                    {
                        const int zeroEnd = std::min(L, static_cast<int>(oldChannelStates[ch].overlap.size()));
                        std::fill(oldChannelStates[ch].overlap.begin() + oldL,
                                  oldChannelStates[ch].overlap.begin() + zeroEnd, 0.0);
                    }
                }
                oldState = prev;   // 上一块实际使用的设计
                fadeConvFFTSize = oldL;
                crossfadeRemaining = CrossfadeLen;
            }
            else
            {
                // 延迟不同：冲刷新侧累加器，立即切换到新设计。
                flushConvolvers();
            }
        }
        activeConvFFTSize = L;
        processedVersion = version;
    }

    const int channelCount = std::min(buffer.getNumChannels(), numChannels);
    const int numSamples = buffer.getNumSamples();
    const bool hasOld = (oldState != nullptr) && !oldState->freqCoeffs.empty();

    if (crossfadeRemaining > 0 && numSamples <= xfadeNew.getNumSamples())
    {
        // 双卷积 + 线性斜坡混合：旧侧从当前累加器无缝续接，新侧从 0 权重
        // 渐入；权重按样本递增，淡化结束后的输出纯为新设计。
        const int startRemaining = crossfadeRemaining;
        const float invLen = 1.0f / static_cast<float>(CrossfadeLen + 1);
        for (int ch = 0; ch < channelCount; ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            if (hasOld)
            {
                processOlaChannel(oldChannelStates[ch],
                                  xfadeOld.getWritePointer(ch), xfadeOld.getWritePointer(ch),
                                  numSamples, ConvBlockLen, fadeConvFFTSize,
                                  oldState->freqCoeffs.data(), fftWork);
            }
            processOlaChannel(channelStates[ch],
                              xfadeNew.getWritePointer(ch), xfadeNew.getWritePointer(ch),
                              numSamples, ConvBlockLen, L,
                              state->freqCoeffs.data(), fftWork);

            const float* oldData = hasOld ? xfadeOld.getReadPointer(ch) : nullptr;
            const float* newData = xfadeNew.getReadPointer(ch);
            for (int n = 0; n < numSamples; ++n)
            {
                const int remaining = startRemaining - n;
                if (remaining <= 0)
                {
                    data[n] = newData[n];
                    continue;
                }
                const float w = static_cast<float>(CrossfadeLen - remaining + 1) * invLen;
                data[n] = w * newData[n] + (hasOld ? (1.0f - w) * oldData[n] : 0.0f);
            }
        }

        crossfadeRemaining = std::max(0, startRemaining - numSamples);
        if (crossfadeRemaining == 0)
            retireState (oldState);   // 淡化结束：旧设计移交退役环（锁内仅指针搬移）
    }
    else
    {
        if (crossfadeRemaining > 0)
        {
            // 临时缓冲不足（宿主块尺寸超预期）或旧设计不可用：直接收尾淡化，
            // 避免状态悬挂。（F-003 已使块尺寸变化提前重分配缓冲，此分支
            // 仅剩超常规宿主行为时的兜底。）
            crossfadeRemaining = 0;
            retireState (oldState);
        }
        for (int ch = 0; ch < channelCount; ++ch)
        {
            auto* data = buffer.getWritePointer(ch);
            processOlaChannel(channelStates[ch], data, data, numSamples,
                              ConvBlockLen, L, state->freqCoeffs.data(), fftWork);
        }
    }

    processingState = state;   // 记录本块实际使用的设计，供下次淡化起点
}

//==============================================================================
int FFTProcessor::getLatencySamples() const
{
    auto state = getPublishedStateForAudioThread();
    if (state == nullptr)
        return 0;
    // Overlap-add pipeline delays the output by ConvBlockLen-1 samples
    // (the first output sample appears after the first complete block).
    //
    // PDC 与实际输出的一致性（F-006）：延迟不同的设计发布后立即冲刷切换
    // （无交叉淡化），因此 PDC 翻转的当块起输出即按新延迟补偿；同延迟
    // 设计走 1024 样本交叉淡化且延迟数值不变——两条路径下上报的延迟都
    // 匹配实际输出的管线延迟。
    return state->groupDelay + (ConvBlockLen - 1);
}

bool FFTProcessor::isReady() const
{
    auto state = getPublishedStateForAudioThread();
    return (state != nullptr) && !state->freqCoeffs.empty();
}

int FFTProcessor::getTailLengthSamples() const
{
    // Lock-guarded direct read — may be called from the host/GUI thread, so it
    // must not touch the audio-thread localState cache.
    std::lock_guard<std::mutex> lock(stateMutex);
    // 未发布时按最大 FIR 长度回退（保守上界）：用户可能选择了 16384/65536
    // 而默认值 4096 会低估宿主应等待的卷积尾。
    if (currentState == nullptr)
        return MaxFirLength + ConvBlockLen;
    return currentState->firLength + ConvBlockLen;
}

//==============================================================================
juce::StringArray FFTProcessor::getModeChoices()
{
    return { "Zero Latency", "Minimum Phase", "Linear Phase" };
}

juce::String FFTProcessor::getModeName(EQMode mode)
{
    switch (mode)
    {
        case EQMode::ZeroLatency:   return "Zero Latency";
        case EQMode::MinimumPhase:  return "Minimum Phase";
        case EQMode::LinearPhase:   return "Linear Phase";
    }
    return "Zero Latency";
}

juce::String FFTProcessor::getLatencyText(int latencySamples, double sr)
{
    double ms = (sr > 0.0) ? (latencySamples * 1000.0 / sr) : 0.0;
    return juce::String::formatted("Latency: %.1f ms (%d samples)", ms, latencySamples);
}

} // namespace SubEQ
