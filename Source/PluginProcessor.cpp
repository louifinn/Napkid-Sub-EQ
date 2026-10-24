/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <functional>
#include <set>

namespace
{
    // Cache column indexes derived from SubEQ::ParamID, so the enum stays the
    // single source of truth for parameter order (see nodeParams in
    // PluginProcessor.h).
    constexpr int kParamFreq    = static_cast<int> (SubEQ::ParamID::Freq);
    constexpr int kParamGain    = static_cast<int> (SubEQ::ParamID::Gain);
    constexpr int kParamQ       = static_cast<int> (SubEQ::ParamID::Q);
    constexpr int kParamType    = static_cast<int> (SubEQ::ParamID::Type);
    constexpr int kParamEnabled = static_cast<int> (SubEQ::ParamID::Enabled);
}

//==============================================================================
SubEQAudioProcessor::SubEQAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
       apvts (*this, nullptr, juce::Identifier ("SubEQParameters"), SubEQ::createParameterLayout())
#else
     : apvts (*this, nullptr, juce::Identifier ("SubEQParameters"), SubEQ::createParameterLayout())
#endif
{
    // Cache atomic parameter pointers once (APVTS owns them for our lifetime)
    masterGainParam = apvts.getRawParameterValue ("master_gain");
    bypassParam = apvts.getRawParameterValue ("bypass");
    eqModeParam = apvts.getRawParameterValue ("eq_mode");
    firLengthParam = apvts.getRawParameterValue ("fir_length");
    spectrumFftParam = apvts.getRawParameterValue ("spectrum_fft_size");
    spectrumWindowParam = apvts.getRawParameterValue ("spectrum_window");
    spectrumDensityParam = apvts.getRawParameterValue ("spectrum_band_density");
    spectrumHopParam = apvts.getRawParameterValue ("spectrum_hop_size");
    spectrumSmoothingParam = apvts.getRawParameterValue ("spectrum_smoothing");

    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        nodeParams[i][kParamFreq] = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Freq));
        nodeParams[i][kParamGain] = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Gain));
        nodeParams[i][kParamQ] = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Q));
        nodeParams[i][kParamType] = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Type));
        nodeParams[i][kParamEnabled] = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Enabled));
    }

    // Deferred PDC reporting poll (message thread) — see timerCallback.
    // 30 Hz keeps the report→ack→guard handshake well below the old 100 ms.
    startTimerHz (30);
}

void SubEQAudioProcessor::timerCallback()
{
    // The audio thread sets latencyDirty instead of calling setLatencySamples()
    // directly: setLatencySamples -> updateHostDisplay allocates and takes the
    // message queue lock, which must not happen on the audio thread.
    if (latencyDirty.exchange (false))
    {
        const uint64_t packed = latencyRequest.load (std::memory_order_acquire);
        const int desired = static_cast<int> (static_cast<int32_t> (packed & 0xffffffffu));
        const int version = static_cast<int> (static_cast<int32_t> (packed >> 32));
        setLatencySamples (desired);
        fftProcessor.acknowledgeLatency (version);
    }
}

SubEQAudioProcessor::~SubEQAudioProcessor()
{
}

//==============================================================================
const juce::String SubEQAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool SubEQAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool SubEQAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool SubEQAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double SubEQAudioProcessor::getTailLengthSeconds() const
{
    // FIR modes keep up to (FIR length + block latency) samples in the
    // pipeline; report the full tail so hosts don't truncate the convolution
    // tail on transport stop.
    // Read the parameter, not currentMode: hosts may query the tail right
    // after setStateInformation, before the first processBlock updates the
    // audio-thread cache.
    const auto mode = static_cast<SubEQ::EQMode> (
        juce::roundToInt (eqModeParam != nullptr ? eqModeParam->load() : 0.0f));
    if (mode == SubEQ::EQMode::ZeroLatency)
        return 0.0;

    const double sr = getSampleRate();
    if (sr <= 0.0)
        return 0.0;

    const int choice = firLengthParam != nullptr
                           ? juce::roundToInt (firLengthParam->load()) : 0;
    const int index = juce::jlimit (0, SubEQ::FFTProcessor::NumFirLengthChoices - 1, choice);
    const int nominal = SubEQ::FFTProcessor::FirLengthChoices[index];
    const int taps = (mode == SubEQ::EQMode::LinearPhase)
                         ? std::max (1, nominal - 1) : nominal;
    return static_cast<double> (taps + SubEQ::FFTProcessor::ConvBlockLen) / sr;
}

