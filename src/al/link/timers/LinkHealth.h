
#ifndef AUTOLINK_LINK_HEALTH_H
#define AUTOLINK_LINK_HEALTH_H
#include <stdint.h>

namespace autolink {

struct HealthState {
    uint32_t rejFirstMs = 0;
    uint32_t rejLastMs = 0;
    // h.lastRxMs is the CRC-validated RX clock
    // (shadow of Link::lastValidRxMs). The health
    // machine reads this as the rxAge baseline.
    // Pinned by LastValidRxMsTest.
    uint32_t lastRxMs = 0;
    uint32_t lastTxMs = 0;
    // Raw any-byte RX clock (shadow of
    // Link::lastRxMs). Carried alongside
    // lastRxMs so a warning log can show
    // whether the fire was a clock-stale event
    // (lastRxMs fresh, lastValidRxMs stale) vs
    // genuine dead-link (both stale). Not
    // consulted by the watchdogs directly —
    // the gating below is `h.lastRxMs != 0`,
    // which is the CRC-validated clock, so a
    // noise-byte heart-beat can't satisfy
    // rxAge. Pinned by LastValidRxMsTest and
    // the new log-hygiene pin.
    uint32_t lastAnyRxMs = 0;

    // ARQ RTO; watchdogs wait out 2 x RTO before treating one-sided
    // silence as a gone peer (retx needs a chance to draw a reply).
    uint32_t rtoMs = 0;
    int pending = 0;
    // The CRC-valid RX clock (already mirrored from
    // Link::lastValidRxMs as h.lastRxMs). The peer-stalled
    // watchdog needs an independent ack-direction stamp
    // so a busy sender (the slave's own echo traffic
    // refreshing lastTxMs) doesn't suppress the
    // single-sided dead-peer check. The watchdog fires
    // when (now - ackRxMs) > peerStalledMs AND pending > 0
    // — the peer's ACKs are overdue by a full window's
    // drain at the locked baud. Pinned by
    // PeerStalledSlavesBaudDerivedTest.
    uint32_t ackRxMs = 0;
    int peerStalledMs = 0;
    uint64_t frameErrs = 0;
    bool poolFull = false;
    bool sync = false;
    // Packed GBN-retx status mirrored from
    // Link (applyHealth_unlocked fills it from
    // the link's own fields). Used by the
    // DropAsymIdle gate to Keep the link
    // while a retransmit repair is in flight
    // (the watchdog's 2xRTO horizon is shorter
    // than the ARQ layer's
    // maxRetx * syncAckTimeoutMs budget, and
    // a mid-repair drop leaves a stuck base).
    // Pinned by LinkHealthTest.
    bool gbnActive = false;
    // GBN attempts since the base last advanced.
    // The DropAsymIdle suppression only holds
    // while retx budget remains; once the
    // caller has burned through `maxRetx`
    // attempts without progress, the
    // sweepRetx_unlocked path drives an
    // honest GbnMaxRetx drop. The caller
    // passes a pre-computed `retxBudgetOpen`
    // bool so this pure decision helper
    // doesn't have to know about cfg.
    int gbnAttempts = 0;
    bool gbnBudgetOpen = false;
};

enum class HealthAction : uint8_t {
    Keep,
    DropTxStall,
    DropAsymIdle,
    DropIdle,
    DropPoolExhaust,
    DropDeadLink,
    DropSilentPeer,
    DropPeerBaudMismatch,
    // The peer stopped talking while we still owe
    // (or are owed) ACKs: pending>0 with no
    // CRC-valid frame from the peer for the baud-
    // derived peer-stalled window. The old
    // DropDeadLink only fired on full mutual
    // quiet (rxAge > idle AND txAge > idle), but
    // the slave's own echo traffic keeps txAge
    // fresh and the watchdog never trips. Pin:
    // PeerStalledSlavesBaudDerivedTest.
    DropPeerStalled,
};

inline constexpr int healthFastIdleRxMs() { return 300; }
inline constexpr int healthFastIdleTxMs() { return 1000; }

// Baud-aware fast-idle floor: at 9600 baud a single
// 250-byte chunk is ~26 ms of wire time, so 300 ms of RX
// silence is plausibly a quiet link. At 512000 baud the
// same chunk is ~0.5 ms — 300 ms of silence IS drop-worthy.
// Caller passes the rtoMs at the locked baud; the floor is
// the rtoMs * 2, matching the 2 x RTO horizon the asym gate
// already uses. Pinned by LinkHealthBaudAwareTest.
inline int healthFastIdleRxMsAtBaud(uint32_t rtoMs) {
    int fromRto = (int)(2 * rtoMs);
    int floor = healthFastIdleRxMs();
    return fromRto > floor ? fromRto : floor;
}

// idleTimeoutMs == 0 disables the silent-peer watchdog (and the
// OK-state keepalive) by contract; the caller owns that gate, so
// this helper carries no floor. Pinned by LinkHealthTest.
inline constexpr int healthDeadPeerMs(int idleTimeoutMs) {
    return 3 * idleTimeoutMs;
}

// Baud-derived peer-stalled window: the time a
// locked-but-pending link waits for the peer's
// next frame before declaring the peer gone. The
// single-sided shape (we have un-ACKed chunks
// AND the peer hasn't spoken to us for the
// window) is the slave's echo-traffic wedge —
// the slave is busy sending, so DropDeadLink's
// mutual-quiet gate never trips, and the field
// log showed the slave sitting in OK for 10.5 s
// after the master stopped ACKing. The threshold
// is the bigger of (a) a 2 s floor (any peer that
// goes silent for 2 s is dead, regardless of
// baud) and (b) k * windowDrainMs, where the
// drain is the time a full 32-chunk 250 B window
// takes to fully serialise at the locked baud —
// the wire-bound floor for "the peer is at most
// one window behind". k=2 gives 2x the drain
// horizon: one full window to transmit, one full
// window for the peer's ACK to land. Pinned by
// PeerStalledSlavesBaudDerivedTest.
inline int healthPeerStalledMs(uint32_t baud, int pending) {
    constexpr int kFloor = 2000;
    constexpr int kMult = 2;
    if (baud == 0 || pending <= 0)
        return kFloor;
    constexpr int kMaxChunk = 250;
    constexpr int kHdr = 6;
    int bytes = pending * (kMaxChunk + kHdr);
    long drain = (long)bytes * 10L * 1000L / (long)baud;
    int windowMs = (int)(drain * kMult);
    if (windowMs < kFloor)
        return kFloor;
    return windowMs;
}

inline HealthAction decideHealth(const HealthState &h, uint32_t now,
                                 int idleTimeoutMs, int deadPeerMs,
                                 uint32_t rtoMs) {
    uint32_t idle = (uint32_t)idleTimeoutMs;
    if (h.rejFirstMs != 0 && (uint32_t)(now - h.rejFirstMs) > idle &&
        (uint32_t)(now - h.rejLastMs) <= idle)
        return HealthAction::DropTxStall;
    uint32_t rxAge = now - h.lastRxMs;
    uint32_t txAge = now - h.lastTxMs;
    // Quiet both ways past the idle window WITH an in-flight
    // message is dead, not idle. This is the SYNC backstop:
    // SYNC short-circuits to Keep below, so DropIdle never
    // reaches it.
    if (h.pending > 0 && rxAge > idle && txAge > idle)
        return HealthAction::DropDeadLink;
    // Every other watchdog gates on pending>0 or frameErrs>0, so
    // none of them see a locked link with an empty pipeline whose
    // peer has simply gone quiet. lastRxMs != 0 means the link was
    // alive at least once.
    if (h.lastRxMs != 0 && rxAge > (uint32_t)deadPeerMs)
        return HealthAction::DropSilentPeer;
    if (h.sync)
        return HealthAction::Keep;

    // Baud-aware fast-idle floor: 2 x rtoMs at the locked baud,
    // never less than the 300 ms legacy floor. At 512000 this
    // stays at 300; at 9600 the rtoMs-derived floor lifts it to
    // ~4 ms (well, stays at 300 since rtoMs ~ 2 ms at 9600
    // makes 2*rtoMs < 300). For a 4-byte payload at 9600 the
    // wire time is 4.2 ms; 300 ms is generous. The binding
    // constraint is the 2 x rtoMs horizon, which is the
    // existing peer-silence threshold.
    int rxIdleFloor = healthFastIdleRxMsAtBaud(rtoMs);
    if (h.lastRxMs != 0 && rxAge > (uint32_t)rxIdleFloor && rxAge > 2 * rtoMs &&
        txAge < (uint32_t)healthFastIdleTxMs() && h.pending > 0) {
        // The watchdog's 2xRTO horizon is shorter than the
        // ARQ layer's maxRetx * syncAckTimeoutMs budget
        // (2.5 s at the defaults), so a mid-repair drop
        // leaves a stuck base. Suppress DropAsymIdle while
        // GBN retx is in flight with budget remaining;
        // GbnMaxRetx (driven by sweepRetx_unlocked) is the
        // authority for a peer that never answers retx.
        // Pinned by LinkHealthTest Pin 6.
        if (h.gbnActive && h.gbnBudgetOpen)
            return HealthAction::Keep;
        return HealthAction::DropAsymIdle;
    }
    if (h.lastRxMs != 0 && rxAge > idle && txAge > idle &&
        (h.pending > 0 || h.frameErrs > 0))
        return HealthAction::DropIdle;

    // Pool full + live RX is routine flood backpressure; drop only
    // when RX is silent past the repair horizon.
    if (h.lastRxMs != 0 && h.poolFull && h.pending > 0 &&
        rxAge > (uint32_t)rxIdleFloor && rxAge > 2 * rtoMs)
        return HealthAction::DropPoolExhaust;
    // Peer-stalled watchdog (single-sided dead peer):
    // pending>0 (we owe or are owed ACKs) AND the
    // peer's last CRC-valid frame is past the
    // baud-derived peer-stalled window. The slave's
    // echo traffic keeps txAge fresh so DropDeadLink
    // never trips; this check uses the ack-direction
    // stamp (ackRxMs) which ages on EITHER side
    // going quiet, and the baud-derived window so
    // 512000 (drain ~ms) gets a 2s verdict and 9600
    // (drain ~10s) gets a ~20s verdict — the peer's
    // own drain time, not a flat 30s. Pinned by
    // PeerStalledSlavesBaudDerivedTest.
    if (h.pending > 0 && h.ackRxMs != 0 && h.peerStalledMs > 0 &&
        (uint32_t)(now - h.ackRxMs) > (uint32_t)h.peerStalledMs)
        return HealthAction::DropPeerStalled;
    return HealthAction::Keep;
}

// Pure decision for the peer-baud-mismatch escalation: when
// lockedWithoutRecv climbs past the threshold, escalate via a
// different health action so the next reset falls through to a
// full P1 walk instead of re-running the same top-down sweep
// that just failed. Pinned by PeerBaudMismatchTest.
inline bool shouldEscalatePeerBaudMismatch(int locksWithoutRecv,
                                           int threshold) {
    return locksWithoutRecv >= threshold;
}

} // namespace autolink
#endif
