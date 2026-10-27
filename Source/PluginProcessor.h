/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#pragma once

#include <atomic>
#include <JuceHeader.h>
#include "SubEQ_Core.h"
#include "SubEQ_OutputStage.h"
#include "SubEQ_Parameters.h"
#include "SubEQ_FFTProcessor.h"
#include "SubEQ_LatencyCoordinator.h"
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

    // Synchronize EQ engine state from APVTS parameters. Audio thread, or the
    // prepareToPlay context where the host has stopped processing (JUCE does
    // not guarantee prepareToPlay runs on the audio thread) — writes eqEngine
    // and the non-atomic caches; never call from the GUI thread (F-009).
    void updateEQParameters();

    // Spectrum analyzer access
    SubEQ::SpectrumAnalyzer& getSpectrumAnalyzer() { return spectrumAnalyzer; }
    const SubEQ::SpectrumAnalyzer& getSpectrumAnalyzer() const { return spectrumAnalyzer; }
    SubEQ::SpectrumAnalyzer& getInputSpectrumAnalyzer() { return inputAnalyzer; }
    const SubEQ::SpectrumAnalyzer& getInputSpectrumAnalyzer() const { return inputAnalyzer; }

    // Current EQ mode (atomic: written by audio thread, read by GUI thread)
    SubEQ::EQMode getCurrentMode() const { return static_cast<SubEQ::EQMode> (currentMode.load()); }
    // Actual output-path latency (GUI chip). This is the host-facing latency
    // currently used by the IIR bridge / active FIR path. The VST3 editor can
    // live on an edit-controller instance that never processes audio; only in
    // that never-processed case fall back to the selected mode/FIR nominal
    // latency. A processing instance always reports the real (possibly 0
    // during warm-up/abort) value.
    int getCurrentLatencySamples() const
    {
        const int reported = reportedLatency.load();
        if (reported > 0)
            return reported;
        return hasProcessedAudio.load(std::memory_order_acquire)
                   ? 0
                   : nominalLatencySamples();
    }

    // Audio-thread mirror updated in processBlock; safe for the GUI to read.
    bool isFirActive() const noexcept { return firActiveMirror.load(std::memory_order_acquire); }

    // GUI: 1024-point immutable FIR response table for the active design.
    bool getFirResponseSnapshot(SubEQ::FFTProcessor::FirResponseSnapshot& out,
                                int version) const;

#if SUBEQ_TESTING
    // Integration-test hooks: compiled only into the test translation unit,
    // never into the published plugin interface.
    void processDeferredLatencyForTest() { timerCallback(); }
    bool isFirActiveForTest() const { return isFirActive(); }
    uint64_t getPendingTargetForTest() const { return fftProcessor.getPendingTargetSample(); }
    uint64_t getCommittedForTest() const { return fftProcessor.getCommittedSampleCount(); }
    int getIirPathLatencyForTest() const { return iirPathLatency; }
    bool isLatencyTransitionForTest() const { return latencyCoordinator.isActive(); }
    int getWarmVersionForTest() const { return fftProcessor.getWarmVersionForTest(); }
    uint64_t getWarmTargetForTest() const { return fftProcessor.getWarmTargetForTest(); }
    int getAudioLocalVersionForTest() const { return fftProcessor.getAudioLocalVersionForTest(); }
    int getAudioLocalWarmVersionForTest() const { return fftProcessor.getAudioLocalWarmVersionForTest(); }
#endif

    // 已发布 FIR 设计的版本号：GUI 据此在后台设计异步发布后自动重绘相位曲线。
    int getFirDesignVersion() const;

    // 实际接管音频路径的 FIR 设计版本（0 = 无）；GUI 用它匹配响应快照。
    int getActiveFirDesignVersion() const { return fftProcessor.getActiveDesignVersion(); }

    // 频谱分析门控（GUI 线程计数、音频线程读取）：至少一个编辑器存在时才
    // 运行分析，避免无 GUI 时持续消耗音频线程 CPU；引用计数保证多编辑器
    // 并存时不会因其中一个关闭而误停分析。
    void spectrumEditorOpened() noexcept
    {
        if (spectrumEditorCount.fetch_add (1) == 0)
        {
            // Clear the published snapshot immediately on the GUI thread so a
            // stopped host cannot show the previous session's low-frequency
            // data; the audio-thread flag still resets rings/envelopes.
            spectrumAnalyzer.resetPublishedDisplay();
            inputAnalyzer.resetPublishedDisplay();
            spectrumDisplayResetRequested.store (true, std::memory_order_release);
        }
    }
    void spectrumEditorClosed() noexcept { spectrumEditorCount.fetch_sub (1); }

