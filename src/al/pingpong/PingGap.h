// Gap-stop transition table for ASYNC mode.
//
// Extracted from Ping::loop so the entry/transition/
// resume logic is host-testable without the
// ARDUINO-gated Ping class. Pure decision: given
// the current gapSeq, the latest peer NAK seq
// (lastNak), and the latest peer ACK seq
// (lastAck), decide the next gapSeq and the
// action the caller should take (enter gap-stop,
// resume sending, update the tracked gap, or
// stay paused and continue draining rx).
//
// The "0xFF sentinel" semantics:
//   NO_GAP = 0xFF (default-init state, no NAK
//   ever observed since the last reset_unlocked)
//   0..254 = a real cobsSeq (COBS_SEQ_MAX is 0xFE).
//
// Resume test: `lastAck == gapSeq_`. After we
// enter gap-stop on a NAK for seq N, the link
// layer retransmits the missing chunk and Pong
// sends a 5-byte ACK (this release) carrying
// cobsSeq = N. The link's onAck stamps
// lastAckSeq_ = N, and `lastAck == gapSeq_`
// (i.e. N == N) resumes the sender.
//
// The entry guard `lastAck != lastNak` blocks
// the trivial race: if lastAck already equals
// the gap seq before we even entered (an ACK
// that predates the NAK — possible if the peer
// ACK'd chunk K, the link went quiet, then a
// later packet got lost and the peer NAK'd the
// same K because its reorder buffer still
// expected it), we skip the entry entirely
// rather than enter and immediately resume on
// the same iteration. Without the guard, the
// send loop would pause for zero loop
// iterations and the gap-stop feature would be
// inert.
#pragma once
#include <stdint.h>

namespace autolink {

constexpr uint8_t PING_GAP_NO_GAP = 0xFF;

enum class GapAction : uint8_t {
    // Stay in / enter gap-stop; caller should
    // drain rx, walk the ARQ table for any
    // ack-stamps that landed, and return
    // without sending new chunks.
    Stay,
    // First time we've seen this NAK from a
    // no-gap state; record it and emit the
    // "gap stop" warning log. Same downstream
    // behavior as Stay (no sends this loop).
    Enter,
    // A fresh NAK landed for a different gap
    // while we were already paused. Replace the
    // tracked gapSeq with the new one. Same
    // downstream behavior as Stay.
    Update,
    // The current gap's seq is now ACKed; clear
    // gapSeq_ to NO_GAP and let the caller
    // resume its send loop on the next
    // iteration.
    Resume,
};

// Pure transition. currentGap is gapSeq_ before
// the call; returns the new gapSeq_ the caller
// should store. The caller branches on the
// returned action to decide whether to emit a
// log line, drain rx, or resume sending.
inline GapAction decideGapTransition(uint8_t currentGap, uint8_t lastNak,
                                     uint8_t lastAck, uint8_t &nextGap) {
    // Entry edge: from NO_GAP, a NAK that's
    // not already ACKed → enter gap-stop.
    if (currentGap == PING_GAP_NO_GAP) {
        if (lastNak != PING_GAP_NO_GAP && lastAck != lastNak) {
            nextGap = lastNak;
            return GapAction::Enter;
        }
        nextGap = PING_GAP_NO_GAP;
        return GapAction::Stay;
    }
    // We're already in gap-stop. Update the
    // tracked seq if a fresh NAK for a
    // different seq arrived; resume if the
    // gap seq is now ACKed; otherwise stay.
    if (lastNak != PING_GAP_NO_GAP && lastNak != currentGap) {
        nextGap = lastNak;
        return GapAction::Update;
    }
    if (lastAck == currentGap) {
        nextGap = PING_GAP_NO_GAP;
        return GapAction::Resume;
    }
    nextGap = currentGap;
    return GapAction::Stay;
}

} // namespace autolink
