/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

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
        const int desired = reportedLatency.load();
        setLatencySamples (desired);
        fftProcessor.acknowledgeLatency (pendingLatencyVersion.load());
    }
}

void SubEQAudioProcessor::writeIirHistory (const juce::AudioBuffer<float>& buffer,
                                           int nch, int numSamples) noexcept
{
    if (iirHistory.empty() || iirHistorySize <= 0)
        return;

    const int channels = std::min (nch, iirHistoryChannels);
    for (int n = 0; n < numSamples; ++n)
    {
        for (int ch = 0; ch < channels; ++ch)
            iirHistory[static_cast<size_t>(ch) * static_cast<size_t>(iirHistorySize)
                       + static_cast<size_t>(iirHistoryWrite)] =
                buffer.getReadPointer (ch)[n];

        if (++iirHistoryWrite == iirHistorySize)
            iirHistoryWrite = 0;
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

    return static_cast<double> (fftProcessor.getTailLengthSamples()) / sr;
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
void SubEQAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // PDC 不再在音频线程直接调用 setLatencySamples()（音频线程禁止）：延迟上报
    // 统一由 processBlock 置脏标记、timerCallback 在消息线程执行；同会话重入
    // prepare（transport stop/start）时已发布设计与延迟上报均被保留，无需动作。
    eqEngine.prepare (sampleRate);
    eqEngine.reset();
    outputStage.prepare (sampleRate, SubEQ::FFTProcessor::MaxLatencySamples,
                         std::max (1, getTotalNumInputChannels()));
    outputStage.setMasterDb (static_cast<double> (masterGainParam->load()));
    outputStage.setBypass (bypassParam->load() > 0.5f);
    outputStage.snapToTargets();
    dryInput.setSize (std::max (1, getTotalNumInputChannels()),
                      std::max (samplesPerBlock, 8192));
    transitionBuffer.setSize (std::max (1, getTotalNumInputChannels()),
                              std::max (samplesPerBlock, 8192));
    iirHistorySize = SubEQ::FFTProcessor::MaxLatencySamples + 1;
    iirHistoryChannels = std::max (1, getTotalNumInputChannels());
    iirHistory.assign (static_cast<size_t>(iirHistoryChannels)
                           * static_cast<size_t>(iirHistorySize), 0.0f);
    iirHistoryWrite = 0;
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
    updateEQParameters();

    if (spectrumDisplayResetRequested.exchange (false, std::memory_order_acquire))
    {
        spectrumAnalyzer.resetDisplayState();
        inputAnalyzer.resetDisplayState();
    }

    const auto mode = static_cast<SubEQ::EQMode> (currentMode.load());

    // Handle mode change (latency reporting + FIR redesign)
    if (modeChanged)
    {
        modeChanged = false;

        // Flush the FIR pipeline on any mode switch: stale input blocks /
        // overlap tails from a previous FIR session must never convolve with
        // new coefficients (audible echoes/transients). The raw input history
        // is preserved so a later FIR entry can warm from past audio.
        fftProcessor.resetConvolution();

        if (mode == SubEQ::EQMode::ZeroLatency)
        {
            // The IIR chain did not run while FIR was active; its biquad states
            // are frozen at the moment of leaving ZL. Reset them so the first
            // ZL block cannot inject stale low-frequency energy.
            eqEngine.reset();

            // Back to pure IIR: report zero latency (deferred to the message
            // thread — setLatencySamples must not run on the audio thread)
            if (reportedLatency.load() != 0)
            {
                reportedLatency.store (0);
                latencyDirty.store (true, std::memory_order_release);
            }
        }
        else
        {
            // 进入 FIR 模式：丢弃先前发布的（可能已过期的）设计，使输出在
            // 新设计发布前保持静音（与下方“未发布前输出静音”的契约一致），
            // 而不是短暂沿用旧曲线。延迟在上报处随发布更新，PDC 始终匹配
            // 实际生效的系数。
            fftProcessor.clearPublishedState();
            fftProcessor.requestRedesign();
        }
    }
    else if (eqParamsChanged && mode != SubEQ::EQMode::ZeroLatency)
    {
        // In FIR mode, redesign FIR only when EQ parameters actually changed
        fftProcessor.requestRedesign();
    }
    eqParamsChanged = false;

    // Input spectrum: analyze BEFORE processing (buffer still holds the raw
    // input at this point; EQ processing overwrites it in place). 频谱分析
    // 仅在编辑器存在时运行（spectrumEditorCount 由编辑器构造/析构维护）。
    if (spectrumEditorCount.load() > 0 && totalNumInputChannels > 0)
        inputAnalyzer.process (buffer.getReadPointer (0), buffer.getNumSamples());

    // Process audio based on current mode
    if (mode == SubEQ::EQMode::ZeroLatency)
    {
        firEntryFadeRemaining = 0;

        // IIR node chain, in place, advanced in 32-sample sub-blocks so node
        // enable/coefficient ramps are sample-rate independent of host blocks.
        float* channelPtrs[SubEQ::EQNode::MaxChannels] = {};
        const int nch = std::min (totalNumInputChannels, SubEQ::EQNode::MaxChannels);
        for (int ch = 0; ch < nch; ++ch)
            channelPtrs[ch] = buffer.getWritePointer (ch);

        eqEngine.processChannels (channelPtrs, nch, buffer.getNumSamples());
        writeIirHistory (buffer, nch, buffer.getNumSamples());
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

        if (!firActive)
        {
            float* channelPtrs[SubEQ::EQNode::MaxChannels] = {};
            for (int ch = 0; ch < nch; ++ch)
                channelPtrs[ch] = buffer.getWritePointer (ch);
            eqEngine.processChannels (channelPtrs, nch, buffer.getNumSamples());
            writeIirHistory (buffer, nch, buffer.getNumSamples());
            firEntryFadeRemaining = 0;
        }
        else
        {
            if (!wasActive && haveDryCopy)
                firEntryFadeRemaining = 1024;

            if (firEntryFadeRemaining > 0 && haveDryCopy && nch > 0)
            {
                // Crossfade the newly active FIR output against the IIR path
                // on the same raw input. The IIR state advances normally (it is
                // reset if we later return to Zero Latency).
                for (int ch = 0; ch < nch; ++ch)
                    transitionBuffer.copyFrom (ch, 0, dryInput, ch, 0,
                                               buffer.getNumSamples());

                float* transitionPtrs[SubEQ::EQNode::MaxChannels] = {};
                for (int ch = 0; ch < nch; ++ch)
                    transitionPtrs[ch] = transitionBuffer.getWritePointer (ch);
                eqEngine.processChannels (transitionPtrs, nch, buffer.getNumSamples());

                const int numSamples = buffer.getNumSamples();
                const int latency = std::max (0, fftProcessor.getLatencySamples());
                const int ringSize = iirHistorySize;
                const bool haveRing = ringSize > 0 && !iirHistory.empty();

                for (int n = 0; n < numSamples; ++n)
                {
                    const int rem = firEntryFadeRemaining - n;
                    const float firGain = (rem > 0)
                                              ? 1.0f - static_cast<float>(rem) / 1024.0f
                                              : 1.0f;
                    const float iirGain = 1.0f - firGain;
                    const int readIndex = (haveRing && latency > 0)
                                              ? (iirHistoryWrite - latency + ringSize) % ringSize
                                              : -1;

                    for (int ch = 0; ch < nch; ++ch)
                    {
                        float* dst = buffer.getWritePointer (ch);
                        const float iirCurrent = transitionBuffer.getReadPointer (ch)[n];
                        const bool ringCh = haveRing && ch < iirHistoryChannels;
                        const float iirAligned = (readIndex >= 0 && ringCh)
                                                     ? iirHistory[static_cast<size_t>(ch)
                                                                  * static_cast<size_t>(ringSize)
                                                                  + static_cast<size_t>(readIndex)]
                                                     : iirCurrent;

                        if (ringCh)
                            iirHistory[static_cast<size_t>(ch) * static_cast<size_t>(ringSize)
                                       + static_cast<size_t>(iirHistoryWrite)] = iirCurrent;

                        dst[n] = firGain * dst[n] + iirGain * iirAligned;
                    }

                    if (haveRing && ++iirHistoryWrite == ringSize)
                        iirHistoryWrite = 0;
                }
                firEntryFadeRemaining = std::max (0, firEntryFadeRemaining - numSamples);
            }
        }
    }

    // PDC handshake: report the *published* design's latency first. The audio
    // path keeps the old design until the message thread has called
    // setLatencySamples() and acknowledged that version.
    {
        int desiredLatency = reportedLatency.load();
        if (mode == SubEQ::EQMode::ZeroLatency)
        {
            desiredLatency = 0;
        }
        else
        {
            // No published design yet (e.g. just entered FIR): keep the last
            // reported latency. Reporting 0 and then L would make the host
            // bounce its compensation twice; the first FIR design will update
            // this as soon as its package is published.
            const int published = fftProcessor.getPublishedLatencySamples();
            if (published > 0)
                desiredLatency = published;
        }

        const int designVersion = fftProcessor.getDesignVersion();
        if (desiredLatency != reportedLatency.load()
            || designVersion != lastRequestedDesignVersion)
        {
            reportedLatency.store (desiredLatency);
            pendingLatencyVersion.store (designVersion);
            latencyDirty.store (true, std::memory_order_release);
            lastRequestedDesignVersion = designVersion;
        }
    }

    // Post-EQ output stage: smoothed master gain and latency-compensated bypass.
    outputStage.setMasterDb (static_cast<double> (masterGainParam->load()));
    outputStage.setBypass (bypassParam->load() > 0.5f);
    outputStage.setLatency (mode == SubEQ::EQMode::ZeroLatency
                                ? 0 : fftProcessor.getLatencySamples());

    float* outputPtrs[SubEQ::EQNode::MaxChannels] = {};
    const int nout = std::min (totalNumOutputChannels, SubEQ::EQNode::MaxChannels);
    for (int ch = 0; ch < nout; ++ch)
        outputPtrs[ch] = buffer.getWritePointer (ch);
    outputStage.process (haveDryCopy ? dryInput.getArrayOfReadPointers() : nullptr,
                         outputPtrs, nout, buffer.getNumSamples());

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
    xml->setAttribute ("stateVersion", 2);
    copyXmlToBinary (*xml, destData);
}

void SubEQAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState.get() != nullptr)
        if (xmlState->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SubEQAudioProcessor();
}
