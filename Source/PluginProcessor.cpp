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
    return 0.0;
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
    spectrumAnalyzer.prepare (sampleRate);
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
    float masterGain = apvts.getRawParameterValue ("master_gain")->load();
    if (std::abs (masterGain - masterGainCache) > 0.001f)
    {
        masterGainCache = masterGain;
        eqEngine.setMasterGain (static_cast<double> (masterGain));
    }

    // Update bypass
    bool bypass = apvts.getRawParameterValue ("bypass")->load() > 0.5f;
    if (bypass != bypassCache)
    {
        bypassCache = bypass;
        eqEngine.setBypass (bypass);
    }

    // Update each node
    for (int i = 0; i < SubEQ::NumNodes; ++i)
    {
        float freq = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Freq))->load();
        float gain = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Gain))->load();
        float qVal = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Q))->load();
        int type = static_cast<int> (apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Type))->load());
        bool enabled = apvts.getRawParameterValue (SubEQ::getNodeParamID (i, SubEQ::ParamID::Enabled))->load() > 0.5f;

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

    // Process each channel (each channel has independent Biquad state)
    for (int channel = 0; channel < totalNumInputChannels; ++channel)
    {
        auto* inputData = buffer.getReadPointer (channel);
        auto* outputData = buffer.getWritePointer (channel);

        eqEngine.processChannel (inputData, outputData, buffer.getNumSamples(), channel);
    }

    // Feed audio to spectrum analyzer (use input channel 0) — AFTER IIR processing
    // to avoid FFT blocking the audio thread's IIR processing deadline
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
