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
    spectrumDensityParam = apvts.getRawParameterValue ("spectrum_band_density");
    spectrumHopParam = apvts.getRawParameterValue ("spectrum_hop_size");

    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        nodeParams[i][kParamFreq] = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Freq));
        nodeParams[i][kParamGain] = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Gain));
        nodeParams[i][kParamQ] = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Q));
        nodeParams[i][kParamType] = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Type));
        nodeParams[i][kParamEnabled] = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Enabled));
    }

    // Deferred PDC reporting poll (message thread) — see timerCallback
    startTimerHz (10);
}

void SubEQAudioProcessor::timerCallback()
{
    // The audio thread sets latencyDirty instead of calling setLatencySamples()
    // directly: setLatencySamples -> updateHostDisplay allocates and takes the
    // message queue lock, which must not happen on the audio thread.
    if (latencyDirty.exchange (false))
        setLatencySamples (reportedLatency.load());
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
    const auto mode = static_cast<SubEQ::EQMode> (currentMode.load());
    if (mode == SubEQ::EQMode::ZeroLatency)
        return 0.0;

    const double sr = getSampleRate();
    if (sr <= 0.0)
        return 0.0;

    return static_cast<double> (fftProcessor.getTailLengthSamples()) / sr;
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
    eqEngine.prepare (sampleRate, samplesPerBlock);
    eqEngine.reset();
    fftProcessor.prepare (sampleRate, samplesPerBlock, getTotalNumInputChannels());
    fftProcessor.setParameterSource (&apvts);
    fftProcessor.reset();
    spectrumAnalyzer.prepare (sampleRate);
    inputAnalyzer.prepare (sampleRate);
    // 不强制 currentMode/eqModeCache 归零：同会话重复 prepare 时 eq_mode
    // 参数未变，若清空缓存，updateEQParameters 会误报“模式变化”，触发一次
    // 多余的 FIR 重设计并在发布时冲刷卷积尾（播放中出现输出跳变）。仅当
    // 参数真正变化时才由 updateEQParameters 置 modeChanged。
    modeChanged = false;
    eqParamsChanged = false;
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
    // Update master gain (use epsilon to avoid spurious updates from float jitter)
    float masterGain = masterGainParam->load();
    if (std::abs (masterGain - masterGainCache) > 0.001f)
    {
        masterGainCache = masterGain;
        eqEngine.setMasterGain (static_cast<double> (masterGain));
        eqParamsChanged = true;
    }

    // Update bypass
    bool bypass = bypassParam->load() > 0.5f;
    if (bypass != bypassCache)
    {
        bypassCache = bypass;
        eqEngine.setBypass (bypass);
        eqParamsChanged = true;
    }

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
    // analysers only write pre-allocated buffers and plain integers)
    int spectrumFft = static_cast<int> (spectrumFftParam->load());
    int spectrumDensity = static_cast<int> (spectrumDensityParam->load());
    int spectrumHop = static_cast<int> (spectrumHopParam->load());
    if (spectrumFft != spectrumFftCache
        || spectrumDensity != spectrumDensityCache
        || spectrumHop != spectrumHopCache)
    {
        spectrumFftCache = spectrumFft;
        spectrumDensityCache = spectrumDensity;
        spectrumHopCache = spectrumHop;

        int fftOrder = SubEQ::spectrumFftChoiceToOrder (spectrumFft);
        int numBands = SubEQ::spectrumDensityChoiceToBands (spectrumDensity);
        int hop = SubEQ::spectrumHopChoiceToSamples (spectrumHop);

        spectrumAnalyzer.configure (fftOrder, numBands, hop);
        inputAnalyzer.configure (fftOrder, numBands, hop);
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

    // Update EQ parameters before processing
    updateEQParameters();

    // Advance coefficient interpolation once per block (shared by all channels)
    eqEngine.advanceSmoothing (buffer.getNumSamples());

    const auto mode = static_cast<SubEQ::EQMode> (currentMode.load());

    // Handle mode change (latency reporting + FIR redesign)
    if (modeChanged)
    {
        modeChanged = false;

        // Flush the FIR pipeline on any mode switch: stale input blocks /
        // overlap tails from a previous FIR session must never convolve with
        // new coefficients (audible echoes/transients).
        fftProcessor.reset();

        if (mode == SubEQ::EQMode::ZeroLatency)
        {
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

    // Pick up a freshly published FIR design and report its latency (PDC).
    // The actual setLatencySamples() call is deferred to timerCallback on the
    // message thread (it allocates/locks inside updateHostDisplay).
    if (mode != SubEQ::EQMode::ZeroLatency)
    {
        int newLatency = fftProcessor.getLatencySamples();
        if (newLatency != reportedLatency.load())
        {
            reportedLatency.store (newLatency);
            latencyDirty.store (true, std::memory_order_release);
        }
    }

    // Input spectrum: analyze BEFORE processing (buffer still holds the raw
    // input at this point; EQ processing overwrites it in place). 频谱分析
    // 仅在编辑器存在时运行（spectrumEditorCount 由编辑器构造/析构维护）。
    if (spectrumEditorCount.load() > 0 && totalNumInputChannels > 0)
        inputAnalyzer.process (buffer.getReadPointer (0), buffer.getNumSamples());

    // Process audio based on current mode
    if (mode == SubEQ::EQMode::ZeroLatency)
    {
        // IIR biquad processing
        for (int channel = 0; channel < totalNumInputChannels; ++channel)
        {
            auto* inputData = buffer.getReadPointer (channel);
            auto* outputData = buffer.getWritePointer (channel);
            eqEngine.processChannel (inputData, outputData, buffer.getNumSamples(), channel);
        }
    }
    else
    {
        // FIR processing. Until a design is published, output silence rather
        // than an unprocessed passthrough: with PDC still 0, a passthrough
        // would be latency-mismatched against the compensated signal.
        if (fftProcessor.isReady())
            fftProcessor.process (buffer);
        else
            buffer.clear();
    }

    // Feed audio to the output spectrum analyzer (post-EQ signal)
    if (spectrumEditorCount.load() > 0 && totalNumInputChannels > 0)
        spectrumAnalyzer.process (buffer.getReadPointer (0), buffer.getNumSamples());
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