std::shared_ptr<const SubEQ::FFTProcessor::FIRState>
SubEQAudioProcessor::getFirDesignSnapshot() const
{
    return fftProcessor.getFirDesignSnapshot();
}

int SubEQAudioProcessor::getFirDesignVersion() const
{
    return fftProcessor.getDesignVersion();
}

int SubEQAudioProcessor::getNumPrograms()
{
    return 1;
}

int SubEQAudioProcessor::getCurrentProgram()
{
    return 0;
}

void SubEQAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String SubEQAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void SubEQAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void SubEQAudioProcessor::requestLatencyReport(int version, int latency) noexcept
{
    reportedLatency.store (latency);
    pendingLatencyVersion.store (version);
    const uint64_t packed = (static_cast<uint64_t> (static_cast<uint32_t> (version)) << 32)
                          | static_cast<uint64_t> (static_cast<uint32_t> (latency));
    latencyRequest.store (packed, std::memory_order_release);
    latencyDirty.store (true, std::memory_order_release);
}

int SubEQAudioProcessor::nominalLatencySamples() const noexcept
{
    const int mode = eqModeParam != nullptr
                         ? juce::roundToInt (eqModeParam->load())
                         : static_cast<int> (SubEQ::EQMode::ZeroLatency);
    if (mode == static_cast<int> (SubEQ::EQMode::ZeroLatency))
        return 0;
    if (mode == static_cast<int> (SubEQ::EQMode::MinimumPhase))
        return SubEQ::FFTProcessor::ConvBlockLen - 1;

    const int choice = firLengthParam != nullptr
                           ? juce::roundToInt (firLengthParam->load())
                           : 0;
    const int index = juce::jlimit (0, SubEQ::FFTProcessor::NumFirLengthChoices - 1, choice);
    const int nominal = SubEQ::FFTProcessor::FirLengthChoices[index];
    const int taps = std::max (1, nominal - 1);         // Type-I
    return (taps - 1) / 2 + SubEQ::FFTProcessor::ConvBlockLen - 1;
}

void SubEQAudioProcessor::delayIirPath(juce::AudioBuffer<float>& buffer,
                                       int nch, int numSamples) noexcept
{
    if (iirHistorySize <= 0 || iirHistoryChannels <= 0 || iirHistory.empty())
        return;

    const int latency = juce::jlimit (0, iirHistorySize - 1, iirPathLatency);
    const int channels = std::min (nch, iirHistoryChannels);
    const int ringSize = iirHistorySize;

    for (int n = 0; n < numSamples; ++n)
    {
        const int readIndex = (latency > 0)
                                  ? (iirHistoryWrite - latency + ringSize) % ringSize
                                  : -1;
        for (int ch = 0; ch < channels; ++ch)
        {
            float* data = buffer.getWritePointer (ch);
            const float current = data[n];
            const float delayed = (readIndex >= 0)
                                      ? iirHistory[static_cast<size_t>(ch)
                                                   * static_cast<size_t>(ringSize)
                                                   + static_cast<size_t>(readIndex)]
                                      : current;
            iirHistory[static_cast<size_t>(ch) * static_cast<size_t>(ringSize)
                       + static_cast<size_t>(iirHistoryWrite)] = current;
            data[n] = delayed;
        }
        if (++iirHistoryWrite == ringSize)
            iirHistoryWrite = 0;
    }
}

