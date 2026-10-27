// JUCE console integration test for the real SubEQAudioProcessor.
// Simulates a host: prepareToPlay, processBlock, parameter automation,
// PDC timer pumping, 65536-tap FIR, Min<->Linear switches and bypass.
// Build with Tests/build_runtime_test.bat (links the Release SharedCode lib).

#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
#endif

#define SUBEQ_TESTING 1
#include <JuceHeader.h>
#include "PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <vector>

#if JUCE_WINDOWS
 #include <windows.h>
 #ifdef min
  #undef min
 #endif
 #ifdef max
  #undef max
 #endif
#endif

namespace
{
int failures = 0;
int checksRun = 0;
constexpr int kExpectedChecks = 66;

void check(bool condition, const char* message)
{
    ++checksRun;
    if (!condition)
    {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
    else
    {
        std::printf("PASS: %s\n", message);
    }
}

void setParam(SubEQAudioProcessor& processor, const char* id, float value)
{
    if (auto* p = processor.getAPVTS().getParameter(id))
        p->setValueNotifyingHost(p->convertTo0to1(value));
}

float setChoice(SubEQAudioProcessor& processor, const char* id, int index)
{
    if (auto* p = processor.getAPVTS().getParameter(id))
    {
        const float norm = p->convertTo0to1(static_cast<float>(index));
        p->setValueNotifyingHost(norm);
        return norm;
    }
    return 0.0f;
}

void pumpMessages(int ms)
{
    // runDispatchLoopUntil() is not available in this JUCE 7 build; run the
    // message loop with a one-shot timer that stops it after `ms`.
    juce::Timer::callAfterDelay(ms, []
    {
        juce::MessageManager::getInstance()->stopDispatchLoop();
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
}

// Real wall-clock pump for the VST3 bundle test: the hosted instance lives in
// a separate JUCE module/DLL with its own MessageManager, and re-entering the
// host's dispatch loop after editor creation can return immediately. Pumping
// Win32 messages directly still dispatches the plugin's timer posts.
void pumpWallClock(int ms)
{
   #if JUCE_WINDOWS
    const double end = juce::Time::getMillisecondCounterHiRes() + ms;
    MSG msg;
    while (juce::Time::getMillisecondCounterHiRes() < end)
    {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        juce::Thread::sleep(1);
    }
   #else
    juce::Thread::sleep(ms);
   #endif
}

struct Host
{
    SubEQAudioProcessor processor;
    double sampleRate = 48000.0;
    int blockSize = 512;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
    int phase = 0;
    bool pumpLatency = true;   // false = simulate a host that never acks PDC

    explicit Host(int bs = 512, double sr = 48000.0, int channels = 2)
        : sampleRate(sr), blockSize(bs), buffer(channels, bs)
    {
        processor.setPlayConfigDetails(channels, channels, sampleRate, blockSize);
        processor.prepareToPlay(sampleRate, blockSize);
    }

    void processBlock(int sleepMs = 0)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int n = 0; n < blockSize; ++n)
            {
                const float t = static_cast<float>(phase + n) / static_cast<float>(sampleRate);
                buffer.setSample(ch, n, 0.25f * std::sin(2.0f * juce::MathConstants<float>::pi * 100.0f * t));
            }
        phase += blockSize;
        processor.processBlock(buffer, midi);
        // Drive the deferred PDC report/ack deterministically instead of
        // relying on the message-thread timer in a console harness.
        if (pumpLatency)
            processor.processDeferredLatencyForTest();
        if (sleepMs > 0)
            juce::Thread::sleep(sleepMs);
    }

    void runBlocks(int count, int sleepMs = 0)
    {
        for (int i = 0; i < count; ++i)
            processBlock(sleepMs);
    }

    void changeSampleRate(double newRate)
    {
        sampleRate = newRate;
        phase = 0;
        const int channels = buffer.getNumChannels();
        processor.setPlayConfigDetails(channels, channels, sampleRate, blockSize);
        processor.prepareToPlay(sampleRate, blockSize);
    }

    // Maximum per-block peak across a run: proves an output gate opens at
    // least part of the time instead of only sampling the last block.
    float runBlocksTracked(int count, int sleepMs = 0)
    {
        float maxPeak = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            processBlock(sleepMs);
            maxPeak = std::max(maxPeak, peak());
        }
        return maxPeak;
    }

    bool outputFinite() const
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int n = 0; n < buffer.getNumSamples(); ++n)
                if (!std::isfinite(buffer.getSample(ch, n)))
                    return false;
        return true;
    }

    float rawParam(const char* id)
    {
        if (auto* v = processor.getAPVTS().getRawParameterValue(id))
            return v->load();
        return -999.0f;
    }

    void dump(const char* tag)
    {
        std::printf("  [%s] mode=%d fir=%g enabled=%g bypass=%g hostLat=%d reported=%d designV=%d active=%d iirL=%d trans=%d target=%llu committed=%llu peak=%g\n",
                    tag,
                    processor.getCurrentMode(),
                    rawParam("fir_length"),
                    rawParam("node0_enabled"),
                    rawParam("bypass"),
                    processor.getLatencySamples(),
                    processor.getCurrentLatencySamples(),
                    processor.getFirDesignVersion(),
                    processor.isFirActiveForTest() ? 1 : 0,
                    processor.getIirPathLatencyForTest(),
                    processor.isLatencyTransitionForTest() ? 1 : 0,
                    static_cast<unsigned long long>(processor.getPendingTargetForTest()),
                    static_cast<unsigned long long>(processor.getCommittedForTest()),
                    peak());
        std::printf("    warmV=%d warmT=%llu localV=%d localWarmV=%d\n",
                    processor.getWarmVersionForTest(),
                    static_cast<unsigned long long>(processor.getWarmTargetForTest()),
                    processor.getAudioLocalVersionForTest(),
                    processor.getAudioLocalWarmVersionForTest());
    }

    float peak() const
    {
        float p = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int n = 0; n < buffer.getNumSamples(); ++n)
                p = std::max(p, std::abs(buffer.getSample(ch, n)));
        return p;
    }
};

