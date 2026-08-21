// LinkStats.h — public Stats surface for the link layer.
//
// All the user-visible counters and the per-cause / per-reason
// enums that go on them. Split out of Link.h so the
// implementation header can stay focused on the link
// state machine. Consumers (LinkApi.cpp, LinkCore.cpp,
// PingPongBase.h, LinkTestAccessor) include this header
// directly.
#pragma once
#include <stdint.h>

namespace autolink {

struct Stats {
    uint64_t tx, rx;
    uint64_t discCount, frameErrs;
    // Per-cause breakdown of frameErrs. The aggregate counter is
    // kept (every increment to a cause field also bumps
    // frameErrs), and the three cause fields together always sum
    // to frameErrs — that invariant is pinned by FrameErrCauseTest.
    // BadHeader = recvMsg's beginMsg-failure resync scan;
    // OverLen = recvMsg's len > max_len branch; CrcFail = wire
    // CRC fail (COBS payload CRC16, framer unknown-type, wire
    // CTRL CRC8). Pinned by FrameErrCauseTest.
    uint64_t badHeaderErrs, overLenErrs, crcFailErrs;
    // Chunks that were accepted into the ARQ pipeline but wiped
    // un-delivered by a link reset (arq_.clearAll()). Nonzero
    // means sendMsg-accepted data did NOT reach the peer — the
    // link's delivery guarantee is per-session; across resets the
    // application layer must detect (echo gap / sequence check)
    // and re-send. Surfaced so silent loss is impossible: any
    // end-to-end shortfall must be attributable to this counter.
    uint64_t droppedChunksOnReset;
    // OK-state sendMsg-accepted bytes that were then dropped
    // because the peer's post-lock settle gate was still
    // dropping. Pinned by PostLockQuietDropsSurfaceTest.
    uint64_t postLockQuietDrops;
    // sendMsg calls refused by the rate limiter (oversize
    // messages, rollout-storm protection). Pinned by
    // RateLimitRolloverAdmitTest.
    uint64_t rateLimitedDrops;
    // sendMsg calls refused because the GBN in-flight
    // window was full (inflight + chunks > window). The
    // existing `postLockQuietDrops` and `rateLimitedDrops`
    // counters cover the other two surface-visible
    // rejection paths but this one was conflated under
    // the generic `lastSendMsgReason_` enum, so the
    // operator could not see the *count* of GBN window
    // rejections — only the most-recent reason on the
    // next read. Surfaced so a sustained back-pressure
    // signal (the field's "messages piling up because
    // the link is moving slower than the app can wait")
    // is countable. Pinned by SendMsgReasonEnumTest.
    uint64_t gbnWindowFullDrops;
    // sendMsg calls refused because the ARQ cache ran
    // out of room mid-message. The "partial send" path
    // in LinkApi.cpp's multi-chunk ASYNC loop — a chunk
    // accepted and the message continues, but
    // arqCache_.hasRoom() returns false before the
    // next chunk can be inserted, so the link drops the
    // partial message with PoolExhaust. The counter is
    // bumped once per dropped message (not per dropped
    // chunk — the spec is "per refused sendMsg", and
    // the surfacing shape mirrors gbnWindowFullDrops).
    // Pinned by SendMsgReasonEnumTest.
    uint64_t poolExhaustDrops;
    // E3: TX-ring stall drops. The per-chunk drain
    // wait (D4/E1) hit its deadline before the UART
    // ring accepted the chunk. Distinct from
    // rateLimitedDrops (token-bucket admission) and
    // gbnWindowFullDrops (pending-window admission)
    // — a ring stall is hardware-side backpressure
    // (the UART driver's queue is full and the wire
    // is taking longer than one chunk RTO to drain).
    // Conflating the three made the field-log
    // diagnostic of "messages piling up at 9600
    // baud" impossible: a sustained
    // token-bucket refusal looks identical to a
    // sustained ring stall on the operator's
    // dashboard. Pinned by
    // SendMsgTxRingStallDropsCountedTest.
    uint64_t txRingStallDrops;
    // CTRL frames whose CRC8 failed inside the post-lock
    // settle window and were swallowed without counting a
    // frame error. Expected baud-switch garbage, not a link
    // fault — but counted so the window can never hide loss
    // silently. A CRC-valid frame is never settle-dropped.
    // Pinned by SettleGateTest.
    uint64_t settleDrops;
    // Per-second counters from the link layer:
    //   acksSent / naksSent — wire-level ACK / NAK
    //     emissions. Per-async-pipeline rate, but
    //     surfaced in the periodic stats line so the
    //     field operator can see "did the link ACK at
    //     all during the last 5 s window" without
    //     grepping wire-COBS verbose lines.
    //   staleAmbiguous — wrap-ambiguous dup seqs
    //     dropped without ACK (see onPayload's
    //     Stale branch). Zero in a healthy link;
    //     non-zero flags a peer reset that restarted
    //     txSeq without our cursor moving.
    //   txBlockedMs — accumulated time hw.tx()
    //     blocked under the link lock. The
    //     per-call warning lives in EspHal.cpp; the
    //     aggregate surfaces the regression that the
    //     bounded-burst fix (defect 1) is meant to
    //     prevent. Pinned by
    //     StatsAckNakCountersTest.
    uint64_t acksSent = 0, naksSent = 0;
    // holdNaksLiveness — subset of naksSent emitted purely on the
    //     hold-NAK liveness timer (HOLD_NAK_LIVENESS_MS elapsed with
    //     zero appBufFree() drain progress). Distinguishes "the
    //     receiver is draining slowly" from "the receiver is
    //     stalled and this NAK exists only to keep the sender from
    //     declaring it dead." Pinned by HoldNakLivenessCadenceTest.
    // resyncDroppedBytes — bytes discarded by
    //     findMsgHeaderResync_unlocked's failure path (no valid
    //     header found in the scanned window). Was previously
    //     unrecorded and printed as a misleading negative byte
    //     count in the resync log line. Pinned by
    //     ResyncScanReportsDroppedBytesTest.
    uint64_t holdNaksLiveness = 0;
    uint64_t resyncDroppedBytes = 0;
    uint64_t staleAmbiguous = 0;
    uint64_t txBlockedMs = 0;
    // HAL-side RX overflow / framing-error counters.
    // A total RX blackout produced no log line at
    // all (defect 1) — these surfaces in the
    // periodic stats line so the wire-level
    // overflow / framing-error signature is
    // visible to the field operator without a
    // verbose log level. Pinned by
    // StatsHalOverflowCountersTest.
    uint64_t rxOverflows = 0, rxFrameErrs = 0;
    // AL89-11: log ring drops surfaced as a
    // permanent Stats field. The field
    // capture's 42.8 s log hole was
    // inferable from a gap in the master
    // log; this counter makes the
    // underlying overflow countable, so a
    // future hole is detected without
    // timestamp-grepping the output. Read
    // from Log::log().droppedLines() at
    // getStats() time (the Log singleton
    // owns the counter — Stats just
    // mirrors it). Pinned by
    // LogDropsSurfaceTest.
    // AL90-7: zero-initialised (a fresh
    // Stats struct read before getStats()
    // populates a field would otherwise
    // surface garbage).
    uint64_t logDrops = 0;
};

enum class ResetReason : uint8_t {
    Kickoff = 0,        // Link::kickoff() master branch (count=false path)
    UserDropLink = 1,   // Link::dropLink() public API
    ErrThreshold = 2,   // err counter past cfg.errThreshold
    ErrRate = 3,        // err rate window past cfg.errRateWindow
    HealthWatchdog = 4, // applyHealth_unlocked: idle/asym/dead/silent/tx-stall
    GbnMaxRetx = 5,     // GBN base maxRetx, honest link drop
    GbnKeepRescue = 6,  // GBN Keep streak hit the rescue cap
    PeerEpochMismatch = 7, // processCtrlFrame_unlocked epoch mismatch
    PeerBaudMismatch = 8,  // HAL BREAK-storm escalation: P1 walk
    BaudUpgrade = 9,       // bounded re-try of a faster proven baud
    ResetReasonCount = 10
};

// Per-cause frame-error tag, stamped on every err_unlocked() call so
// the Stats counters can disambiguate what kind of bad wire activity
// drove an err() increment. The aggregated frameErrs counter is
// kept; the per-cause breakdown (badHeaderErrs / overLenErrs /
// crcFailErrs) surfaces the same data in a form an operator can act
// on (a 10x spike in badHeaderErrs vs crcFailErrs is a different
// field problem). BadHeader = recvMsg's beginMsg-failure resync
// scan; OverLen = recvMsg's len > max_len branch; CrcFail = wire
// CRC fail (COBS payload CRC16, framer unknown-type, wire CTRL CRC8).
// Pinned by FrameErrCauseTest.
enum class FrameErrCause : uint8_t {
    BadHeader = 0,
    OverLen = 1,
    CrcFail = 2,
};

// Enum reason for sendMsg's failure modes. The
// bool-returning sendMsg keeps its contract (true=ok,
// false=failed) and stamps lastSendMsgReason_ with the
// specific cause so the app can log "postLockQuiet" /
// "gbnWindowFull" / "poolExhaust" / "notOk" instead of
// one conflated "backpressure" label. Pinned by
// SendMsgReasonEnumTest.
enum class SendMsgReason : uint8_t {
    Ok = 0,
    NotOk = 1,                 // state != State::OK
    PostLockQuiet = 2,         // txQuiet_unlocked() returned true
    GbnWindowFull = 3,         // inflight + chunks > window
    PoolExhaust = 4,           // arqCache_.hasRoom() false mid-message
    LengthInvalid = 5,         // len < 0 or len > cfg.maxMsg
    LengthZero = 6,            // len == 0
    ChunksOverflow = 7,        // chunks > COBS_SEQ_SPACE
    SyncMidMessageTimeout = 8, // SYNC mid-message ACK timeout
    RateLimited = 9,           // offered rate > baud/10 bytes/s
    // E3: TX-ring stall — the per-chunk drain wait
    // (D4/E1) hit its deadline before the ring
    // accepted the chunk. Distinct from RateLimited
    // (the token-bucket refused the send — that's a
    // producer-side admission decision) and from
    // PoolExhaust (the ARQ cache is full — that's a
    // pending-window decision). TX-ring stall is a
    // hardware-side backpressure event: the UART
    // driver's queue is full and the wire is taking
    // longer to drain than the per-chunk RTO. Pinned
    // by SendMsgTxRingStallEnumTest.
    TxRingStall = 10,
    // G4: out-param sentinel for
    // drainTxRing_unlocked. A None
    // return means the drain
    // succeeded; the outReason
    // stays None. The drain call
    // sites that don't care about
    // the reason pass nullptr.
    None = 11,
};

} // namespace autolink
