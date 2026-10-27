/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "SubEQ_LatencyMath.h"
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
        // Capture the ack epoch before reading the request. If an abort lands
        // between the read and the ack, acknowledgeLatency's epoch check
        // rejects the superseded request; retry with the newer request then.
        const uint64_t epoch = fftProcessor.getLatencyAckEpoch();
        const uint64_t packed = latencyRequest.load (std::memory_order_acquire);

        if (fftProcessor.getLatencyAckEpoch() != epoch)
        {
            latencyDirty.store (true, std::memory_order_release);
            return;
        }

        const int desired = static_cast<int> (static_cast<int32_t> (packed & 0xffffffffu));
        const int version = static_cast<int> (static_cast<int32_t> (packed >> 32));

        // Ack first: if an abort landed, the CAS fails and we must NOT tell the
        // host a latency whose switch was cancelled. The 50 ms guard starts at
        // the ack, so setLatencySamples (below) still lands well before the
        // audio switches.
        if (! fftProcessor.acknowledgeLatency (version, epoch))
        {
            latencyDirty.store (true, std::memory_order_release);
            return;
        }

        setLatencySamples (desired);
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

bool SubEQAudioProcessor::getFirResponseSnapshot(
    SubEQ::FFTProcessor::FirResponseSnapshot& out, int version) const
{
    return fftProcessor.getFirResponseSnapshot (out, version);
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

    const int choice = firLengthParam != nullptr
                           ? juce::roundToInt (firLengthParam->load())
                           : 0;
    const int index = juce::jlimit (0, SubEQ::FFTProcessor::NumFirLengthChoices - 1, choice);
    const int nominal = SubEQ::FFTProcessor::FirLengthChoices[index];
    return SubEQ::pipelineLatency (mode, nominal,
                                   SubEQ::FFTProcessor::ConvBlockLen);
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
        latencyCoordinator.reset();
    }
    switchGateStep = 1.0f / static_cast<float> (std::max (1.0, 0.005 * sampleRate));
    firEntryFadeRemaining = 0;
    fftProcessor.prepare (sampleRate, samplesPerBlock, getTotalNumInputChannels());
    fftProcessor.setParameterSource (&apvts);
    // Do NOT reset the convolution on a same-session prepare: prepare()
    // already resets it when the sample rate/channel count changes, and
    // dropping an active design here would force a needless re-warm + PDC
    // handshake on every transport stop/start.
    // One immutable kernel/twiddle bank per sample rate, shared by the input
    // and output analyzers (identical tables; building both duplicated tens of
    // MB of window/kernel data).
    if (vqtKernelBank == nullptr || vqtKernelBank->sampleRate != sampleRate)
        vqtKernelBank = SubEQ::buildVQTKernelBank (sampleRate);
    spectrumAnalyzer.prepare (sampleRate, vqtKernelBank);
    inputAnalyzer.prepare (sampleRate, vqtKernelBank);
    // 不强制 currentMode/eqModeCache 归零：同会话重复 prepare 时 eq_mode
    // 参数未变，若清空缓存，updateEQParameters 会误报“模式变化”，触发一次
    // 多余的 FIR 重设计并在发布时冲刷卷积尾（播放中出现输出跳变）。仅当
    // 参数真正变化时才由 updateEQParameters 置 modeChanged。
    modeChanged = false;
    eqParamsChanged = false;
    lastRequestedDesignVersion = -1;
    firActiveMirror.store (false, std::memory_order_release);
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
    hasProcessedAudio.store (true, std::memory_order_release);

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
            // Leaving FIR: flush its OLA state and fall back to the IIR chain.
            // The always-on IIR shadow kept the engine current during FIR, so
            // no biquad reset is needed. Mute immediately before changing the
            // time axis: a 5 ms fade after the switch would leak mistimed audio.
            const bool leavingFir = reportedLatency.load() != 0;
            fftProcessor.resetConvolution();
            latencyCoordinator.reset();

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

    // Bounded latency coordinator (pure state machine in
    // SubEQ_LatencyCoordinator.h). It starts only when the pending plan's
    // target is close; the old path stays audible for the whole warm-build
    // lead. If the target is missed or the transition exceeds the deadline,
    // abort the candidate and restore the active latency — never gate forever.
    SubEQ::LatencyCoordinator::BlockInput latencyInput;
    latencyInput.hostBlockSamples = std::max (1, buffer.getNumSamples());
    latencyInput.sampleRate = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    latencyInput.firMode = (mode != SubEQ::EQMode::ZeroLatency);
    latencyInput.nonRealtime = isNonRealtime();
    latencyInput.activeBridgeLatency = iirPathLatency;
    // Read a stable (plan, target) pair: the worker stores the target before
    // the packed plan, so a plan re-read catches a publication that landed
    // between our two loads. Unstable after a retry: skip this block's
    // transition rather than pair v8's target with v7's latency.
    int planVersion = 0;
    int planLatency = 0;
    bool planWarmReady = false;
    uint64_t planTarget = 0;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        fftProcessor.getPublishedPlan (planVersion, planLatency, planWarmReady);
        planTarget = fftProcessor.getPendingTargetSample();

        int recheckVersion = 0;
        int recheckLatency = 0;
        bool recheckWarmReady = false;
        fftProcessor.getPublishedPlan (recheckVersion, recheckLatency, recheckWarmReady);

        if (recheckVersion == planVersion && recheckLatency == planLatency)
            break;

        if (attempt == 1)
        {
            planVersion = 0;
            planLatency = 0;
            planWarmReady = false;
            planTarget = 0;
        }
    }

    latencyInput.pendingTarget = planTarget;
    latencyInput.committed = fftProcessor.getCommittedSampleCount();
    latencyInput.designVersion = planVersion;
    latencyInput.publishedLatency = planLatency;
    latencyInput.guardActive = fftProcessor.isPdcGuardActive();
    latencyInput.warmReady = planWarmReady;

    const auto latencyDecision = latencyCoordinator.evaluate (latencyInput);

    // On abort: request a fresh warm target, then report the restored latency
    // BEFORE invalidating the ack. The report updates latencyRequest first;
    // a timer that observes the new ack epoch is then guaranteed to see the
    // new (version 0) request instead of acknowledging the superseded design.
    if (latencyDecision.requestWarmRebuild)
        fftProcessor.requestWarmRebuild();
    if (latencyDecision.reportLatency)
        requestLatencyReport (latencyDecision.reportVersion,
                              latencyDecision.reportSamples);
    if (latencyDecision.invalidateAck)
        fftProcessor.invalidateLatencyAck();
    if (latencyDecision.gateHoldSamples > 0)
        switchGateHoldSamples = latencyDecision.gateHoldSamples;

    // Process audio based on current mode
    if (mode == SubEQ::EQMode::ZeroLatency)
    {
        firActiveMirror.store (false, std::memory_order_release);
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
        fftProcessor.setHostLatency (iirPathLatency);
        const bool firActive = fftProcessor.process (buffer);
        firActiveMirror.store (firActive, std::memory_order_release);
        const int nch = std::min (totalNumInputChannels, SubEQ::EQNode::MaxChannels);

        // Update the bridge latency before the entry crossfade: both paths must
        // use the new active latency for the duration of the fade.
        const int activeL = fftProcessor.getLatencySamples();
        if (firActive && activeL > 0)
        {
            iirPathLatency = activeL;

            int completionHold = 0;
            if (latencyCoordinator.notifyActiveLatency (
                    activeL, latencyInput.sampleRate, completionHold))
                switchGateHoldSamples = completionHold;
        }

        // Always-on IIR shadow: the same engine advances on the raw input in
        // FIR mode, so ZL<->FIR never needs a biquad reset and the bridge is
        // always time-aligned. In ZL / not-yet-FIR mode its output is audible.
        const bool haveShadow = haveDryCopy && nch > 0
                             && buffer.getNumSamples() <= transitionBuffer.getNumSamples();
        if (haveShadow)
        {
            for (int ch = 0; ch < nch; ++ch)
                transitionBuffer.copyFrom (ch, 0, dryInput, ch, 0,
                                           buffer.getNumSamples());

            float* transitionPtrs[SubEQ::EQNode::MaxChannels] = {};
            for (int ch = 0; ch < nch; ++ch)
                transitionPtrs[ch] = transitionBuffer.getWritePointer (ch);
            eqEngine.processChannels (transitionPtrs, nch, buffer.getNumSamples());
            delayIirPath (transitionBuffer, nch, buffer.getNumSamples());

            if (!firActive)
            {
                for (int ch = 0; ch < nch; ++ch)
                    buffer.copyFrom (ch, 0, transitionBuffer, ch, 0,
                                     buffer.getNumSamples());
            }
        }
        else if (!firActive)
        {
            float* channelPtrs[SubEQ::EQNode::MaxChannels] = {};
            for (int ch = 0; ch < nch; ++ch)
                channelPtrs[ch] = buffer.getWritePointer (ch);
            eqEngine.processChannels (channelPtrs, nch, buffer.getNumSamples());
            delayIirPath (buffer, nch, buffer.getNumSamples());
        }

        if (firActive)
        {
            if (!wasActive && haveShadow)
                firEntryFadeRemaining = 1024;

            if (firEntryFadeRemaining > 0 && haveShadow)
            {
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
            requestLatencyReport (latencyInput.designVersion, 0);
    }
    else if (isNonRealtime())
    {
        const int desiredLatency = latencyInput.publishedLatency > 0
                                       ? latencyInput.publishedLatency
                                       : iirPathLatency;
        if (desiredLatency != reportedLatency.load()
            || latencyInput.designVersion != lastRequestedDesignVersion)
        {
            requestLatencyReport (latencyInput.designVersion, desiredLatency);
            lastRequestedDesignVersion = latencyInput.designVersion;
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
        bool pending = latencyCoordinator.isActive();
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

    // v3 is current. The missing-parameter loop above is the active migration
    // for v1/v2 sessions; semantic choice-index migrations remain versioned
    // here when a future session format requires them.
    juce::ignoreUnused (stateVersion);

    // Do not write modeChanged/eqParamsChanged/lastRequestedDesignVersion from
    // the message thread: those are audio-thread caches. updateEQParameters()
    // detects the parameter differences on the next block and applies the
    // change without an unconditional biquad reset/gate.
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SubEQAudioProcessor();
}