private:
    // juce::Timer (message thread, 30 Hz): deferred latency (PDC) reporting.
    // setLatencySamples() -> updateHostDisplay() allocates and takes the
    // message queue lock, so the audio thread only sets latencyDirty and this
    // callback performs the actual host notification.
    void timerCallback() override;
    int nominalLatencySamples() const noexcept;
    void delayIirPath(juce::AudioBuffer<float>& buffer, int nch, int numSamples) noexcept;
    void requestLatencyReport(int version, int latency) noexcept;

    SubEQ::EQEngine eqEngine;
    SubEQ::OutputStage outputStage;
    juce::AudioBuffer<float> dryInput;   // pre-EQ input for latency-compensated bypass
    juce::AudioBuffer<float> transitionBuffer;   // IIR path for the FIR entry crossfade
    std::vector<float> iirHistory;       // IIR output history for time-aligned FIR entry
    int iirHistoryWrite = 0;
    int iirHistorySize = 0;
    int iirHistoryChannels = 0;
    int iirPathLatency = 0;              // IIR bridge delay = host-facing latency
    int firEntryFadeRemaining = 0;
    // Output gate used while a latency-changing design waits for host PDC
    // acknowledgement. Muting through the transition is the only way to keep
    // the host's compensation and the audible path time-aligned.
    float switchGateGain = 1.0f;
    float switchGateStep = 1.0f;
    int switchGateHoldSamples = 0;
    // Bounded latency-transition coordinator (pure header, offline-tested).
    // It only starts when the pending plan's target is close, never for the
    // whole warm-build lead.
    SubEQ::LatencyCoordinator latencyCoordinator;
    int lastMode = 0;
    // apvts must be declared BEFORE fftProcessor: members are destroyed in
    // reverse declaration order, and ~FFTProcessor joins its background
    // thread (which reads apvts via setParameterSource) — apvts must outlive
    // the FIR designer thread.
    juce::AudioProcessorValueTreeState apvts;
    SubEQ::FFTProcessor fftProcessor;
    std::shared_ptr<const SubEQ::VQTKernelBank> vqtKernelBank;  // shared by both analyzers
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
    int eqModeCache = 0;
    int firLengthCache = 0;

    // 频谱设置（把 APVTS 的五个 choice 下标打包成一个整体：比较与解码都只做
    // 一次，解码器见 SubEQ_SpectrumConfig.h）
    struct SpectrumSettings
    {
        int resolution = 1;   // 0=Fast, 1=Balanced, 2=Fine（VQT 核长尺度）
        int window = 2;       // 0=Hann, 1=Blackman, 2=B-Harris, 3=Nuttall
        int density = 0;      // 0=1/6 oct (61), 1=1/12 oct (121)
        int hop = 0;          // 0=512, 1=1024, 2=2048
        int smoothing = 0;    // 0=Off, 1=1/6 oct, 2=1/3 oct, 3=2/3 oct（v0.5.0 default Off）

        bool operator== (const SpectrumSettings& other) const noexcept
        {
            return resolution == other.resolution
                && window == other.window
                && density == other.density
                && hop == other.hop
                && smoothing == other.smoothing;
        }
        bool operator!= (const SpectrumSettings& other) const noexcept { return ! (*this == other); }
    };

    // Sentinel: forces the first updateEQParameters() call to configure the
    // analyzers with the real APVTS defaults (not the struct's convenience
    // defaults, which previously left smoothRadius at 1 while the UI said Off).
    SpectrumSettings spectrumSettings { -1, -1, -1, -1, -1 };

    // Cached atomic parameter pointers (avoid per-block string lookups).
    // Index order matches ParamID: Freq, Gain, Q, Type, Enabled.
    std::atomic<float>* masterGainParam = nullptr;
    std::atomic<float>* bypassParam = nullptr;
    std::atomic<float>* eqModeParam = nullptr;
    std::atomic<float>* firLengthParam = nullptr;
    std::atomic<float>* spectrumFftParam = nullptr;     // spectrum_fft_size（v0.4.0 起为分辨率档）
    std::atomic<float>* spectrumWindowParam = nullptr;  // spectrum_window
    std::atomic<float>* spectrumDensityParam = nullptr;
    std::atomic<float>* spectrumHopParam = nullptr;
    std::atomic<float>* spectrumSmoothingParam = nullptr;  // spectrum_smoothing（v0.4.1）
    std::atomic<float>* nodeParams[SubEQ::NumNodes][5] = {};
    std::atomic<int> reportedLatency { 0 };
    std::atomic<bool> hasProcessedAudio { false };
    // Packed (version, latency) so the timer can never ack a version with a
    // different latency than the one it passed to setLatencySamples().
    std::atomic<uint64_t> latencyRequest { 0 };
    std::atomic<int> currentMode { static_cast<int> (SubEQ::EQMode::ZeroLatency) };
    std::atomic<bool> firActiveMirror { false };
    std::atomic<int> spectrumEditorCount { 0 };   // 存活的编辑器数
    std::atomic<bool> spectrumDisplayResetRequested { false };
    std::atomic<bool> latencyDirty { false };   // audio thread -> timer: report PDC
    int lastRequestedDesignVersion = -1;        // audio thread only
    double lastPreparedSampleRate = 0.0;
    int lastPreparedChannels = 0;
    bool modeChanged = false;
    bool eqParamsChanged = false;
    bool firLengthChanged = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubEQAudioProcessor)
};
