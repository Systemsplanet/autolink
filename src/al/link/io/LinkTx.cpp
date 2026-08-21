
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/util/log/Log.h"
#include "al/util/codec/UtilCobs.h"
#include "al/util/codec/UtilCrc.h"
#include <algorithm>
#include <cstring>

static constexpr const char *TAG = "AutoLink";

namespace autolink {
static_assert(
    MAX_CHUNK + MSG_HDR <= ArqCache::POOL_BUF_MAX,
    "MAX_CHUNK + MSG_HDR > ArqCache::POOL_BUF_MAX: bump POOL_BUF_MAX or shrink MAX_CHUNK");

void Link::sendPongAck_unlocked() { sendSweepFrame_unlocked(PONG_CMD); }

// The only sender of the 5-byte control-frame shape (preamble +
// seq/epoch byte + payload + CRC) reachable from production code —
// byte[2] always carries sweepEpoch_, both during the sweep phase
// and from OK-state's own periodic keepalive PING
// (LinkTimersOk.cpp). Every receiver path that reads this byte
// (handleSwp_unlocked, the OK-state PING-epoch check in
// processCtrlFrame_unlocked) can assume epoch semantics unconditionally
// for exactly that reason — there is no second sender putting a
// different meaning on the same byte for it to collide with.
void Link::sendSweepFrame_unlocked(uint8_t payload) {
    uint8_t frame[CTRL_FRAME_SIZE];
    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[CTRL_FRAME_SEQ_IDX] = sweepEpoch_;
    frame[CTRL_FRAME_PAYLOAD_IDX] = payload;
    frame[CTRL_FRAME_CRC_IDX] = UtilCrc::crc8(frame, CTRL_FRAME_SIZE - 1);
    if (hw.tx(frame, CTRL_FRAME_SIZE) != CTRL_FRAME_SIZE)
        Log::log().error(TAG, "sendSweepFrame truncated");
    else
        Log::log().debug(TAG, "wire SWEEP epoch=%u payload=0x%02X",
                         (unsigned)sweepEpoch_, (unsigned)payload);

    txBytes += CTRL_FRAME_SIZE;
    lastTxMs = hw.nowMs();
}

// F1 + F2 + F3 + F5 + F6: buildAndTxCobsFrame_unlocked
// is the load-bearing wire-write primitive. Three
// properties the previous shape violated:
//   (1) It dropped the link lock while draining
//       the TX ring — onRx → onNak →
//       gbnResendWindow_unlocked → retxSeq_unlocked
//       → resendCobsFrame_unlocked → here. The
//       drop let onTimer → reset_unlocked run
//       under us and wipe gbnBase_ (same
//       defect class as D1, one layer down).
//   (2) It void-returned. A ring-stall abort
//       looked identical to a success at every
//       call site; txSeq / txBytes / arq_.onSent
//       advanced on a frame that never went out,
//       burning a seq with nothing on the wire —
//       a permanent gap the peer would NAK.
//   (3) It logged the abort but never bumped
//       txRingStallDrops_ — every SYNC / retx
//       stall was invisible to the field-log
//       diagnostic this release exists to
//       provide.
//
// New contract: returns true on a successful
// wire write, false on a ring-stall refusal.
// The pre-condition is the same kWorstCaseCobsFrame
// gate the ASYNC multi-chunk loop uses, but
// here it's a single read (no drain). The ASYNC
// loop and gbnResendWindow_unlocked already gate
// before calling in, so this read is the
// final check; a true race (the ring drains
// between the gate and the write) is rare and
// the caller's bool-return propagates it back.
// G8: bind the frame buffer to the COBS
// worst-case bound. The buffer is
// `MAX_CHUNK + MSG_HDR` = 256 B and the
// worst case the encoder can produce
// (rawLen = MAX_CHUNK + 2, COBS worst-
// case 1:254 expansion, 1 preamble, 1
// delim) is at most 255 B. The current
// shape is safe; the static_assert
// enforces the relationship so a future
// change to MAX_CHUNK or MSG_HDR that
// pushes the worst case above 256 B
// fails to compile. Pinned by
// BuildAndTxGateBeforeEncodeTest
// (the buildAndTx function binds
// the gate to the encode, and the
// static_assert is the compile-time
// backstop).
static_assert(1 + (MAX_CHUNK + 2) + (((MAX_CHUNK + 2) + 254 - 1) / 254) + 1 <=
                  MAX_CHUNK + MSG_HDR,
              "frame[] buffer too small for COBS worst-case expansion; "
              "raise MAX_CHUNK or MSG_HDR");

// The SYNC side (sendMsg's SYNC branch +
// syncRtoStep_unlocked) gates before calling
// in too, with a proper drain that DOES drop
// the link lock at the only point the lock
// drop is safe (the SYNC ladder already has
// the unlock-between-segments protocol).
bool Link::buildAndTxCobsFrame_unlocked(uint8_t seq, const uint8_t *b, int n) {
    // G6: gate on txAvail FIRST. On a stalled ring under a retx burst this is
    // up to 8 wasted CRC + COBS encodes per sweep tick — the encode is a
    // tight loop over the chunk (250 bytes) and the sweep fires every
    // gbnResendBurstMax*period ms. Hoist the ring check to before the encode
    // so a stall returns false with one read and zero work. Pinned by
    // BuildAndTxGateBeforeEncodeTest.
    if (hw.txAvail() < kWorstCaseCobsFrame) {
        txRingStallDrops_++;
        // AL92-4: this is the same TX-side
        // "couldn't get a frame onto the wire"
        // signal drainTxRing_unlocked's RTO-abort
        // branch stamps (LinkTimersOk.cpp) — both
        // increment txRingStallDrops_, only this
        // one left txRejFirstMs_/txRejLastMs_
        // untouched. decideHealth's DropTxStall
        // verdict (LinkHealth.h) reads that window
        // to catch a SUSTAINED stall; a caller that
        // hits this specific gate repeatedly (e.g.
        // the GBN resend burst loop retrying into a
        // still-stalled ring) was invisible to it.
        // Deliberately NOT added to the ACK-ladder-
        // exhausted branch in sendMsg (LinkApi.cpp)
        // — that path's frame already went out
        // successfully (txBytes/arq_.onSent already
        // ran); the peer simply isn't ACKing, which
        // is a different signal decideHealth already
        // covers via rxAge/lastRxMs
        // (DropAsymIdle and neighbors). Stamping a
        // TX-reject there would conflate "couldn't
        // transmit" with "wasn't acknowledged" and
        // could mis-fire DropTxStall on a healthy
        // TX path with a silent peer.
        noteTxReject_unlocked();
        Log::log().warning(TAG,
                           "buildAndTx: TX ring stall (free=%d, perFrame=%d) — "
                           "frame NOT built, txSeq NOT advanced",
                           hw.txAvail(), kWorstCaseCobsFrame);
        return false;
    }
    if (n < 0)
        n = 0;
    if (n > MAX_CHUNK)
        n = MAX_CHUNK;
    uint8_t unenc[MAX_CHUNK + 3];
    unenc[0] = seq;
    if (n > 0)
        memcpy(unenc + 1, b, n);
    unenc[1 + n] = UtilCrc::crc8(unenc, 1 + n);
    size_t rawLen = (size_t)(1 + n) + 1;
    uint8_t frame[MAX_CHUNK + MSG_HDR];
    frame[0] = 0x00;
    size_t encLen = UtilCobs::encode(unenc, rawLen, frame + 1);
    frame[1 + encLen] = 0x00;
    int frameBytes = (int)(encLen + 2);
    // F5: a single txAvail() read here. The
    // ASYNC sendMsg loop and the GBN resend
    // burst loop have already gated on this
    // value before calling in. The remaining
    // race window (the ring drains between the
    // caller's gate and this read) is a true
    // race, not a steady-state condition —
    // the bool return lets it propagate. A
    // SYNC stall here is rare; the SYNC ladder's
    // own gate (syncRtoStep_unlocked +
    // sendMsg's SYNC branch) is the load-bearing
    // pre-check. G6: the txAvail gate
    // is at the top of the function
    // (before the encode). H3: the
    // belt-and-braces second check
    // that lived here was dead code
    // (nothing unlocks between the
    // two reads, so a wire-state
    // shift mid-encode is impossible
    // on the host HAL and equally
    // impossible on the ESP32
    // hardware HAL — the wire path
    // is single-threaded under
    // hw.lock()). The duplicate
    // would have double-counted
    // txRingStallDrops_ on any
    // path that ever became
    // reachable, so the only
    // safe shape is one txAvail
    // read per function call. The
    // function holds the link
    // lock; the G6 top-of-function
    // read is the contract.
    hw.tx(frame, frameBytes);
    return true;
}

bool Link::sendCobsFrame_unlocked(const uint8_t *b, int n) {
    uint8_t seq = txSeq;
    // F1 + F2: the build-and-tx returns false
    // on a ring-stall refusal. Do not advance
    // txSeq / txSeqLap_ on a refusal — a "burned
    // seq with nothing on the wire" hides a
    // permanent gap the peer would NAK forever.
    // The peer sees a missing chunk in the
    // seq stream and NAKs it; we re-emit the
    // same seq on the next slot. The seq is
    // re-used, not skipped.
    if (!buildAndTxCobsFrame_unlocked(seq, b, n))
        return false;

    // F9: reworded from "256-lap roll" to the
    // 254-value wire space (COBS_SEQ_MAX = 0xFD).
    // The seq namespace is 8 bits but only
    // 0..0xFD are valid on the wire; the
    // wrap is at 0xFD, not 0xFF. A seq reused
    // after 254 chunks (not 256) would alias
    // an earlier message's bytesRecvd_ walk
    // without the lap qualifier. Pinned by
    // BytesForMessageLapQualifierTest +
    // SendMsgReturnsBaseLapTest.
    if (txSeq == COBS_SEQ_MAX) {
        txSeq = 0;
        txSeqLap_++;
    } else {
        txSeq = (uint8_t)(txSeq + 1);
    }
    // COBS chunk sent: per-async-pipeline rate would flood
    // (MAX_TX_PER_LOOP per loop = hundreds/sec at ASYNC). Verbose
    // captures the seq + bytes for deep-trace only; the
    // SYNC/ASYNC distinction plus the wire-recvd companion in
    // Ping already gives the operator the per-chunk shape
    // without lifting to info. Per-frame trace;
    // default-compiled-out. Pinned by
    // WireTraceOffByDefaultTest.
#ifdef AUTOLINK_TRACE_WIRE
    Log::log().verbose(TAG, "wire COBS seq=%u n=%d", (unsigned)seq, n);
#endif
    return true;
}

uint8_t Link::sendCobsFrameAcked_unlocked(const uint8_t *b, int n,
                                          uint8_t baseSeq) {
    uint8_t seq = txSeq;
    // Pool-exhaust guard (defect 6 in the field-log
    // analysis): arqCache_.insert() silently no-ops on
    // pool exhaustion — logs and returns. Earlier
    // shape already called arq_.onSent() at this point,
    // which stamped the seq as pending with no bytes in
    // the cache; retxSeq_unlocked's cache-miss path then
    // resends nothing while applyRetx keeps re-stamping
    // sentAtMs_, so the slot never times out either.
    // Hoist the room check above the wire write so a
    // refused insertion never leaves a pending-with-no-
    // bytes ghost in the ARQ.
    if (n > 0 && !arqCache_.hasRoom()) {
        // Caller (LinkApi.cpp's multi-chunk loop) catches
        // this and breaks; the diagnostic here is for
        // the single-chunk path which has no such
        // backstop. Bump poolExhaustDrops_ so the
        // operator's stats line attributes the silent
        // miss to the right cause.
        poolExhaustDrops_++;
        Log::log().warning(
            TAG,
            "sendCobsFrameAcked: arqCache pool exhausted before "
            "insert (cobsSeq=%u) — chunk refused, pending NOT set",
            (unsigned)seq);
        return 0xFF;
    }
    // F2: sendCobsFrame_unlocked returns false on a wire-stall. Propagate
    // 0xFF so the caller treats the chunk as not-sent (no seq advance, no
    // onSent, no cache insert).
    if (!sendCobsFrame_unlocked(b, n)) {
        return 0xFF;
    }
    if (cfg.mode != AutoLinkConfig::Mode::SYNC && !arq_.gbnActive()) {
        arq_.setGbnBase(seq);
        gbnAttempts_ = 0;
        gbnBackoffMs_ = 0;
        gbnLastRetxBase_ = 0xFF;
        consecutiveKeep_ = 0;
        arq_.setGbnActive(true);
    }
    arq_.onSent(seq, baseSeq, hw.nowMs(), txSeqLap_);
    if (n > 0)
        arqCache_.insert(seq, b, n);
    return seq;
}

bool Link::resendCobsFrame_unlocked(uint8_t seq, const uint8_t *b, int n) {
    // F2: buildAndTxCobsFrame_unlocked returns
    // false on a wire-stall. The GBN resend
    // burst loop in LinkTimerBreak.cpp calls
    // this in a tight loop; a false return
    // breaks the burst (caller's bool return
    // is honoured) without advancing txBytes
    // or lastTxMs for a frame that never made
    // it. Pinned by
    // ResendCobsFramePropagatesStallTest.
    if (!buildAndTxCobsFrame_unlocked(seq, b, n))
        return false;

    if (n > 0)
        txBytes += (uint64_t)n;
    lastTxMs = hw.nowMs();
    return true;
}

// CRC8-tail + COBS-encode + tx a small control payload, counting the
// wire bytes (ACK/NAK traffic dominates Pong's tx rate).
void Link::txSmallCobs_unlocked(uint8_t *u, size_t rawLen) {
    // G3: gate on txAvail. The ACK/NAK path runs from the onRx handler (peer-
    // ACK arrival → sendAckFrame, peer-NAK arrival → sendNakFrame). On a
    // stall: bump txRingStallDrops_, skip the wire write, and let the next
    // inbound frame re-derive the ACK (the slot is still in the ARQ's pending
    // set; a duplicate ACK on the next round is idempotent). Pinned by
    // AckNakDoesNotBlockOnRxTest (the test forces a 4-byte ring and confirms
    // the gate fires). H4: the gate must use the post-encode byte count, not
    // the pre-encode rawLen. COBS expands any rawLen >= 1 by ceil(rawLen /
    // 254) extra bytes (1:254 worst case) — a 3-byte ACK produces a 4-byte
    // COBS payload (rawLen + 1) and a 5-byte ACK produces a 6-byte payload.
    // Use the post-encode value to gate (or +3 to be safe on every rawLen >=
    // 1). The encode is tiny (5 bytes), so paying it before the gate is
    // cheaper than the bug class of "admit 1 byte short".
    u[rawLen - 1] = UtilCrc::crc8(u, rawLen - 1);
    uint8_t frame[16];
    frame[0] = 0x00;
    // I6: the encode here
    // deliberately runs BEFORE
    // the txAvail gate — the
    // opposite of the H3
    // "gate first" rule in
    // buildAndTxCobsFrame_unlocked.
    // Reason: txSmallCobs is
    // called on small fixed-size
    // payloads (ACK=5, NAK=3,
    // ctrl=3) where the post-
    // encode byte count is
    // unknowable without running
    // the encoder — COBS expands
    // any rawLen >= 1 by one
    // extra byte (1:254 worst
    // case). buildAndTx's
    // worst-case bound
    // (kWorstCaseCobsFrame)
    // doesn't help here (the
    // gate is per-payload, not
    // per-link). The encode is
    // 5 bytes of work; the
    // "admit 1 byte short" bug
    // class (H4) is the cost of
    // skipping it. Pinned by
    // AckNakDoesNotBlockOnRxTest.
    size_t el = UtilCobs::encode(u, rawLen, frame + 1);
    frame[1 + el] = 0x00;
    int frameBytes = (int)(el + 2);
    if (hw.txAvail() < frameBytes) {
        txRingStallDrops_++;
        Log::log().warning(TAG,
                           "txSmallCobs: TX ring stall on ACK/NAK (free=%d, "
                           "frame=%d) — drop, next inbound frame re-derives",
                           hw.txAvail(), frameBytes);
        return;
    }
    hw.tx(frame, frameBytes);
    txBytes += (uint64_t)frameBytes;
    lastTxMs = hw.nowMs();
}

void Link::sendCtrlCobsFrame_unlocked(uint8_t type, uint8_t seq) {
    uint8_t u[3] = { type, seq, 0 };
    txSmallCobs_unlocked(u, 3);
}

void Link::sendAckFrame_unlocked(uint8_t ackedCobsSeq, uint16_t bytesRecvd) {
    uint8_t u[5] = { ACK_TYPE, ackedCobsSeq, (uint8_t)(bytesRecvd & 0xFF),
                     (uint8_t)((bytesRecvd >> 8) & 0xFF), 0 };
    txSmallCobs_unlocked(u, 5);
    // Per-second aggregate (Stats.acksSent) — wire-op
    // result, per AGENTS rule 16. The per-frame verbose
    // line below is default-compiled-out so the field
    // operator gets the count without the per-ACK flood.
    // Pinned by StatsAckNakCountersTest.
    acksSent_++;
    // Pong's primary TX path: per-chunk-ack rate is ASYNC
    // pipeline rate. Verbose, not info, for the same reason as
    // sendCobsFrame_unlocked above. Per-frame trace;
    // default-compiled-out (see onPayload's field
    // comment). Pinned by WireTraceOffByDefaultTest.
#ifdef AUTOLINK_TRACE_WIRE
    Log::log().verbose(TAG, "wire ACK seq=%u bytesRecvd=%u",
                       (unsigned)ackedCobsSeq, (unsigned)bytesRecvd);
#endif
}

void Link::sendNakFrame_unlocked(uint8_t missingCobsSeq) {
    sendCtrlCobsFrame_unlocked(NAK_TYPE, missingCobsSeq);
    // Per-second aggregate. Pinned by
    // StatsAckNakCountersTest.
    naksSent_++;
    // NAK on the wire: per-async-pipeline rate (one per
    // out-of-order arrival). Debug, not verbose — this file's
    // wire-send hot path is pinned by SubsystemLoggingTest at
    // >=2 debug-level calls; COBS and ACK are verbose, SWEEP and
    // NAK are the remaining floor. The volume concern AL-14
    // raised is real but belongs to LinkRx's *receive* side (the
    // actual per-ACK/per-NAK flood in the field capture came
    // from the receiver's wire ACK/NAK companion lines, not this
    // sender-side echo) — see the LinkRx.cpp fixes for that half.
    Log::log().debug(TAG, "wire NAK missing=%u", (unsigned)missingCobsSeq);
}

bool Link::buildAndSendMsg_unlocked(const uint8_t *b, int len,
                                    uint8_t *outLastSeq) {
    uint8_t hdr[MSG_HDR];
    msgHdrEncode((uint32_t)len, UtilCrc::crc16(b, len), hdr);
    uint8_t lastSeq = 0;
    if (len + MSG_HDR <= MAX_CHUNK) {
        uint8_t merged[MAX_CHUNK];
        memcpy(merged, hdr, MSG_HDR);
        memcpy(merged + MSG_HDR, b, len);
        lastSeq = txSeq;
        // F1: ignore the bool return for the single-frame test path. The
        // production sendMsg goes through the multi-chunk loop or the SYNC
        // branch, which DO honour the bool return.
        sendCobsFrame_unlocked(merged, MSG_HDR + len);
        txBytes += len;
        lastTxMs = hw.nowMs();
    } else {
        lastSeq = txSeq;
        sendCobsFrame_unlocked(hdr, MSG_HDR);
        lastTxMs = hw.nowMs();
        int offset = 0;
        while (offset < len) {
            int chunk = std::min(len - offset, MAX_CHUNK);
            lastSeq = txSeq;
            sendCobsFrame_unlocked(b + offset, chunk);
            txBytes += chunk;
            lastTxMs = hw.nowMs();
            offset += chunk;
        }
    }
    if (outLastSeq)
        *outLastSeq = lastSeq;
    return true;
}

} // namespace autolink
