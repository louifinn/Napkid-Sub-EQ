/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

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

    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        nodeParams[i][0] = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Freq));
        nodeParams[i][1] = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Gain));
        nodeParams[i][2] = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Q));
        nodeParams[i][3] = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Type));
        nodeParams[i][4] = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Enabled));
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
    setLatencySamples (0);  // IIR biquads: zero processing latency
    eqEngine.prepare (sampleRate, samplesPerBlock);
    eqEngine.reset();
    fftProcessor.prepare (sampleRate, samplesPerBlock, getTotalNumInputChannels());
    fftProcessor.setParameterSource (&apvts);
    fftProcessor.reset();
    spectrumAnalyzer.prepare (sampleRate);
    currentMode.store (static_cast<int> (SubEQ::EQMode::ZeroLatency));
    reportedLatency.store (0);
    eqModeCache = 0;
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

    // Update each node
    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        float freq   = nodeParams[i][0]->load();
        float gain   = nodeParams[i][1]->load();
        float qVal   = nodeParams[i][2]->load();
        int type     = static_cast<int> (nodeParams[i][3]->load());
        bool enabled = nodeParams[i][4]->load() > 0.5f;

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

            auto& node = eqEngine.getNode (i);
            node.setEnabled (enabled);

            if (enabled)
            {
                node.update (static_cast<double> (freq),
                             static_cast<double> (gain),
                             static_cast<double> (qVal),
                             SubEQ::intToFilterType (type));
            }

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
            // Back to pure IIR: report zero latency immediately
            if (reportedLatency.load() != 0)
            {
                reportedLatency.store (0);
                setLatencySamples (0);
            }
        }
        else
        {
            // Request an asynchronous FIR redesign. Latency is reported below
            // (after the design is published) so that PDC always matches the
            // coefficients actually in use — no premature compensation.
            fftProcessor.requestRedesign();
        }
    }
    else if (eqParamsChanged && mode != SubEQ::EQMode::ZeroLatency)
    {
        // In FIR mode, redesign FIR only when EQ parameters actually changed
        fftProcessor.requestRedesign();
    }
    eqParamsChanged = false;

    // Pick up a freshly published FIR design and report its latency (PDC)
    if (mode != SubEQ::EQMode::ZeroLatency)
    {
        int newLatency = fftProcessor.getLatencySamples();
        if (newLatency != reportedLatency.load())
        {
            reportedLatency.store (newLatency);
            setLatencySamples (newLatency);
        }
    }

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

    // Feed audio to spectrum analyzer
    if (totalNumInputChannels > 0)
    {
        auto* inputData = buffer.getReadPointer (0);
        spectrumAnalyzer.process (inputData, buffer.getNumSamples());
    }
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
