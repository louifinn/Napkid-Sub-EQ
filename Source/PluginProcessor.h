/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#pragma once

#include <atomic>
#include <JuceHeader.h>
#include "SubEQ_Core.h"
#include "SubEQ_Parameters.h"
#include "SubEQ_FFTProcessor.h"
#include "SubEQ_Spectrum.h"

//==============================================================================
class SubEQAudioProcessor  : public juce::AudioProcessor
{
public:
    //==============================================================================
    SubEQAudioProcessor();
    ~SubEQAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }
    SubEQ::EQEngine& getEQEngine() { return eqEngine; }
    const SubEQ::EQEngine& getEQEngine() const { return eqEngine; }

    // Synchronize EQ engine state from APVTS parameters (call from GUI thread)
    void updateEQParameters();

    // Spectrum analyzer access
    SubEQ::SpectrumAnalyzer& getSpectrumAnalyzer() { return spectrumAnalyzer; }
    const SubEQ::SpectrumAnalyzer& getSpectrumAnalyzer() const { return spectrumAnalyzer; }

    // Current EQ mode (atomic: written by audio thread, read by GUI thread)
    SubEQ::EQMode getCurrentMode() const { return static_cast<SubEQ::EQMode> (currentMode.load()); }
    int getCurrentLatencySamples() const { return reportedLatency.load(); }

private:

    SubEQ::EQEngine eqEngine;
    // apvts must be declared BEFORE fftProcessor: members are destroyed in
    // reverse declaration order, and ~FFTProcessor joins its background
    // thread (which reads apvts via setParameterSource) — apvts must outlive
    // the FIR designer thread.
    juce::AudioProcessorValueTreeState apvts;
    SubEQ::FFTProcessor fftProcessor;
    SubEQ::SpectrumAnalyzer spectrumAnalyzer;

    // Parameter cache to avoid redundant coefficient updates
    struct NodeParamCache
    {
        float freq = 100.0f;
        float gain = 0.0f;
        float q = 0.707f;
        int type = 0;
        bool enabled = false;
    };

    NodeParamCache nodeCache[SubEQ::NumNodes];
    float masterGainCache = 0.0f;
    bool bypassCache = false;
    int eqModeCache = 0;
    int firLengthCache = 0;

    // Cached atomic parameter pointers (avoid per-block string lookups).
    // Index order matches ParamID: Freq, Gain, Q, Type, Enabled.
    std::atomic<float>* masterGainParam = nullptr;
    std::atomic<float>* bypassParam = nullptr;
    std::atomic<float>* eqModeParam = nullptr;
    std::atomic<float>* firLengthParam = nullptr;
    std::atomic<float>* nodeParams[SubEQ::NumNodes][5] = {};
    std::atomic<int> reportedLatency { 0 };
    std::atomic<int> currentMode { static_cast<int> (SubEQ::EQMode::ZeroLatency) };
    bool modeChanged = false;
    bool eqParamsChanged = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubEQAudioProcessor)
};