//==============================================================================
void SubEQAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // A prepare that changes sample rate or channel count is a new session:
    // discard the host-facing latency and report 0 until the IIR bridge / new
    // FIR design is ready. A same-session transport stop/start keeps both the
    // active design and the reported latency.
    const int channelsNow = std::max (1, getTotalNumInputChannels());
    const bool fullReset = lastPreparedSampleRate <= 0.0
                        || std::abs (sampleRate - lastPreparedSampleRate) > 1.0e-9
                        || channelsNow != lastPreparedChannels;
    lastPreparedSampleRate = sampleRate;
    lastPreparedChannels = channelsNow;

    // PDC 不再在音频线程直接调用 setLatencySamples()（音频线程禁止）：延迟上报
    // 统一由 processBlock 置脏标记、timerCallback 在消息线程执行；同会话重入
    // prepare（transport stop/start）时已发布设计与延迟上报均被保留，无需动作。
    eqEngine.prepare (sampleRate);
    eqEngine.reset();
    outputStage.prepare (sampleRate, SubEQ::FFTProcessor::MaxLatencySamples, channelsNow);
    outputStage.setMasterDb (static_cast<double> (masterGainParam->load()));
    outputStage.setBypass (bypassParam->load() > 0.5f);
    outputStage.snapToTargets();
    dryInput.setSize (channelsNow, std::max (samplesPerBlock, 32768));
    transitionBuffer.setSize (channelsNow, std::max (samplesPerBlock, 32768));
    iirHistorySize = SubEQ::FFTProcessor::MaxLatencySamples + 1;
    iirHistoryChannels = channelsNow;
    if (fullReset)
    {
        std::fill (iirHistory.begin(), iirHistory.end(), 0.0f);
        iirHistory.assign (static_cast<size_t> (iirHistoryChannels)
                               * static_cast<size_t> (iirHistorySize), 0.0f);
        iirHistoryWrite = 0;
        iirPathLatency = nominalLatencySamples();
        requestLatencyReport (fftProcessor.getDesignVersion(), iirPathLatency);
        lastRequestedDesignVersion = -1;
        switchGateGain = 1.0f;
        // First FIR entry is handled by the near-target coordinator; do not
        // mute the session start (offline in particular must start audible).
        switchGateHoldSamples = 0;
        latencyTransitionActive = false;
        latencyAbortCooldownSamples = 0;
        latencyAbortVersion = -1;
    }
    switchGateStep = 1.0f / static_cast<float> (std::max (1.0, 0.005 * sampleRate));
    firEntryFadeRemaining = 0;
    fftProcessor.prepare (sampleRate, samplesPerBlock, getTotalNumInputChannels());
    fftProcessor.setParameterSource (&apvts);
    // Do NOT reset the convolution on a same-session prepare: prepare()
    // already resets it when the sample rate/channel count changes, and
    // dropping an active design here would force a needless re-warm + PDC
    // handshake on every transport stop/start.
    spectrumAnalyzer.prepare (sampleRate);
    inputAnalyzer.prepare (sampleRate);
    // 不强制 currentMode/eqModeCache 归零：同会话重复 prepare 时 eq_mode
    // 参数未变，若清空缓存，updateEQParameters 会误报“模式变化”，触发一次
    // 多余的 FIR 重设计并在发布时冲刷卷积尾（播放中出现输出跳变）。仅当
    // 参数真正变化时才由 updateEQParameters 置 modeChanged。
    modeChanged = false;
    eqParamsChanged = false;
    lastRequestedDesignVersion = -1;
    updateEQParameters();
    // Keep lastMode from the previous session: if updateEQParameters()
    // detected a real mode change, the first processBlock must see the old
    // mode as prevMode and run the ZL<->FIR bridge/coordination path.
}

