// LinkDecision.h — pure decision functions for the AutoLink protocol.
//
// v5.1.40: split pure decision logic from I/O-bound methods. Pre-v5.1.40
// had decision logic (gap/stale classification, ARQ retransmit
// scheduling, baud-sweep tick decisions, LCK retry counting) hiding
// inside I/O methods (`onPayload`, `onTimerOk_unlocked`,
// `onTimerSwp_unlocked`, `onTimerLck_unlocked`). The `_unlocked`
// suffix convention was doing the job a type should do — the method
// was still on the same object as the I/O, so a caller could pass
// wrong state in. Pure functions that take a state struct and return
// an enum decision can\'t touch the lock, can\'t race, and can be
// table-driven tested without hardware or mocks.
//
// Each function:
//   - Takes its inputs as plain values or a state struct.
//   - Returns an enum (Action / GapClass / etc.) describing the decision.
//   - Performs NO I/O (no hw.* calls, no Log calls, no memory
//     allocation, no global state mutation).
//   - Side effects on the ALink instance are the CALLER\'s job
//     (counting gaps, incrementing spdI, etc.) — the function only
//     describes the decision.
//
// Convention: free functions, not methods. The state struct is
// passed by const reference where it\'s large (e.g. ArqSlotState for
// the ARQ tick). Single-value decisions take plain ints.

#ifndef AUTOLINK_LINK_DECISION_H
#define AUTOLINK_LINK_DECISION_H

#include <stdint.h>
#include <stddef.h>