int runSoak(int minutes)
{
    Host host;
    setChoice(host.processor, "eq_mode", 2);       // Linear Phase
    setChoice(host.processor, "fir_length", 2);    // 65536
    setParam(host.processor, "node0_enabled", 1.0f);
    setParam(host.processor, "node0_freq", 80.0f);
    setParam(host.processor, "node0_gain", 6.0f);

    const int blocks = std::max(1, minutes) * 60 * 100;   // 10 ms cadence
    bool finite = true;
    float maxPeak = 0.0f;

    for (int i = 0; i < blocks; ++i)
    {
        if (i % 300 == 0)
        {
            setParam(host.processor, "node0_freq",
                     40.0f + static_cast<float>((i / 300) % 200));
            setParam(host.processor, "node0_gain",
                     6.0f * static_cast<float>((i / 300) % 3) - 4.0f);
        }

        host.processBlock(10);
        finite = finite && host.outputFinite();
        maxPeak = std::max(maxPeak, host.peak());

        if ((i + 1) % 6000 == 0)
        {
            std::printf("SOAK %d/%d min: latency=%d active=%d peak=%.6f\n",
                        (i + 1) / 6000, minutes,
                        host.processor.getLatencySamples(),
                        host.processor.isFirActiveForTest() ? 1 : 0,
                        maxPeak);
        }

        if (!finite)
        {
            std::printf("SOAK FAIL: non-finite output at block %d\n", i);
            return 1;
        }
    }

    check(finite, "soak: output stays finite");
    check(maxPeak > 1.0e-5f, "soak: output stays audible");
    check(host.processor.isFirActiveForTest(), "soak: FIR path stays active");
    check(host.processor.getLatencySamples() == 33278, "soak: latency stays reported");
    std::printf("SOAK %s (%d min)\n", failures == 0 ? "PASSED" : "FAILED", minutes);
    return failures == 0 ? 0 : 1;
}

} // namespace

constexpr int kExpectedBundleChecks = 23;

juce::AudioProcessorParameter* findParam(juce::AudioProcessor& processor,
                                        const juce::String& name)
{
    for (auto* p : processor.getParameters())
        if (p->getName(128).containsIgnoreCase(name))
            return p;
    return nullptr;
}

