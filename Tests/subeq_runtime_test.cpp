// JUCE console integration test for the real SubEQAudioProcessor.
// Simulates a host: prepareToPlay, processBlock, parameter automation,
// PDC timer pumping, 65536-tap FIR, Min<->Linear switches and bypass.
// Build with Tests/build_runtime_test.bat (links the Release SharedCode lib).

#include <JuceHeader.h>
#include "PluginProcessor.h"

#include <cmath>
#include <cstdio>

namespace
{
int failures = 0;

void check(bool condition, const char* message)
{
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

struct Host
{
    SubEQAudioProcessor processor;
    double sampleRate = 48000.0;
    int blockSize = 512;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
    int phase = 0;

    explicit Host(int bs = 512, double sr = 48000.0)
        : sampleRate(sr), blockSize(bs), buffer(2, bs)
    {
        processor.setPlayConfigDetails(2, 2, sampleRate, blockSize);
        processor.prepareToPlay(sampleRate, blockSize);
    }

    void processBlock(int sleepMs = 0)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < blockSize; ++n)
            {
                const float t = static_cast<float>(phase + n) / static_cast<float>(sampleRate);
                buffer.setSample(ch, n, 0.25f * std::sin(2.0f * juce::MathConstants<float>::pi * 100.0f * t));
            }
        phase += blockSize;
        processor.processBlock(buffer, midi);
        // Drive the deferred PDC report/ack deterministically instead of
        // relying on the message-thread timer in a console harness.
        processor.processDeferredLatencyForTest();
        if (sleepMs > 0)
            juce::Thread::sleep(sleepMs);
    }

    void runBlocks(int count, int sleepMs = 0)
    {
        for (int i = 0; i < count; ++i)
            processBlock(sleepMs);
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

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

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
        cold.runBlocks(120, 10);
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
                        element.removeChildElement(i, true);
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

    std::printf("RUNTIME TEST %s (%d failure(s))\n",
                failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