namespace autolink {

// COBS sequence width. Defined here as a free constant for the
// decision functions (LinkDecision.h is the only header that needs
// it; ALink.h still has its own copy for the protocol\'s array sizes).
// v5.1.45: the cobsSeq counter skips ACK_TYPE (0xFF) so ACK and
// data frames can be discriminated by the first decoded byte alone.
// Effective wrap is 255 values per counter cycle, not 256.
static constexpr int LD_SEQ_WRAP = 255;

// ---- Gap / stale classification (was inline in onPayload) ----
//
// Given the just-arrived cobsSeq and the receiver\'s last-good seq:
//   - Forward: cobsSeq is the next expected seq (or rxSeq is unset).
//   - Stale:   cobsSeq is a duplicate or wraparound (already seen).
//   - Gap:     cobsSeq is forward but skips >=1 expected seqs.
//
// The caller (onPayload) handles the side effects:
//   - Forward: push to app buffer, ACK, update rxSeq.
//   - Stale:   drop, no ACK (sender already saw the first ACK).
//   - Gap:     push to app buffer, ACK, bump gaps/lostMsgs, MAY
//              trigger err-threshold drop.
enum class GapClass {
    Forward,
    Stale,
    Gap,
};

// Classify a received cobsSeq against the receiver\'s last-good seq.
// `diff` is set to the forward distance (1..255) for Gap or Stale;
// 0 for Forward. The caller uses diff to bump gaps+lostMsgs on a Gap
// and to log the distance on a Stale.
//
// Pure function — no I/O, no log calls, no state mutation.
inline GapClass classifyGap(uint8_t cobsSeq,
                            uint8_t rxSeq,
                            bool    rxSeqSet,
                            int*    outDiff = nullptr) {
    if (!rxSeqSet) {
        if (outDiff) *outDiff = 0;
        return GapClass::Forward;
    }
    // v5.1.45: cobsSeq counter skips ACK_TYPE (0xFF), effective wrap=255.
    // Next expected after rxSeq=254 is 0 (wraparound), not 255.
    // The mod-256 wrap at rxSeq=255 would be the unused reserved value.
    uint8_t expected = (rxSeq == LD_SEQ_WRAP - 1) ? 0 : (uint8_t)(rxSeq + 1);
    if (cobsSeq == expected) {
        if (outDiff) *outDiff = 0;
        return GapClass::Forward;
    }
    int diff = (int)cobsSeq - (int)rxSeq;
    if (diff < 0) diff += LD_SEQ_WRAP;
    if (outDiff) *outDiff = diff;
    if (diff == 0 || diff > LD_SEQ_WRAP / 2) {
        // Duplicate or wraparound (large diff after mod).
        return GapClass::Stale;
    }
    return GapClass::Gap;
}

// ---- ARQ retransmit scheduling (was inline in onTimerOk_unlocked) ----
//
// For each cobsSeq in the ARQ map, decide whether to retransmit, hold,
// or drop the link. The decision depends on:
//   - nowMs - sentAtMs: age of the slot.
//   - retxCount: how many retransmits already attempted.
//   - ackRtoMs:    the retransmit-timeout threshold.
//   - maxRetx:     how many retransmits before giving up.
enum class ArqAction {
    Hold,    // not yet expired
    Retx,    // expired, retransmit
    Drop,    // exhausted MAX_RETX, drop link
};

// Decide what to do with one ARQ slot. Pure function.
//
// `ageMs` is `nowMs - sentAtMs` (caller computes under lock).
// `retxCount` is the current retry count for this seq.
// `ackRtoMs` is the retransmit timeout (cfg-driven).
// `maxRetx` is the cap (MAX_RETX in production).
inline ArqAction decideArqSlot(uint32_t ageMs,
                              uint8_t  retxCount,
                              uint32_t ackRtoMs,
                              uint8_t  maxRetx) {
    if (ageMs < ackRtoMs) return ArqAction::Hold;
    if (retxCount >= maxRetx) return ArqAction::Drop;
    return ArqAction::Retx;
}

// ---- Baud-sweep tick decision (was inline in onTimerSwp_unlocked) ----
//
// Given the current spdI (baud index), pingSample (count within a
// baud), and the protocol\'s samplesPerBaud, decide what the master
// should do next:
//   - SendPingSame:    send PING at the current baud, stay on it.
//   - SendPingAdvance: send PING and advance to the next baud.
//   - EnterLck:        all bauds swept, enter LCK.
//   - RestartSweep:    LCK retries exhausted, re-sweep from baud[0].
enum class SwpAction {
    SendPingSame,
    SendPingAdvance,
    EnterLck,
    RestartSweep,
};

// Decide what the master should do in the next SWP tick.
// `samplesPerBaud` is the number of PING samples per baud index
// (1 = fast, >1 = stable). `spdI >= baudCount` means the master has
// exhausted the baud list and should enter LCK.
//
// Pure function.
inline SwpAction decideSwpTick(int spdI,
                              int baudCount,
                              int pingSample,
                              int samplesPerBaud,
                              bool lckExhausted) {
    if (lckExhausted) return SwpAction::RestartSweep;
    if (spdI >= baudCount) return SwpAction::EnterLck;
    if (pingSample + 1 >= samplesPerBaud) return SwpAction::SendPingAdvance;
    return SwpAction::SendPingSame;
}

// ---- LCK retry decision (was inline in onTimerLck_unlocked) ----
//
// In LCK state, the master sends REQ to lock the baud. If no reply
// after `maxRetries` attempts, drop the link and re-sweep. The
// production `maxRetries` is `cfg.allowedBaudsCount * 2`.
enum class LckAction {
    SendReq,
    DropAndResweep,
};

// Decide what to do in the next LCK tick.
// `maxRetries` is `cfg.allowedBaudsCount * 2` (production).
inline LckAction decideLckTick(int lckRetries, int maxRetries) {
    if (lckRetries > maxRetries) return LckAction::DropAndResweep;
    return LckAction::SendReq;
}

// ---- Idle watchdog decision (was inline in onTimerOk_unlocked) ----
//
// In OK state, decide whether to drop the link based on how long
// since the last RX. The keepalive (sent when (now - lastTxMs) >=
// idleTimeoutMs / 3) resets lastTxMs but NOT lastRxMs, so the
// idle watchdog is purely a receive-side check.
enum class IdleAction {
    Hold,
    Drop,
};

// Decide whether the OK-state idle watchdog should drop the link.
// `ageMs` is `nowMs - lastRxMs`.
inline IdleAction decideIdleWatchdog(uint32_t ageMs, int idleTimeoutMs) {
    if (idleTimeoutMs <= 0) return IdleAction::Hold;  // disabled
    if (ageMs > (uint32_t)idleTimeoutMs) return IdleAction::Drop;
    return IdleAction::Hold;
}

// ---- Keepalive decision (was inline in onTimerOk_unlocked) ----
//
// In OK state, decide whether to emit a keepalive (a 1-byte
// cobsSeq-bearing 0-payload frame). Keepalive fires every
// idleTimeoutMs/3 of TX silence.
enum class KeepaliveAction {
    Hold,
    Emit,
};

// Decide whether to emit a keepalive this tick.
// `txAgeMs` is `nowMs - lastTxMs`. linkPaused suppresses keepalive
// (v5.1.31: operator inspection window).
inline KeepaliveAction decideKeepalive(uint32_t txAgeMs,
                                       int      idleTimeoutMs,
                                       bool     linkPaused) {
    if (linkPaused) return KeepaliveAction::Hold;
    if (idleTimeoutMs <= 0) return KeepaliveAction::Hold;
    if (txAgeMs >= (uint32_t)(idleTimeoutMs / 3)) return KeepaliveAction::Emit;
    return KeepaliveAction::Hold;
}

// ---- App-buffer-full hold decision (was inline in onPayload) ----
//
// When the receiver\'s app buffer is full and a chunk arrives, the
// protocol holds the ACK so the sender retransmits. When the app
// drains, the next attempt delivers fully and we ACK.
//
// `appBufAvailable` and `n` (incoming bytes) determine the decision.
enum class AppBufAction {
    Accept,  // app buffer has room; push and ACK
    HoldAck,  // app buffer full; don\'t push, hold ACK
};

// Decide what to do with an arriving chunk given app-buffer capacity.
// `available` is the app buffer\'s current available bytes; `incoming`
// is the chunk\'s payload length.
// Decide whether to hold the ACK after pushing to the app buffer.
// `accepted` is the number of bytes actually pushed (may be less
// than `incoming` if the buffer was full). HoldAck when the push
// was partial — the sender would free the cache slot if we ACK
// a frame we didn't fully deliver, and a subsequent retransmit
// would then collide.
inline AppBufAction decideAppBuf(int accepted, int incoming) {
    return (accepted < incoming) ? AppBufAction::HoldAck : AppBufAction::Accept;
}

}  // namespace autolink

#endif  // AUTOLINK_LINK_DECISION_H