void SubEQAudioProcessor::releaseResources()
{
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool SubEQAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

void SubEQAudioProcessor::updateEQParameters()
{
    // Master gain and bypass are post-convolution output-stage parameters: they
    // are read directly by OutputStage every block and never request a redesign.

    // Update EQ mode
    int mode = static_cast<int> (eqModeParam->load());
    if (mode != eqModeCache)
    {
        eqModeCache = mode;
        currentMode.store (mode);
        modeChanged = true;
    }

    // FIR length change also requires a redesign in FIR modes
    int firLength = static_cast<int> (firLengthParam->load());
    if (firLength != firLengthCache)
    {
        firLengthCache = firLength;
        eqParamsChanged = true;
        firLengthChanged = true;
    }

    // Spectrum analyzer configuration changes (audio-thread safe: the
    // analysers only switch pre-built kernel-set pointers and write plain
    // integers / pre-allocated buffers)。五元组打包成 SpectrumSettings：
    // 一次比较、一次解码（解码器在 SubEQ_SpectrumConfig.h）。
    const SpectrumSettings settings {
        static_cast<int> (spectrumFftParam->load()),
        static_cast<int> (spectrumWindowParam->load()),
        static_cast<int> (spectrumDensityParam->load()),
        static_cast<int> (spectrumHopParam->load()),
        static_cast<int> (spectrumSmoothingParam->load())
    };
    if (settings != spectrumSettings)
    {
        spectrumSettings = settings;

        const int resolutionIndex = SubEQ::spectrumResolutionChoiceToIndex (settings.resolution);
        const SubEQ::WindowType window = SubEQ::spectrumWindowChoiceToType (settings.window);
        const int numBands = SubEQ::spectrumDensityChoiceToBands (settings.density);
        const int hop = SubEQ::spectrumHopChoiceToSamples (settings.hop);
        const int smoothingIndex = SubEQ::spectrumSmoothingChoiceToIndex (settings.smoothing);

        spectrumAnalyzer.configure (resolutionIndex, numBands, hop, window, smoothingIndex);
        inputAnalyzer.configure (resolutionIndex, numBands, hop, window, smoothingIndex);
    }

    // Update each node
    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        float freq   = nodeParams[i][kParamFreq]->load();
        float gain   = nodeParams[i][kParamGain]->load();
        float qVal   = nodeParams[i][kParamQ]->load();
        int type     = static_cast<int> (nodeParams[i][kParamType]->load());
        bool enabled = nodeParams[i][kParamEnabled]->load() > 0.5f;

        auto& cache = nodeCache[i];
        // Epsilon comparison: prevents coefficient recalculation from tiny float jitter
        // which causes IIR clicks, especially severe with small buffer sizes
        bool changed = (std::abs (freq - cache.freq) > 0.001f)
                    || (std::abs (gain - cache.gain) > 0.001f)
                    || (std::abs (qVal - cache.q) > 0.001f)
                    || (type != cache.type)
                    || (enabled != cache.enabled);

        if (changed)
        {
            cache.freq = freq;
            cache.gain = gain;
            cache.q = qVal;
            cache.type = type;
            cache.enabled = enabled;

            SubEQ::applyNodeValuesToEngine (eqEngine, i, freq, gain, qVal, type, enabled);

            eqParamsChanged = true;
        }
    }
}

void SubEQAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused (midiMessages);

    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    // Offline rendering has no real-time deadline; let the FIR path build and
    // adopt synchronously instead of waiting for the background scheduler.
    fftProcessor.setNonRealtime (isNonRealtime());

    // Capture the raw input before any processing. The history is published at
    // the end of the block, so warm states always align with the start of the
    // next block (not with samples that have not been convolved yet).
    const int dryChannels = std::min (totalNumInputChannels, SubEQ::EQNode::MaxChannels);
    const bool haveDryCopy = dryInput.getNumSamples() >= buffer.getNumSamples()
                          && dryInput.getNumChannels() >= dryChannels;
    if (haveDryCopy)
    {
        for (int ch = 0; ch < dryChannels; ++ch)
            dryInput.copyFrom (ch, 0, buffer, ch, 0, buffer.getNumSamples());
    }

    {
        float* historyPtrs[SubEQ::EQNode::MaxChannels] = {};
        const int historyChannels = dryChannels;
        for (int ch = 0; ch < historyChannels; ++ch)
            historyPtrs[ch] = buffer.getWritePointer (ch);
        fftProcessor.captureHistory (historyPtrs, historyChannels,
                                     buffer.getNumSamples(), false);
    }

    // Update EQ parameters before processing
    const int prevMode = lastMode;
    updateEQParameters();

    if (spectrumDisplayResetRequested.exchange (false, std::memory_order_acquire))
    {
        spectrumAnalyzer.resetDisplayState();
        inputAnalyzer.resetDisplayState();
    }

    const auto mode = static_cast<SubEQ::EQMode> (currentMode.load());
    const bool lengthChanged = firLengthChanged;
    firLengthChanged = false;

    // Handle mode change (latency reporting + FIR redesign)
    if (modeChanged)
    {
        modeChanged = false;

        if (mode == SubEQ::EQMode::ZeroLatency)
        {
            // Leaving FIR: flush its OLA state and restore the IIR chain. The
            // IIR engine was not advanced while FIR was active, so reset it.
            // Mute immediately before changing the time axis: a 5 ms fade
            // after the switch would leak mistimed audio.
            const bool leavingFir = reportedLatency.load() != 0;
            fftProcessor.resetConvolution();
            eqEngine.reset();
            latencyTransitionActive = false;

            iirPathLatency = 0;
            if (leavingFir)
            {
                switchGateGain = 0.0f;
                switchGateHoldSamples = static_cast<int> (
                    std::max (1.0, 0.05 * getSampleRate()));
                requestLatencyReport (fftProcessor.getDesignVersion(), 0);
            }
        }
        else if (prevMode == static_cast<int> (SubEQ::EQMode::ZeroLatency))
        {
            // Entering FIR from ZL: keep the IIR bridge at its current (zero)
            // latency until the plan is actually ready. The coordinator below
            // reports/gates only when the pending target is close.
            fftProcessor.resetConvolution();
            fftProcessor.requestRedesign();
        }
        else
        {
            // FIR -> FIR (e.g. Minimum <-> Linear): keep the active design
            // running until the new design and its latency transition complete.
            fftProcessor.invalidateDesigns();
        }
    }
    else if (lengthChanged && mode != SubEQ::EQMode::ZeroLatency)
    {
        // Structural length change: keep the active design / current bridge
        // latency until the replacement plan is near its adoption point.
        fftProcessor.invalidateDesigns();
    }
    else if (eqParamsChanged && mode != SubEQ::EQMode::ZeroLatency)
    {
        // Node parameter automation: do not invalidate the in-flight design;
        // let it publish, then request the next snapshot.
        fftProcessor.requestRedesign();
    }
    eqParamsChanged = false;
    lastMode = static_cast<int> (mode);

    // Input spectrum: analyze BEFORE processing (buffer still holds the raw
    // input at this point; EQ processing overwrites it in place). 频谱分析
    // 仅在编辑器存在时运行（spectrumEditorCount 由编辑器构造/析构维护）。
    if (spectrumEditorCount.load() > 0 && totalNumInputChannels > 0)
        inputAnalyzer.process (buffer.getReadPointer (0), buffer.getNumSamples());

    // Bounded latency coordinator. It starts only when the pending plan's
    // target is close; the old path stays audible for the whole warm-build
    // lead. If the target is missed or the transition exceeds the deadline,
    // abort the candidate and restore the active latency — never gate forever.
    const int designVersion = fftProcessor.getDesignVersion();
    const int publishedL = fftProcessor.getPublishedLatencySamples();
    const uint64_t pendingTarget = fftProcessor.getPendingTargetSample();
    const uint64_t committed = fftProcessor.getCommittedSampleCount();
    const int hostBlock = std::max (1, buffer.getNumSamples());
    const double srForGate = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    const int nearWindowSamples = static_cast<int> (
        std::max (0.10 * srForGate, 2.0 * hostBlock));
    const int transitionMarginSamples = static_cast<int> (
        std::max (0.12 * srForGate, 2.0 * hostBlock));
    const int transitionHardCapSamples = static_cast<int> (
        std::max (0.60 * srForGate, 8.0 * hostBlock));

    if (latencyAbortCooldownSamples > 0)
        latencyAbortCooldownSamples -= buffer.getNumSamples();

    const bool canTransition = (mode != SubEQ::EQMode::ZeroLatency)
                            && !isNonRealtime()
                            && publishedL > 0
                            && publishedL != iirPathLatency
                            && latencyAbortCooldownSamples <= 0
                            && latencyAbortVersion != designVersion;

    if (canTransition && pendingTarget > committed
        && (pendingTarget - committed) <= static_cast<uint64_t>(nearWindowSamples)
        && !latencyTransitionActive
        && latencyAbortVersion != designVersion)
    {
        latencyTransitionActive = true;
        latencyTransitionVersion = designVersion;
        latencyTransitionTargetLatency = publishedL;
        latencyTransitionElapsedSamples = 0;
        latencyTransitionTotalSamples = 0;
        latencyTransitionLastTarget = pendingTarget;
        latencyTransitionDeadlineSamples = static_cast<int> (
            std::min<uint64_t>(static_cast<uint64_t>(transitionHardCapSamples),
                               (pendingTarget - committed)
                                   + static_cast<uint64_t>(transitionMarginSamples)));
        latencyTransitionReported = false;
    }

    if (latencyTransitionActive)
    {
        latencyTransitionElapsedSamples += buffer.getNumSamples();
        latencyTransitionTotalSamples += buffer.getNumSamples();

        // The worker may publish a fresh warm target after a miss. Extend the
        // deadline for that target rather than aborting at the old one.
        if (pendingTarget != latencyTransitionLastTarget
            && pendingTarget > committed)
        {
            latencyTransitionLastTarget = pendingTarget;
            latencyTransitionElapsedSamples = 0;
            latencyTransitionDeadlineSamples = static_cast<int> (
                std::min<uint64_t>(static_cast<uint64_t>(transitionHardCapSamples),
                                   (pendingTarget - committed)
                                       + static_cast<uint64_t>(transitionMarginSamples)));
        }

        if (!latencyTransitionReported)
        {
            requestLatencyReport (latencyTransitionVersion,
                                  latencyTransitionTargetLatency);
            latencyTransitionReported = true;
        }

        const bool missed = pendingTarget == 0
                         || pendingTarget < committed;
        const bool expired = latencyTransitionElapsedSamples
                             > latencyTransitionDeadlineSamples
                          || latencyTransitionTotalSamples
                             > transitionHardCapSamples;
        if (missed || expired)
        {
            // Keep the design, ask the worker for a fresh warm target, restore
            // the active latency and open the gate. Report the old latency
            // with an invalid version so the timer cannot acknowledge the
            // still-pending design; it must be reported again when the new
            // target is near.
            fftProcessor.requestWarmRebuild();
            fftProcessor.invalidateLatencyAck();
            requestLatencyReport (0, iirPathLatency);
            latencyTransitionActive = false;
            latencyAbortVersion = -1;
            latencyAbortCooldownSamples = static_cast<int> (
                std::max (0.02 * srForGate, 1.0 * hostBlock));
            switchGateHoldSamples = static_cast<int> (
                std::max (1.0, 0.01 * srForGate));
        }
    }

    // Process audio based on current mode
    if (mode == SubEQ::EQMode::ZeroLatency)
    {
        // IIR node chain, in place, advanced in 32-sample sub-blocks so node
        // enable/coefficient ramps are sample-rate independent of host blocks.
        float* channelPtrs[SubEQ::EQNode::MaxChannels] = {};
        const int nch = std::min (totalNumInputChannels, SubEQ::EQNode::MaxChannels);
        for (int ch = 0; ch < nch; ++ch)
            channelPtrs[ch] = buffer.getWritePointer (ch);

        eqEngine.processChannels (channelPtrs, nch, buffer.getNumSamples());
        delayIirPath (buffer, nch, buffer.getNumSamples());
    }
    else
    {
        // FIR processing. While the first design is being built/warmed (or
        // waiting for PDC), process() returns false without touching the
        // buffer; keep the IIR path alive so switching to FIR never mutes the
        // plugin for the warm-up period.
        const bool wasActive = fftProcessor.hasActiveDesign();
        const bool firActive = fftProcessor.process (buffer);
        const int nch = std::min (totalNumInputChannels, SubEQ::EQNode::MaxChannels);

        // Update the bridge latency before the entry crossfade: both paths must
        // use the new active latency for the duration of the fade.
        const int activeL = fftProcessor.getLatencySamples();
        if (firActive && activeL > 0)
        {
            iirPathLatency = activeL;
            if (latencyTransitionActive
                && activeL == latencyTransitionTargetLatency)
            {
                latencyTransitionActive = false;
                switchGateHoldSamples = static_cast<int> (
                    std::max (1.0, 0.02 * (getSampleRate() > 0.0
                                               ? getSampleRate() : 48000.0)));
            }
        }

        if (!firActive)
        {
            float* channelPtrs[SubEQ::EQNode::MaxChannels] = {};
            for (int ch = 0; ch < nch; ++ch)
                channelPtrs[ch] = buffer.getWritePointer (ch);
            eqEngine.processChannels (channelPtrs, nch, buffer.getNumSamples());
            delayIirPath (buffer, nch, buffer.getNumSamples());
        }
        else
        {
            if (!wasActive && haveDryCopy && nch > 0)
                firEntryFadeRemaining = 1024;

            if (firEntryFadeRemaining > 0 && haveDryCopy && nch > 0
                && buffer.getNumSamples() <= transitionBuffer.getNumSamples())
            {
                // First FIR adoption: crossfade from the latency-aligned IIR
                // bridge to the new FIR state. Both paths have the same
                // nominal latency, so this is a pure response crossfade.
                for (int ch = 0; ch < nch; ++ch)
                    transitionBuffer.copyFrom (ch, 0, dryInput, ch, 0,
                                               buffer.getNumSamples());

                float* transitionPtrs[SubEQ::EQNode::MaxChannels] = {};
                for (int ch = 0; ch < nch; ++ch)
                    transitionPtrs[ch] = transitionBuffer.getWritePointer (ch);
                eqEngine.processChannels (transitionPtrs, nch, buffer.getNumSamples());
                delayIirPath (transitionBuffer, nch, buffer.getNumSamples());

                const int numSamples = buffer.getNumSamples();
                for (int n = 0; n < numSamples; ++n)
                {
                    const int rem = firEntryFadeRemaining - n;
                    const float firGain = (rem > 0)
                                              ? 1.0f - static_cast<float>(rem) / 1024.0f
                                              : 1.0f;
                    const float iirGain = 1.0f - firGain;
                    for (int ch = 0; ch < nch; ++ch)
                    {
                        const float firSample = buffer.getSample (ch, n);
                        const float iirSample = transitionBuffer.getSample (ch, n);
                        buffer.setSample (ch, n, firGain * firSample + iirGain * iirSample);
                    }
                }
                firEntryFadeRemaining = std::max (0, firEntryFadeRemaining - numSamples);
            }
        }
    }

    // PDC reporting: real-time latency transitions are reported by the
    // near-target coordinator above. Here we only handle Zero Latency and the
    // offline path (no gate/ack; report the target immediately).
    if (mode == SubEQ::EQMode::ZeroLatency)
    {
        if (reportedLatency.load() != 0)
            requestLatencyReport (designVersion, 0);
    }
    else if (isNonRealtime())
    {
        const int desiredLatency = publishedL > 0 ? publishedL : iirPathLatency;
        if (desiredLatency != reportedLatency.load()
            || designVersion != lastRequestedDesignVersion)
        {
            requestLatencyReport (designVersion, desiredLatency);
            lastRequestedDesignVersion = designVersion;
        }
    }

    // Post-EQ output stage: smoothed master gain and latency-compensated bypass.
    outputStage.setMasterDb (static_cast<double> (masterGainParam->load()));
    outputStage.setBypass (bypassParam->load() > 0.5f);
    outputStage.setLatency (mode == SubEQ::EQMode::ZeroLatency
                                ? 0 : iirPathLatency);

    float* outputPtrs[SubEQ::EQNode::MaxChannels] = {};
    const int nout = std::min (totalNumOutputChannels, SubEQ::EQNode::MaxChannels);
    for (int ch = 0; ch < nout; ++ch)
        outputPtrs[ch] = buffer.getWritePointer (ch);
    outputStage.process (haveDryCopy ? dryInput.getArrayOfReadPointers() : nullptr,
                         outputPtrs, nout, buffer.getNumSamples());

    // Mute through any interval where the host's compensated latency and the
    // audible path can disagree: a design with a new group delay is published
    // but not active yet (waiting for ack/guard), or we just left FIR and the
    // host still holds the old latency for a few timer ticks. The gate fades
    // 5 ms and holds silence for the handshake; it never lets mistimed audio
    // reach the host.
    {
        bool pending = latencyTransitionActive;
        if (!isNonRealtime() && switchGateHoldSamples > 0)
        {
            switchGateHoldSamples -= buffer.getNumSamples();
            pending = true;
        }
        if (isNonRealtime())
            switchGateHoldSamples = 0;

        const float target = pending ? 0.0f : 1.0f;
        const int numSamples = buffer.getNumSamples();
        for (int n = 0; n < numSamples; ++n)
        {
            if (switchGateGain < target)
                switchGateGain = std::min (target, switchGateGain + switchGateStep);
            else if (switchGateGain > target)
                switchGateGain = std::max (target, switchGateGain - switchGateStep);

            if (switchGateGain < 1.0f)
            {
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    buffer.setSample (ch, n, buffer.getSample (ch, n) * switchGateGain);
            }
        }
    }

    // Feed audio to the output spectrum analyzer (final post-EQ/output signal)
    if (spectrumEditorCount.load() > 0 && totalNumInputChannels > 0)
        spectrumAnalyzer.process (buffer.getReadPointer (0), buffer.getNumSamples());

    // Publish this block's raw input only after convolution has advanced.
    fftProcessor.publishHistory();
}

