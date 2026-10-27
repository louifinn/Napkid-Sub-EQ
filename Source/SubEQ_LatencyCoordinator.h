/*
  ==============================================================================

    SubEQ_LatencyCoordinator.h
    Bounded PDC (plugin delay compensation) transition state machine.

    The audio thread must never gate forever when a latency-changing FIR
    design misses its warm-up boundary. This header owns the whole decision
    chain as a pure state machine (no JUCE, no allocation, no locks) so the
    exact timing rules can be exercised offline:

      - a transition only starts when the pending plan's host-block target is
        inside the "near" window (never for the whole warm-build lead);
      - the new latency is reported once, before the switch;
      - the deadline is scaled to the distance to the target and capped;
      - a fresh target for the same design extends the deadline;
      - a missed target or an expired deadline aborts the candidate, asks the
        worker for a fresh warm target, restores the audible latency with an
        invalid version (so a stale ack can never adopt the design), and keeps
        the output gate closed for a short hold while the host catches up;
      - the coordinator closes only when the FIR path actually runs at the
        reported latency.

  ==============================================================================
*/

#pragma once

#include <algorithm>
#include <cstdint>

namespace SubEQ
{

class LatencyCoordinator
{
public:
    // Audio-thread snapshot taken at the start of a block.
    struct BlockInput
    {
        int hostBlockSamples = 512;
        double sampleRate = 48000.0;
        bool firMode = false;             // selected eq_mode != ZeroLatency
        bool nonRealtime = false;         // offline render: no gate/handshake
        int publishedLatency = 0;         // latest published design
        int activeBridgeLatency = 0;      // latency the host currently compensates
        uint64_t pendingTarget = 0;       // host-block-aligned warm target
        uint64_t committed = 0;           // raw input samples committed so far
        int designVersion = 0;
        bool guardActive = false;         // acked design is in its 50 ms guard
        bool warmReady = false;           // matching warm state is available
    };

    // Effects the caller must apply (audio thread, no allocation).
    struct BlockResult
    {
        bool gateClosed = false;          // mute while the handshake is open
        bool reportLatency = false;       // call requestLatencyReport(...)
        int reportVersion = 0;
        int reportSamples = 0;
        bool requestWarmRebuild = false;
        bool invalidateAck = false;
        int gateHoldSamples = 0;          // keep the gate closed this many samples
    };

