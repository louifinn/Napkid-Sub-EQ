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
class SubEQAudioProcessor  : public juce::AudioProcessor,
                             private juce::Timer
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

    // Synchronize EQ engine state from APVTS parameters (audio thread only —
    // writes eqEngine and the non-atomic caches; never call from the GUI thread)
    void updateEQParameters();

    // Spectrum analyzer access
    SubEQ::SpectrumAnalyzer& getSpectrumAnalyzer() { return spectrumAnalyzer; }
    const SubEQ::SpectrumAnalyzer& getSpectrumAnalyzer() const { return spectrumAnalyzer; }
    SubEQ::SpectrumAnalyzer& getInputSpectrumAnalyzer() { return inputAnalyzer; }
    const SubEQ::SpectrumAnalyzer& getInputSpectrumAnalyzer() const { return inputAnalyzer; }

    // Current EQ mode (atomic: written by audio thread, read by GUI thread)
    SubEQ::EQMode getCurrentMode() const { return static_cast<SubEQ::EQMode> (currentMode.load()); }
    int getCurrentLatencySamples() const { return reportedLatency.load(); }

    // 频谱分析门控（GUI 线程计数、音频线程读取）：至少一个编辑器存在时才
    // 运行分析，避免无 GUI 时持续消耗音频线程 CPU；引用计数保证多编辑器
    // 并存时不会因其中一个关闭而误停分析。
    void spectrumEditorOpened() noexcept { spectrumEditorCount.fetch_add (1); }
    void spectrumEditorClosed() noexcept { spectrumEditorCount.fetch_sub (1); }

private:
    // juce::Timer (message thread, 10 Hz): deferred latency (PDC) reporting.
    // setLatencySamples() -> updateHostDisplay() allocates and takes the
    // message queue lock, so the audio thread only sets latencyDirty and this
    // callback performs the actual host notification.
    void timerCallback() override;

    SubEQ::EQEngine eqEngine;
    // apvts must be declared BEFORE fftProcessor: members are destroyed in
    // reverse declaration order, and ~FFTProcessor joins its background
    // thread (which reads apvts via setParameterSource) — apvts must outlive
    // the FIR designer thread.
    juce::AudioProcessorValueTreeState apvts;
    SubEQ::FFTProcessor fftProcessor;
    SubEQ::SpectrumAnalyzer spectrumAnalyzer;   // output (post-EQ) spectrum
    SubEQ::SpectrumAnalyzer inputAnalyzer;      // input (pre-EQ) spectrum

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

    // Spectrum analyzer config cache (indexes into the APVTS choice params)
    int spectrumFftCache = 1;      // 0=4096, 1=8192, 2=16384
    int spectrumDensityCache = 0;  // 0=1/6 oct (61), 1=1/12 oct (121)
    int spectrumHopCache = 0;      // 0=512, 1=1024, 2=2048

    // Cached atomic parameter pointers (avoid per-block string lookups).
    // Index order matches ParamID: Freq, Gain, Q, Type, Enabled.
    std::atomic<float>* masterGainParam = nullptr;
    std::atomic<float>* bypassParam = nullptr;
    std::atomic<float>* eqModeParam = nullptr;
    std::atomic<float>* firLengthParam = nullptr;
    std::atomic<float>* spectrumFftParam = nullptr;
    std::atomic<float>* spectrumDensityParam = nullptr;
    std::atomic<float>* spectrumHopParam = nullptr;
    std::atomic<float>* nodeParams[SubEQ::NumNodes][5] = {};
    std::atomic<int> reportedLatency { 0 };
    std::atomic<int> currentMode { static_cast<int> (SubEQ::EQMode::ZeroLatency) };
    std::atomic<int> spectrumEditorCount { 0 };   // 存活的编辑器数
    std::atomic<bool> latencyDirty { false };   // audio thread -> timer: report PDC
    bool modeChanged = false;
    bool eqParamsChanged = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubEQAudioProcessor)
};