//==============================================================================
bool SubEQAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* SubEQAudioProcessor::createEditor()
{
    return new SubEQAudioProcessorEditor (*this);
}

//==============================================================================
void SubEQAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    // Explicit state version for future parameter-semantics migrations. A
    // missing attribute means a pre-0.5.0 session.
    xml->setAttribute ("stateVersion", 3);
    copyXmlToBinary (*xml, destData);
}

void SubEQAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState.get() == nullptr || ! xmlState->hasTagName (apvts.state.getType()))
        return;

    const int stateVersion = xmlState->getIntAttribute ("stateVersion", 1);

    // Collect parameter IDs present in the incoming tree. APVTS's
    // replaceState() creates missing parameter children from the *current*
    // instance values, which leaks values across sessions; missing IDs must be
    // explicitly reset to their parameter defaults instead.
    std::set<juce::String> presentIDs;
    std::function<void (const juce::XmlElement&)> collectIDs =
        [&] (const juce::XmlElement& element)
        {
            if (element.hasTagName ("PARAM"))
            {
                const auto id = element.getStringAttribute ("id");
                if (id.isNotEmpty())
                    presentIDs.insert (id);
            }
            for (auto* child = element.getFirstChildElement(); child != nullptr;
                 child = child->getNextElement())
                collectIDs (*child);
        };
    collectIDs (*xmlState);

    apvts.replaceState (juce::ValueTree::fromXml (*xmlState));

    // `setValue` does not notify APVTS's ParameterAdapter (only
    // `setValueNotifyingHost` does), so using it here would leave the audio
    // thread and GUI reading the previous session's value. Missing parameters
    // must go through the notifying path to reach the APVTS atomic and tree.
    for (auto* parameter : getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter);
        if (ranged == nullptr)
            continue;
        if (presentIDs.find (ranged->paramID) == presentIDs.end())
            ranged->setValueNotifyingHost (ranged->getDefaultValue());
    }

    if (stateVersion < 2)
    {
        // v1: spectrum_fft_size used the pre-0.4.0 "FFT size" semantics; the
        // parameter is now a resolution choice and its index is retained.
    }

    // Do not write modeChanged/eqParamsChanged/lastRequestedDesignVersion from
    // the message thread: those are audio-thread caches. updateEQParameters()
    // detects the parameter differences on the next block and applies the
    // change without an unconditional biquad reset/gate.
    stateEpoch.fetch_add (1, std::memory_order_release);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SubEQAudioProcessor();
}
