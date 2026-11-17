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
        bool guardActive = false;         // acked design is in its PDC guard
        bool warmReady = false;           // matching warm state is available
        bool gateAtZero = false;          // output gate has fully closed
        int catchUpLimitSamples = 512;    // audio-thread warm catch-up budget
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

        // The gate must cover: fade to zero, one 60 Hz timer tick, the PDC
        // guard and the post-switch hold. Keep it to a few blocks rather than
        // the whole warm-build lead. For very large host blocks one block is
        // already longer than 60 ms; that block floor is unavoidable.
        const int nearWindow = static_cast<int> (
            std::max (0.04 * in.sampleRate, 4.0 * in.hostBlockSamples));
        const int margin = static_cast<int> (
            std::max (0.06 * in.sampleRate, 5.0 * in.hostBlockSamples));
        // The handshake is block-quantized: begin, report (after gate zero),
        // ack+guard, then switch. A switch can legitimately need five host
        // blocks (one extra for timer/ack jitter), so the cap must leave room
        // for it; a four-block cap aborted large-buffer transitions on the
        // very block that would have adopted the design.
        const int hardCap = static_cast<int> (
            std::max (0.12 * in.sampleRate, 6.0 * in.hostBlockSamples));

        // Never start a transition for a design whose warm state is not ready,
        // or whose target is too stale for the bounded audio-side catch-up.
        const bool canTransition = in.firMode
                                && ! in.nonRealtime
                                && in.publishedLatency > 0
                                && in.publishedLatency != in.activeBridgeLatency
                                && abortCooldownSamples <= 0;

        // A future target can start the handshake early; a ready warm whose
        // target was just crossed can also start (the audio thread catches up
        // at most one block). A target older than that is not recoverable:
        // wait for the worker to publish a fresh future target instead of
        // gating the output.
        const bool targetInFuture = in.pendingTarget > in.committed;
        const bool targetRecoverable = ! targetInFuture
            && in.warmReady
            && in.pendingTarget != 0
            && (in.committed - in.pendingTarget)
                   <= static_cast<uint64_t> (std::max (1, in.catchUpLimitSamples));
        const bool targetUsable = targetInFuture
            ? (in.pendingTarget - in.committed) <= static_cast<uint64_t> (nearWindow)
            : targetRecoverable;

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

            // Report only after the gate has fully closed: the host may apply
            // the new PDC on the next block, and no mistimed sample may leak
            // while the gate is still partially open.
            if (! reported && in.gateAtZero)
            {
                result.reportLatency = true;
                result.reportVersion = version;
                result.reportSamples = targetLatency;
                reported = true;
            }

            // While the acked design sits in its 50 ms guard, the warm target
            // is allowed to pass: it is waiting for the switch, not missed.
            // Keep an absolute cap so a stuck guard can never gate forever.
            // Allow the warm builder a bounded grace period after the target:
            // it cannot finish the final replay before the boundary samples
            // exist, so a target that is one or two OLA blocks old is not a
            // miss yet.
            const bool missed = ! in.guardActive && ! in.warmReady
                             && (in.pendingTarget == 0
                                 || in.pendingTarget
                                        + static_cast<uint64_t>(
                                              std::max (1, in.catchUpLimitSamples))
                                      < in.committed);
            const bool expired = in.guardActive
                                   ? totalSamples > hardCap
                                   : (elapsedSamples > deadlineSamples
                                      || totalSamples > hardCap);

            if (missed || expired)
            {
                result.requestWarmRebuild = true;
                result.invalidateAck = true;
                result.reportLatency = true;
                result.reportVersion = 0;          // never ack a pending design
                result.reportSamples = in.activeBridgeLatency;
                // The restored latency is applied by the message-thread timer
                // (60 Hz, worst-case ~16.7 ms) and takes effect on the host's
                // next callback. A 10 ms hold could reopen the gate before the
                // host stops compensating the acked-but-unswitched latency.
                result.gateHoldSamples = static_cast<int> (
                    std::max (1.0, 0.05 * in.sampleRate));

                // If the deadline expired while waiting for an ack, the host
                // is not pumping the timer (or is extremely late). Do not
                // immediately re-enter the handshake and gate again: keep the
                // IIR bridge audible for a longer cooldown and retry later.
                const bool ackMissing = ! in.guardActive && in.warmReady
                                     && elapsedSamples > deadlineSamples;
                active = false;
                reported = false;
                abortCooldownSamples = ackMissing
                    ? static_cast<int> (std::max (0.50 * in.sampleRate,
                                                  2.0 * in.hostBlockSamples))
                    : static_cast<int> (std::max (0.02 * in.sampleRate,
                                                  1.0 * in.hostBlockSamples));
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
        gateHoldSamplesOut = static_cast<int> (std::max (1.0, 0.01 * sampleRate));
        return true;
    }

    bool isActive() const noexcept  { return active; }

#if SUBEQ_TESTING
    int getAbortCooldownForTest() const noexcept { return abortCooldownSamples; }
#endif

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
        const uint64_t distance = in.pendingTarget > in.committed
                                      ? in.pendingTarget - in.committed
                                      : 0;
        return static_cast<int> (
            std::min<uint64_t> (static_cast<uint64_t> (hardCap),
                                distance + static_cast<uint64_t> (margin)));
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