// Loads the shipped .vst3 bundle through JUCE's real VST3 hosting path
// (COM factory, bus layout, PDC, state, editor) instead of linking the
// processor directly. Run with --vst3 <bundle path>.
int runBundleLoadTest(const juce::String& bundlePath)
{
    const juce::File bundle(bundlePath);
    check(bundle.exists(), "VST3 bundle exists on disk");

    juce::AudioPluginFormatManager formats;
    formats.addDefaultFormats();
    juce::AudioPluginFormat* vst3 = nullptr;
    for (int i = 0; i < formats.getNumFormats(); ++i)
        if (formats.getFormat(i)->getName() == "VST3")
            vst3 = formats.getFormat(i);
    check(vst3 != nullptr, "VST3 hosting format is registered");

    if (vst3 == nullptr || ! bundle.exists())
    {
        std::printf("VST3 BUNDLE TEST FAILED (%d check(s), %d failure(s))\n",
                    checksRun, failures);
        return 1;
    }

    juce::OwnedArray<juce::PluginDescription> types;
    vst3->findAllTypesForFile(types, bundle.getFullPathName());
    check(types.size() == 1, "VST3 scan finds exactly one plugin");
    if (types.size() != 1)
    {
        std::printf("VST3 BUNDLE TEST FAILED (%d check(s), %d failure(s))\n",
                    checksRun, failures);
        return 1;
    }

    const auto& desc = *types[0];
    check(desc.name == "Napkid Sub EQ", "VST3 plugin name is correct");
    check(desc.version == "0.5.0", "VST3 plugin version is 0.5.0");
    check(desc.manufacturerName == "Napkid Audio", "VST3 manufacturer is correct");
    check(! desc.isInstrument, "VST3 plugin is an effect");
    check(desc.createIdentifierString().isNotEmpty(),
          "VST3 plugin has a non-empty identifier");

    juce::String error;
    std::unique_ptr<juce::AudioPluginInstance> instance(
        formats.createPluginInstance(desc, 48000.0, 512, error));
    check(instance != nullptr, "VST3 instance can be created");
    if (instance == nullptr)
    {
        std::printf("VST3 BUNDLE TEST FAILED (%d check(s), %d failure(s)): %s\n",
                    checksRun, failures, error.toRawUTF8());
        return 1;
    }

    check(instance->getParameters().size() == 52,
          "VST3 exposes 51 plugin parameters plus host bypass");
    check(instance->getBusCount(true) == 1 && instance->getBusCount(false) == 1
              && instance->getChannelCountOfBus(true, 0) == 2
              && instance->getChannelCountOfBus(false, 0) == 2,
          "VST3 instance exposes stereo I/O");

    instance->setPlayConfigDetails(2, 2, 48000.0, 512);
    instance->prepareToPlay(48000.0, 512);
    check(instance->getLatencySamples() == 0,
          "VST3 default Zero Latency reports zero latency");

    check(instance->hasEditor(), "VST3 instance provides an editor");
    std::unique_ptr<juce::AudioProcessorEditor> editor(
        instance->createEditorIfNeeded());
    check(editor != nullptr, "VST3 editor can be created in-process");
    if (editor != nullptr)
    {
        editor->setSize(900, 620);
        editor->addToDesktop(0);
        editor->setVisible(true);
        pumpWallClock(300);
        check(editor->isShowing(), "VST3 editor attaches to a native peer");
        editor->setVisible(false);
        editor->removeFromDesktop();
        editor.reset();
    }

    auto* enabled = findParam(*instance, "Node 1 Enabled");
    auto* mode = findParam(*instance, "EQ Mode");
    auto* fir = findParam(*instance, "FIR Length");
    auto* gain = findParam(*instance, "Node 1 Gain");
    check(enabled != nullptr && mode != nullptr && fir != nullptr && gain != nullptr,
          "VST3 parameters are addressable by name");
    if (enabled != nullptr) enabled->setValueNotifyingHost(1.0f);
    if (mode != nullptr)    mode->setValueNotifyingHost(1.0f);    // Linear Phase
    if (fir != nullptr)     fir->setValueNotifyingHost(1.0f);     // 65536
    if (gain != nullptr)    gain->setValueNotifyingHost(0.75f);

    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    bool finite = true;
    float peak = 0.0f;
    double phase = 0.0;
    const double inc = 2.0 * juce::MathConstants<double>::pi * 50.0 / 48000.0;
    const double started = juce::Time::getMillisecondCounterHiRes();
    const double deadline = started + 40000.0;
    int blocks = 0;

    while (blocks < 4000 && juce::Time::getMillisecondCounterHiRes() < deadline)
    {
        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            const float v = static_cast<float>(0.25 * std::sin(phase));
            phase += inc;
            buffer.setSample(0, n, v);
            buffer.setSample(1, n, v);
        }

        instance->processBlock(buffer, midi);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int n = 0; n < buffer.getNumSamples(); ++n)
            {
                const float v = buffer.getSample(ch, n);
                finite = finite && std::isfinite(v);
                peak = std::max(peak, std::abs(v));
            }

        ++blocks;
        pumpWallClock(10);

        if (blocks >= 400 && instance->getLatencySamples() == 33278)
            break;
    }

    float lastPeak = 0.0f;
    {
        instance->processBlock(buffer, midi);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int n = 0; n < buffer.getNumSamples(); ++n)
                lastPeak = std::max(lastPeak, std::abs(buffer.getSample(ch, n)));
    }

    check(finite, "VST3 65536 linear processing stays finite");
    check(peak > 1.0e-4f, "VST3 65536 linear processing produces audio");
    check(lastPeak > 1.0e-4f, "VST3 output is audible after PDC converges");
    check(instance->getLatencySamples() == 33278,
          "VST3 PDC reports 33278 samples for 65536 linear phase");

    juce::MemoryBlock state;
    instance->getStateInformation(state);
    check(state.getSize() > 0, "VST3 state serialization is non-empty");

    std::unique_ptr<juce::AudioPluginInstance> restored(
        formats.createPluginInstance(desc, 48000.0, 512, error));
    check(restored != nullptr, "second VST3 instance can be created");
    if (restored != nullptr)
    {
        restored->setPlayConfigDetails(2, 2, 48000.0, 512);
        restored->prepareToPlay(48000.0, 512);
        restored->setStateInformation(state.getData(), (int) state.getSize());
        auto* restoredGain = findParam(*restored, "Node 1 Gain");
        check(restoredGain != nullptr
                  && std::abs(restoredGain->getValue() - 0.75f) < 0.02f,
              "VST3 state restores the node gain");
        restored.reset();
    }

    instance.reset();

    if (checksRun != kExpectedBundleChecks)
    {
        std::printf("FAIL: expected %d VST3 bundle checks, ran %d\n",
                    kExpectedBundleChecks, checksRun);
        ++failures;
    }

    std::printf("VST3 BUNDLE TEST %s (%d check(s), %d failure(s))\n",
                failures == 0 ? "PASSED" : "FAILED", checksRun, failures);
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI init;

    for (int i = 1; i < argc; ++i)
    {
        if (juce::String(argv[i]) == "--dump-desc")
        {
            const juce::String path = (i + 1 < argc) ? juce::String(argv[i + 1])
                                                     : juce::String();
            if (! juce::File(path).exists())
                return 1;
            juce::AudioPluginFormatManager formats;
            formats.addDefaultFormats();
            for (int f = 0; f < formats.getNumFormats(); ++f)
                if (formats.getFormat(f)->getName() == "VST3")
                {
                    juce::OwnedArray<juce::PluginDescription> types;
                    formats.getFormat(f)->findAllTypesForFile(types, path);
                    if (types.size() == 1)
                        if (auto xml = types[0]->createXml())
                        {
                            std::printf("%s\n", xml->toString().toRawUTF8());
                            return 0;
                        }
                    return 1;
                }
            return 1;
        }
        if (juce::String(argv[i]) == "--vst3")
        {
            const juce::String path = (i + 1 < argc) ? juce::String(argv[i + 1])
                                                     : juce::String();
            return runBundleLoadTest(path);
        }
        if (juce::String(argv[i]) == "--soak")
        {
            const int minutes = (i + 1 < argc) ? juce::String(argv[i + 1]).getIntValue()
                                               : 20;
            return runSoak(minutes);
        }
    }

    {
        Host host;
        setChoice(host.processor, "eq_mode", 0);       // Zero Latency
        setChoice(host.processor, "fir_length", 2);    // 65536
        setParam(host.processor, "node0_enabled", 1.0f);
        setParam(host.processor, "node0_freq", 80.0f);
        setParam(host.processor, "node0_gain", 6.0f);
        host.runBlocks(4);
        host.dump("ZL start");
        check(host.processor.getLatencySamples() == 0, "ZL reports zero latency");

        // Enter Linear Phase with the longest FIR. The IIR bridge should keep
        // audio alive while the design/warm is built; the worker needs real
        // time to synthesize, so process at roughly host cadence.
        setChoice(host.processor, "eq_mode", 2);
        host.runBlocks(40, 10);
        host.dump("Linear 65536 after 40 blocks");
        const int expectedLatency = 33278;
        check(host.processor.getCurrentMode() == SubEQ::EQMode::LinearPhase,
              "mode switch reaches Linear Phase");
        check(host.processor.getLatencySamples() == expectedLatency,
              "65536 linear phase reports 33278-sample latency");
        check(host.outputFinite(), "65536 linear phase output stays finite");

        // Give the background design/warm time, then verify it actually
        // produces non-silent audio (not stuck in the bridge/silence).
        host.runBlocks(200, 10);
        host.dump("Linear 65536 after warm");
        check(host.peak() > 1.0e-4f, "65536 linear phase produces audio after warm-up");
        check(host.processor.isFirActiveForTest(), "65536 Linear FIR design is active after warm-up");
        check(host.outputFinite(), "65536 warm-up output stays finite");

        // Switch to Minimum Phase: active Linear design must keep playing
        // until the new candidate is ready; latency becomes 511.
        setChoice(host.processor, "eq_mode", 1);
        host.runBlocks(80, 10);
        host.dump("Minimum phase");
        check(host.processor.getLatencySamples() == 511,
              "minimum phase reports 511-sample latency");
        check(host.outputFinite(), "Min<->Linear transition output stays finite");
        check(host.peak() > 1.0e-4f, "minimum phase produces audio");

        // Bypass with a latency-compensated dry path; output must not go NaN
        // or silent and the FIR path must keep running.
        setParam(host.processor, "bypass", 1.0f);
        host.runBlocks(12);
        check(host.outputFinite(), "bypass output stays finite");
        check(host.peak() > 1.0e-4f, "bypass passes the dry signal");
        setParam(host.processor, "bypass", 0.0f);
        host.runBlocks(12);
        check(host.outputFinite(), "un-bypass output stays finite");

        // Back to Zero Latency and then to Linear Phase again: this exercises
        // the repeated PDC handshake and the IIR bridge.
        setChoice(host.processor, "eq_mode", 0);
        host.runBlocks(20);
        check(host.processor.getLatencySamples() == 0, "return to ZL reports zero latency");
        setChoice(host.processor, "eq_mode", 2);
        host.runBlocks(80, 10);
        host.dump("Re-enter Linear 65536");
        check(host.processor.getLatencySamples() == expectedLatency,
              "re-enter 65536 Linear Phase reports latency again");
        check(host.outputFinite(), "re-enter Linear Phase output stays finite");

        std::printf("Host test peak=%.6f latency=%d tail=%.3f s\n",
                    host.peak(), host.processor.getLatencySamples(),
                    host.processor.getTailLengthSeconds());
    }

    // Cold start directly in Linear Phase (no Zero Latency history first).
    {
        Host cold(512);
        setChoice(cold.processor, "eq_mode", 2);
        setChoice(cold.processor, "fir_length", 2);
        setParam(cold.processor, "node0_enabled", 1.0f);
        setParam(cold.processor, "node0_freq", 80.0f);
        setParam(cold.processor, "node0_gain", 6.0f);
        // 65536 cold design can exceed 1.2 s on a loaded machine; allow 4 s.
        cold.runBlocks(400, 10);
        cold.dump("Cold FIR start");
        check(cold.processor.getLatencySamples() == 33278,
              "cold-start Linear 65536 reports latency");
        check(cold.processor.isFirActiveForTest(),
              "cold-start Linear 65536 FIR design becomes active");
        check(cold.outputFinite(), "cold-start Linear output stays finite");
    }

    // Offline (non-realtime) rendering: faster-than-real-time block cadence,
    // synchronous design/warm at the current boundary. This is the bounce path.
    {
        Host offline(4096);
        offline.processor.setNonRealtime(true);
        setChoice(offline.processor, "eq_mode", 2);
        setChoice(offline.processor, "fir_length", 2);
        setParam(offline.processor, "node0_enabled", 1.0f);
        setParam(offline.processor, "node0_freq", 80.0f);
        setParam(offline.processor, "node0_gain", 6.0f);
        offline.runBlocks(20, 0);   // no real-time pacing
        offline.dump("Offline 4096-block Linear 65536");
        check(offline.processor.getLatencySamples() == 33278,
              "offline Linear 65536 reports latency");
        check(offline.processor.isFirActiveForTest(),
              "offline Linear 65536 FIR design becomes active");
        check(offline.outputFinite(), "offline Linear output stays finite");
        check(offline.peak() > 1.0e-4f, "offline Linear produces audio");

        setChoice(offline.processor, "eq_mode", 1);
        offline.runBlocks(6, 0);
        offline.dump("Offline Min after switch");
        check(offline.processor.getLatencySamples() == 511,
              "offline Min phase reports latency");
        check(offline.processor.isFirActiveForTest(),
              "offline Min FIR design becomes active");
        check(offline.outputFinite(), "offline Min output stays finite");
    }

    // Large host block (4096) with the longest FIR and bypass: exercises the
    // single-chunk path and the 8-OLA-block-per-host-block adoption.
    {
        Host large(4096);
        setChoice(large.processor, "eq_mode", 2);
        setChoice(large.processor, "fir_length", 2);
        setParam(large.processor, "node0_enabled", 1.0f);
        setParam(large.processor, "node0_freq", 80.0f);
        setParam(large.processor, "node0_gain", 6.0f);
        large.runBlocks(60, 90);
        large.dump("4096-block Linear 65536");
        check(large.processor.getLatencySamples() == 33278,
              "4096-block linear phase reports 33278-sample latency");
        check(large.outputFinite(), "4096-block linear phase output stays finite");
        check(large.peak() > 1.0e-4f, "4096-block linear phase produces audio");
        check(large.processor.isFirActiveForTest(), "4096-block Linear FIR design is active");
        setParam(large.processor, "bypass", 1.0f);
        large.runBlocks(8);
        check(large.outputFinite(), "4096-block bypass output stays finite");
        check(large.peak() > 1.0e-4f, "4096-block bypass passes dry audio");
    }

    // 96 kHz / 128-sample host block: shortest block + highest common rate.
    {
        Host matrix(128, 96000.0);
        setChoice(matrix.processor, "eq_mode", 2);
        setChoice(matrix.processor, "fir_length", 0);   // 4096
        setParam(matrix.processor, "node0_enabled", 1.0f);
        setParam(matrix.processor, "node0_freq", 80.0f);
        setParam(matrix.processor, "node0_gain", 6.0f);
        matrix.runBlocks(400, 10);
        matrix.dump("96k/128 Linear 4096");
        check(matrix.processor.getLatencySamples() == 2558,
              "96k/128 Linear 4096 reports 2558-sample latency");
        check(matrix.processor.isFirActiveForTest(),
              "96k/128 Linear 4096 becomes active");
        check(matrix.outputFinite() && matrix.peak() > 1.0e-4f,
              "96k/128 Linear 4096 produces audio");
    }

    // Non-512 host block (1000 samples): warm target must be host-block aligned.
    {
        Host matrix(1000);
        setChoice(matrix.processor, "eq_mode", 2);
        setChoice(matrix.processor, "fir_length", 0);
        setParam(matrix.processor, "node0_enabled", 1.0f);
        setParam(matrix.processor, "node0_freq", 80.0f);
        setParam(matrix.processor, "node0_gain", 6.0f);
        matrix.runBlocks(100, 10);
        matrix.dump("1000-block Linear 4096");
        check(matrix.processor.getLatencySamples() == 2558,
              "1000-block Linear 4096 reports 2558-sample latency");
        check(matrix.processor.isFirActiveForTest(),
              "1000-block Linear 4096 becomes active");
        check(matrix.outputFinite() && matrix.peak() > 1.0e-4f,
              "1000-block Linear 4096 produces audio");
    }

    // Mono at 44.1 kHz: channel-count and sample-rate changes must not break
    // FIR adoption or the latency report.
    {
        Host mono(512, 44100.0, 1);
        setChoice(mono.processor, "eq_mode", 2);
        setChoice(mono.processor, "fir_length", 0);   // 4096
        setParam(mono.processor, "node0_enabled", 1.0f);
        setParam(mono.processor, "node0_freq", 80.0f);
        setParam(mono.processor, "node0_gain", 6.0f);
        mono.runBlocks(150, 10);
        mono.dump("mono 44.1k Linear 4096");
        check(mono.processor.getLatencySamples() == 2558,
              "mono 44.1k Linear 4096 reports 2558-sample latency");
        check(mono.processor.isFirActiveForTest(),
              "mono 44.1k Linear 4096 becomes active");
        check(mono.outputFinite() && mono.peak() > 1.0e-4f,
              "mono 44.1k Linear 4096 produces audio");
    }

    // Mid-session sample-rate change with an active FIR design: prepareToPlay
    // must abort the in-flight warm build, rebuild the shared VQT bank, and
    // re-establish the latency handshake without a crash or permanent silence.
    {
        Host change;
        setChoice(change.processor, "eq_mode", 2);
        setChoice(change.processor, "fir_length", 0);   // 4096
        setParam(change.processor, "node0_enabled", 1.0f);
        setParam(change.processor, "node0_freq", 80.0f);
        setParam(change.processor, "node0_gain", 6.0f);
        change.runBlocks(150, 10);
        check(change.processor.isFirActiveForTest(),
              "pre-SR-change Linear 4096 is active");

        change.changeSampleRate(96000.0);
        const float peakAfterChange = change.runBlocksTracked(250, 10);
        change.dump("after SR change");

        check(change.processor.getLatencySamples() == 2558,
              "SR change re-reports the Linear 4096 latency");
        check(change.processor.isFirActiveForTest(),
              "SR change re-activates the FIR path");
        check(change.outputFinite() && peakAfterChange > 1.0e-5f,
              "SR change output stays finite and audible");
    }

    // Host that never pumps the PDC timer: the bounded coordinator must keep
    // the IIR bridge audible instead of gating forever, and recover once
    // acknowledgements resume.
    {
        Host host;
        host.pumpLatency = false;
        setChoice(host.processor, "eq_mode", 2);
        setChoice(host.processor, "fir_length", 2);   // 65536
        setParam(host.processor, "node0_enabled", 1.0f);
        setParam(host.processor, "node0_freq", 80.0f);
        setParam(host.processor, "node0_gain", 6.0f);
        const float noAckPeak = host.runBlocksTracked(120, 5);
        host.dump("no-PDC-ack after 120 blocks");

        check(host.outputFinite(), "no-PDC-ack: output stays finite");
        check(noAckPeak > 1.0e-5f, "no-PDC-ack: IIR bridge stays audible");
        check(!host.processor.isFirActiveForTest(),
              "no-PDC-ack: unacknowledged design is never adopted");

        host.pumpLatency = true;
        host.runBlocks(200, 10);
        host.dump("no-PDC-ack after recovery");
        check(host.processor.isFirActiveForTest(),
              "no-PDC-ack: design adopts after acknowledgements resume");
        check(host.processor.getLatencySamples() == 33278,
              "no-PDC-ack: latency is reported after recovery");
        check(host.outputFinite() && host.peak() > 1.0e-5f,
              "no-PDC-ack: recovered output is audible");
    }

    // Stress: rapid mode/FIR-length switches with interleaved prepareToPlay
    // re-entry must leave a finite, audible output (design epochs, retirement
    // rings and the PDC coordinator all reset/recover).
    {
        Host stress;
        setParam(stress.processor, "node0_enabled", 1.0f);
        setParam(stress.processor, "node0_freq", 80.0f);
        setParam(stress.processor, "node0_gain", 6.0f);

        for (int i = 0; i < 24; ++i)
        {
            setChoice(stress.processor, "eq_mode",
                      (i % 3 == 0) ? 0 : ((i % 3 == 1) ? 1 : 2));
            setChoice(stress.processor, "fir_length", i % 3);
            stress.runBlocks(3, 1);
            if (i % 6 == 5)
                stress.processor.prepareToPlay(stress.sampleRate, stress.blockSize);
        }

        setChoice(stress.processor, "eq_mode", 2);
        setChoice(stress.processor, "fir_length", 0);
        const float stressPeak = stress.runBlocksTracked(150, 10);

        check(stress.outputFinite(), "rapid mode/FIR/prepare stress output stays finite");
        check(stressPeak > 1.0e-5f, "rapid mode/FIR/prepare stress output stays audible");
        check(stress.processor.getLatencySamples() == 2558,
              "rapid mode/FIR/prepare stress recovers the latency report");
    }

    // Offline Minimum Phase with a 4096 block: the first block must already
    // contain audio (no 50 ms PDC gate on the bounce path).
    {
        Host offlineFirst(4096);
        offlineFirst.processor.setNonRealtime(true);
        setChoice(offlineFirst.processor, "eq_mode", 1);
        setChoice(offlineFirst.processor, "fir_length", 0);
        setParam(offlineFirst.processor, "node0_enabled", 1.0f);
        setParam(offlineFirst.processor, "node0_freq", 80.0f);
        setParam(offlineFirst.processor, "node0_gain", 6.0f);
        offlineFirst.runBlocks(1, 0);
        offlineFirst.dump("Offline Min first block");
        check(offlineFirst.outputFinite() && offlineFirst.peak() > 1.0e-5f,
              "offline Minimum Phase first block is audible (no startup gate)");
        check(offlineFirst.processor.isFirActiveForTest(),
              "offline Minimum Phase activates on the first block");
    }

    // Runtime spectrum: configure, feed 3 s, read a complete frame, reset to
    // floor, and verify NaN input cannot poison the published frame.
    {
        Host spectrumHost;
        auto& spec = spectrumHost.processor.getSpectrumAnalyzer();
        spec.configure(1, 121, 512, SubEQ::WindowType::BlackmanHarris, 0);

        std::vector<float> block(512);
        for (int n = 0; n < 48000 * 3; n += 512)
        {
            for (int i = 0; i < 512; ++i)
                block[static_cast<size_t>(i)] =
                    0.5f * std::sin(2.0f * juce::MathConstants<float>::pi
                                    * 100.0f * static_cast<float>(n + i) / 48000.0f);
            spec.process(block.data(), 512);
        }

        float bands[SubEQ::SpectrumAnalyzer::MaxBands] {};
        const int numBands = spec.getSpectrum(bands);
        check(numBands == 121, "runtime spectrum: 121-band frame returned");
        bool finite = true;
        bool aboveFloor = false;
        for (int i = 0; i < numBands; ++i)
        {
            finite = finite && std::isfinite(bands[i]);
            aboveFloor = aboveFloor || bands[i] > -59.0f;
        }
        check(finite, "runtime spectrum: frame values are finite");
        check(aboveFloor, "runtime spectrum: frame contains analysed energy");

        spec.resetPublishedDisplay();
        const int floorBands = spec.getSpectrum(bands);
        bool allFloor = floorBands == 121;
        for (int i = 0; i < floorBands; ++i)
            allFloor = allFloor && std::abs(bands[i] + 60.0f) < 1.0e-3f;
        check(allFloor, "runtime spectrum: editor reopen shows the floor frame");

        for (auto& v : block) v = std::numeric_limits<float>::quiet_NaN();
        spec.process(block.data(), 512);
        const int nanBands = spec.getSpectrum(bands);
        bool nanFinite = nanBands == 121;
        for (int i = 0; i < nanBands; ++i)
            nanFinite = nanFinite && std::isfinite(bands[i]);
        check(nanFinite, "runtime spectrum: NaN input does not poison the frame");
    }

    // Accelerated soak: 60 s of offline audio with periodic node automation.
    // Offline mode renders faster than real time while still driving the real
    // design/warm/OLA path, so this catches state leaks and drift cheaply.
    {
        Host soak(4096);
        soak.processor.setNonRealtime(true);
        setChoice(soak.processor, "eq_mode", 2);
        setChoice(soak.processor, "fir_length", 0);   // 4096
        setParam(soak.processor, "node0_enabled", 1.0f);

        const int totalBlocks = (60 * 48000) / soak.blockSize;
        bool finite = true;
        float maxPeak = 0.0f;

        for (int i = 0; i < totalBlocks; ++i)
        {
            if (i % 20 == 0)
            {
                setParam(soak.processor, "node0_freq", 40.0f + static_cast<float>(i % 200));
                setParam(soak.processor, "node0_gain",
                         6.0f * static_cast<float>((i / 20) % 3) - 4.0f);
            }

            soak.processBlock(0);
            finite = finite && soak.outputFinite();
            maxPeak = std::max(maxPeak, soak.peak());
        }

        check(finite, "60 s offline soak output stays finite");
        check(maxPeak > 1.0e-5f, "60 s offline soak output stays audible");
        check(soak.processor.isFirActiveForTest(),
              "60 s offline soak keeps the FIR path active");
    }

    // Old-session migration: a missing spectrum_range must reset to its
    // parameter default (60 dB), not leak the current instance value.
    {
        Host host;
        setChoice(host.processor, "spectrum_range", 0);   // 24 dB current value
        juce::MemoryBlock state;
        host.processor.getStateInformation(state);

        auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(),
                                                          (int) state.getSize());
        check(xml != nullptr, "state serialization produces XML");

        if (xml != nullptr)
        {
            auto removeParam = [&](auto&& self, juce::XmlElement& element) -> void
            {
                for (int i = element.getNumChildElements(); --i >= 0;)
                {
                    auto* child = element.getChildElement(i);
                    if (child->hasTagName("PARAM")
                        && child->getStringAttribute("id") == "spectrum_range")
                        element.removeChildElement(child, true);
                    else
                        self(self, *child);
                }
            };
            removeParam(removeParam, *xml);

            juce::MemoryBlock oldState;
            juce::AudioProcessor::copyXmlToBinary(*xml, oldState);
            host.processor.setStateInformation(oldState.getData(),
                                               (int) oldState.getSize());

            auto* range = host.processor.getAPVTS().getRawParameterValue("spectrum_range");
            check(range != nullptr
                      && juce::roundToInt(range->load()) == 1,
                  "missing spectrum_range migrates to the 60 dB default");
        }
    }

    if (checksRun != kExpectedChecks)
    {
        std::printf("FAIL: expected %d runtime checks, ran %d\n",
                    kExpectedChecks, checksRun);
        ++failures;
    }

    std::printf("RUNTIME TEST %s (%d check(s), %d failure(s))\n",
                failures == 0 ? "PASSED" : "FAILED", checksRun, failures);
    return failures == 0 ? 0 : 1;
}