    // Evaluates the pre-processing half of the handshake.
    BlockResult evaluate (const BlockInput& in) noexcept
    {
        BlockResult result;

        if (abortCooldownSamples > 0)
            abortCooldownSamples -= in.hostBlockSamples;

        const int nearWindow = static_cast<int> (
            std::max (0.10 * in.sampleRate, 2.0 * in.hostBlockSamples));
        const int margin = static_cast<int> (
            std::max (0.12 * in.sampleRate, 2.0 * in.hostBlockSamples));
        const int hardCap = static_cast<int> (
            std::max (0.60 * in.sampleRate, 8.0 * in.hostBlockSamples));

        const bool canTransition = in.firMode
                                && ! in.nonRealtime
                                && in.publishedLatency > 0
                                && in.publishedLatency != in.activeBridgeLatency
                                && abortCooldownSamples <= 0;

        // A future target can start the handshake early; a ready warm whose
        // target has already been reached can also start (the audio thread
        // catches up at most one block). Without the latter, a host that
        // ignored earlier reports could never be re-reported.
        const bool targetUsable = in.pendingTarget > in.committed
            ? (in.pendingTarget - in.committed) <= static_cast<uint64_t> (nearWindow)
            : in.warmReady;

        if (canTransition && targetUsable && ! active)
        {
            begin (in, hardCap, margin);
        }

        if (active)
        {
            elapsedSamples += in.hostBlockSamples;
            totalSamples += in.hostBlockSamples;

            // A newer design supersedes the transition in flight (parameter
            // edit while a previous design was warming/guarding). Restart the
            // handshake for the new version/latency instead of waiting for a
            // cap; the old ack cannot satisfy the new design.
            if (in.publishedLatency > 0
                && (in.designVersion != version
                    || in.publishedLatency != targetLatency))
            {
                version = in.designVersion;
                targetLatency = in.publishedLatency;
                reported = false;
                elapsedSamples = 0;
                if (in.pendingTarget > in.committed)
                {
                    lastTarget = in.pendingTarget;
                    deadlineSamples = scaledDeadline (in, hardCap, margin);
                }
            }

            // The worker may publish a fresh warm target after a miss. Extend
            // the deadline for that target rather than aborting at the old one.
            if (in.pendingTarget != lastTarget
                && in.pendingTarget > in.committed)
            {
                lastTarget = in.pendingTarget;
                elapsedSamples = 0;
                deadlineSamples = scaledDeadline (in, hardCap, margin);
            }

            if (! reported)
            {
                result.reportLatency = true;
                result.reportVersion = version;
                result.reportSamples = targetLatency;
                reported = true;
            }

            // While the acked design sits in its 50 ms guard, the warm target
            // is allowed to pass: it is waiting for the switch, not missed.
            // Keep an absolute cap so a stuck guard can never gate forever.
            const bool missed = ! in.guardActive && ! in.warmReady
                             && (in.pendingTarget == 0
                                 || in.pendingTarget < in.committed);
            const bool expired = in.guardActive
                                   ? totalSamples > 2 * hardCap
                                   : (elapsedSamples > deadlineSamples
                                      || totalSamples > hardCap);

            if (missed || expired)
            {
                result.requestWarmRebuild = true;
                result.invalidateAck = true;
                result.reportLatency = true;
                result.reportVersion = 0;          // never ack a pending design
                result.reportSamples = in.activeBridgeLatency;
                result.gateHoldSamples = static_cast<int> (
                    std::max (1.0, 0.01 * in.sampleRate));

                active = false;
                reported = false;
                abortCooldownSamples = static_cast<int> (
                    std::max (0.02 * in.sampleRate, 1.0 * in.hostBlockSamples));
            }
        }

        result.gateClosed = active;
        return result;
    }

    // Post-processing: called with the latency of the FIR path actually
    // running this block. Returns true when the transition just completed and
    // supplies the short post-switch gate hold.
    bool notifyActiveLatency (int activeLatency, double sampleRate,
                              int& gateHoldSamplesOut) noexcept
    {
        gateHoldSamplesOut = 0;
        if (! active || activeLatency <= 0 || activeLatency != targetLatency)
            return false;

        active = false;
        reported = false;
        gateHoldSamplesOut = static_cast<int> (std::max (1.0, 0.02 * sampleRate));
        return true;
    }

    bool isActive() const noexcept  { return active; }

    void reset() noexcept
    {
        active = false;
        version = -1;
        targetLatency = 0;
        elapsedSamples = 0;
        deadlineSamples = 0;
        totalSamples = 0;
        lastTarget = 0;
        reported = false;
        abortCooldownSamples = 0;
    }

private:
    static int scaledDeadline (const BlockInput& in, int hardCap, int margin) noexcept
    {
        return static_cast<int> (
            std::min<uint64_t> (static_cast<uint64_t> (hardCap),
                                (in.pendingTarget - in.committed)
                                    + static_cast<uint64_t> (margin)));
    }

    void begin (const BlockInput& in, int hardCap, int margin) noexcept
    {
        active = true;
        version = in.designVersion;
        targetLatency = in.publishedLatency;
        elapsedSamples = 0;
        totalSamples = 0;
        lastTarget = in.pendingTarget;
        deadlineSamples = scaledDeadline (in, hardCap, margin);
        reported = false;
    }

    bool active = false;
    int version = -1;
    int targetLatency = 0;
    int elapsedSamples = 0;
    int deadlineSamples = 0;
    int totalSamples = 0;
    uint64_t lastTarget = 0;
    bool reported = false;
    int abortCooldownSamples = 0;
};

} // namespace SubEQ
