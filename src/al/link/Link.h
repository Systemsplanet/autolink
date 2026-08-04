
#pragma once
#include "al/AutoLinkConfig.h"
#include "al/hal/IHal.h"
#include "al/link/arq/IArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/ISweepCtx.h"
#include "al/link/LinkWire.h"
#include "al/link/io/LinkMsgCodec.h"
#include "al/link/sweep/LinkDecision.h"
#include "al/link/timers/LinkHealth.h"
#include "al/link/io/LinkFrameRx.h"
#include "al/link/sweep/LinkBaudSweep.h"
#include "al/link/sweep/LinkDecision.h"
#include "al/link/sweep/LinkSweep.h"
#include "al/link/timers/LinkBreak.h"
#include <stdint.h>
#include <stddef.h>

namespace autolink {
enum class State { OK, SWP };
const char *StateToStr(State s);

constexpr int CTRL_FRAME_SIZE = 5;
constexpr int CTRL_FRAME_SEQ_IDX = 2;
constexpr int CTRL_FRAME_PAYLOAD_IDX = 3;
constexpr int CTRL_FRAME_CRC_IDX = 4;

struct Stats {
    uint64_t tx, rx;
    uint64_t discCount, frameErrs;
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
    // CTRL frames whose CRC8 failed inside the post-lock
    // settle window and were swallowed without counting a
    // frame error. Expected baud-switch garbage, not a link
    // fault — but counted so the window can never hide loss
    // silently. A CRC-valid frame is never settle-dropped.
    // Pinned by SettleGateTest.
    uint64_t settleDrops;
};

struct Diag {
    uint8_t txSeq;
    bool rxSeqSet;
    uint8_t rxSeq;
    uint64_t gaps, stale, lostMsgs;
    uint64_t baudRetries;
    uint8_t preferredBaud;
    // Monotonic count of every reset_unlocked() call grouped
    // by trigger reason. An unexplained OK -> SWP transition
    // without a corresponding log line (e.g. a debug-flooded
    // log sink) is traceable from these counters. Pinned by
    // ResetReasonDiagTest.
    uint64_t resetReasons[8];
    uint64_t resetCount;
    // Count of OK-state BREAKs that the confirm window judged
    // spurious (cleared by a valid frame before the deadline).
    uint64_t breaksSuppressed;
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
};

class LinkArq;
class LinkSweep;
class LinkTestAccessor;

class Link : private UtilFrameRx::Listener,
             public ISweepCtx,
             public ILinkEvents {
    friend class AutoLink;
    friend class LinkTestAccessor;
    IHal &hw;

    IArqCache &arqCache_;
    bool isMaster;
    AutoLinkConfig cfg;

    State state;
    int errs, spdI, pingSample;
    int emptySweeps;
    // PONGs received during the current sweep round.
    // Master p2/p3-fallback paths require at least one
    // PONG at the fallback baud before declaring OK —
    // declaring OK against a peer that never answered
    // is a 10 s retx-storm wait with no recovery path
    // (master sends LOCK_CMD + lockOk, peer is wedged
    // listening at a different baud, neither side ever
    // converges). Reset to 0 at the start of every
    // P1/P2 entry. Pinned by FallbackRequiresPongTest.
    int sweepPongCount_ = 0;
    UtilBaudSweep baudSweep;

    LinkSweep sweep_;
    bool wasEverOk_ = false;

    static constexpr uint8_t NO_PREFERRED_BAUD = 0xFF;
    uint8_t preferredBaud_ = NO_PREFERRED_BAUD;
    // Fastest baud index this pair has ever locked. preferredBaud_
    // is overwritten by every lock, including a p2-fallback lock at
    // the slowest baud, which erases the memory of the fast baud
    // that was working minutes earlier — from then on every
    // preserving reset camps at the slow baud and the link never
    // climbs back. bestProvenBaud_ only ever moves toward faster
    // (lower index), so the fast baud survives a slow fallback and
    // a bounded upgrade can aim at it. Cleared with preferredBaud_
    // only when the pair genuinely renegotiates from scratch.
    uint8_t bestProvenBaud_ = NO_PREFERRED_BAUD;
    // Deadline for the next bounded upgrade attempt, 0 = disarmed.
    uint32_t baudUpgradeAtMs_ = 0;
    int baudUpgradeAttempts_ = 0;
    // A failed upgrade costs one preserving reset, so the cap is
    // small; a genuinely degraded line settles at the slow baud
    // after this many tries instead of oscillating.
    static constexpr int BAUD_UPGRADE_MAX_ATTEMPTS = 3;
    // Let the slow lock prove itself before spending a reset on an
    // upgrade, so a flapping line is not made worse.
    static constexpr uint32_t BAUD_UPGRADE_DELAY_MS = 10000;
    // A fast P3 re-lock at preferredBaud_ is in flight. If it
    // misses, P3's timeout walks P1 from the slowest rather than
    // advancing to the next baud, so one bad preferred baud
    // can't drag the link off the proven one forever.
    bool resweepPrefPending_ = false;
    // : on a P3 preferredBaud_ miss, retry up to
    // RESWEEP_PREF_MAX_ATTEMPTS times at the same baud before
    // falling through to a full P1 walk. The peer may be on a
    // sweep-phase skew (slave took the preserved-baud fast path
    // in the same window, but the master's P3 timeout fired
    // before the peer's PONG arrived). One more PING at the
    // same baud lets the two sides converge without dropping
    // to 9600. Pinned by WireSimReConvergeTest.
    int resweepPrefAttempts_ = 0;
    // Hard ceiling on P3 re-PINGs. The camp is bounded by
    // resweepPrefDeadlineMs_ (a time budget); this only stops a
    // pathologically short t3 from spinning the timer.
    static constexpr int RESWEEP_PREF_MAX_ATTEMPTS =
        LinkSweep::RESWEEP_PREF_MAX_ATTEMPTS;
    static constexpr int RESWEEP_PREF_ATTEMPT_CEILING = 24;
    // Wall-clock deadline for camping at preferredBaud_ after a
    // preserving reset. An attempt count alone made the camp
    // t3-sized: 2 attempts x ~250 ms = ~750 ms, against a peer
    // that in the field took ~12 s to come back (its log stopped
    // mid-stream). The camp expired long before the peer returned,
    // so the master walked P1 down to 9600 and locked there.
    // A time budget decouples camp length from t3.
    uint32_t resweepPrefDeadlineMs_ = 0;
    // Camp budget: generous enough to outlast a peer reboot or a
    // multi-second app stall, bounded so a genuinely dead peer
    // still reaches the P1 walk. The disc-storm and
    // locksWithoutRecv_ guards in reset_unlocked continue to gate
    // whether the camp is entered at all.
    uint32_t resweepPrefBudgetMs_unlocked() const {
        uint32_t b = (uint32_t)cfg.syncAckTimeoutMs * 8u;
        if (b < 3000u)
            b = 3000u;
        if (b > 5000u)
            b = 5000u;
        return b;
    }
    int baudRetries_ = 0;
    uint32_t errWindowStartMs_ = 0;
    int errWindowCount_ = 0;

    uint8_t rxBuf[CTRL_FRAME_SIZE];
    int rxIdx;
    // A CTRL frame can straddle two onRx delivery chunks; hold
    // the tail until the next chunk completes or disqualifies it.
    uint8_t okCarry_[CTRL_FRAME_SIZE];
    int okCarryLen_ = 0;

    UtilFrameRx frameRx;
    LinkMsgCodec msgRx_;

    uint32_t lastRxMs, lastTxMs;
    // lastValidRxMs is the timestamp of the last CRC-validated
    // frame arrival. lastRxMs is stamped on any byte, including
    // a noise byte that happens to land on a CTRL preamble
    // and clears carry state; keying the health machine off
    // lastValidRxMs distinguishes a link that's actually
    // exchanging data from one that's seeing a noise heartbeat.
    // Pinned by LastValidRxMsTest.
    uint32_t lastValidRxMs = 0;

    uint32_t txRejFirstMs_ = 0, txRejLastMs_ = 0;
    void noteTxReject_unlocked() {
        uint32_t t = hw.nowMs();
        if (t == 0)
            t = 1;
        if (txRejFirstMs_ == 0)
            txRejFirstMs_ = t;
        txRejLastMs_ = t;
    }
    uint64_t txBytes, rxBytes;
    // Drops attributed to the post-lock quiet
    // window. Was previously returned as a generic
    // "backpressure" rejection with no surface, so
    // the app layer's dropped-message accounting
    // couldn't disambiguate postLockQuiet from
    // GbnWindowFull or RateLimited. Pinned by
    // PostLockQuietDropsCountedTest.
    uint64_t postLockQuietDrops_ = 0;
    uint64_t rateLimitedCount_ = 0;
    // Offered-rate admission: track bytes sent in the
    // last RATE_WINDOW_MS and refuse sendMsg with
    // SendMsgReason::RateLimited if the projected
    // line rate exceeds baud/10 bytes/s. A
    // 512000 lock with 32 × 250 B already in flight
    // reports 8000 B/s offered, which is well under
    // the 51200 B/s line rate, so the check is a
    // no-op. The same 32 × 250 B into a 9600 lock
    // would offer 8000 B/s into a 960 B/s line —
    // guaranteed congestion collapse. The line rate
    // is 10 bits/byte (start + 8 data + stop). Pinned
    // by RateLimitRolloverCheckTest.
    static constexpr uint32_t RATE_WINDOW_MS = 1000;
    uint32_t rateWindowStartMs_ = 0;
    uint32_t rateWindowBytes_ = 0;
    // Signed debt timestamp: holds a future
    // `hw.nowMs()` value until which sendMsg must
    // refuse. Set on oversize admission so the
    // next call cannot roll the window early via
    // unsigned underflow. Pinned by
    // RateLimitRolloverCheckTest.
    int32_t rateNextAllowedMs_ = 0;
    uint64_t discCount, frameErrs;
    // Chunks that were accepted into the ARQ pipeline but wiped
    // un-delivered by a link reset (arq_.clearAll()). Nonzero
    // means sendMsg-accepted data did NOT reach the peer — the
    // link's delivery guarantee is per-session; across resets the
    // application layer must detect (echo gap / sequence check)
    // and re-send. Surfaced so silent loss is impossible: any
    // end-to-end shortfall must be attributable to this counter.
    uint64_t droppedChunksOnReset;

    // A CRC-valid frame while OK is proof the link is actually
    // healthy at the locked baud, which is exactly what clears an
    // OK-state BREAK's confirm window (see breakSuspectMs_) — but
    // only once the baud-derived grace has passed since the break
    // armed, AND only on the second qualifying frame. A frame
    // arriving sooner could already have been queued on the wire
    // before the BREAK fired (the dying session's tail data), not
    // proof of an ongoing conversation. A single late frame is
    // the dying tail too — only two frames straddling the grace
    // window prove the peer is alive *now*. Pinned by
    // AckClearsBreakWindowTest and BaudDerivedBreakGraceTest.
    void noteValidFrameOk_unlocked() {
        if (breakSuspectMs_ == 0)
            return;
        uint32_t now = hw.nowMs();
        uint32_t grace = breakGraceMs_unlocked(*this);
        if ((uint32_t)(now - breakSuspectMs_) < grace) {
            return;
        }
        // Second frame clears. The first qualifying frame is the
        // "still in flight" tail; the second proves the link
        // survived the BREAK and is producing new traffic.
        if (breakSuspectSeen_ == 0) {
            breakSuspectSeen_ = 1;
            return;
        }
        breakSuspectMs_ = 0;
        breakSuspectSeen_ = 0;
        breaksSuppressed_++;
    }

    // Paces a multi-chunk ASYNC send so the peer's uart_event_task
    // can drain the RX FIFO between chunks. SYNC is ACK-gated and
    // has no burst to pace. Pinned by AsyncChunkGapTest.
    int interChunkGapMs_unlocked() const {
        if (cfg.mode != AutoLinkConfig::Mode::ASYNC)
            return 0;
        return cfg.asyncChunkGapMs < 0 ? 0 : cfg.asyncChunkGapMs;
    }

    bool onSyncAckTimeout_unlocked(bool midMessage);
    // Edge-triggered latch so the post-lock quiet gate logs
    // its first activation per session exactly once, not on
    // every sendMsg call inside the window. Cleared by
    // reset_unlocked.
    bool postLockQuietLogged_ = false;

    struct SyncOp {
        uint8_t seq = 0;
        const uint8_t *raw = nullptr;
        int rawLen = 0;
        int attempt = 0;
    };
    bool syncRtoStep_unlocked(SyncOp &op);
    bool syncAwaitAcked_unlocked(SyncOp &op);
    bool txQuiet_unlocked() const;
    uint32_t lockedAtMs_ = 0;
    // Post-lock settle gate. After a sweep → OK
    // transition, the wire has line garbage from
    // the baud switch (NAKs at
    // 17.983-17.995, drain at 18.018). Frames
    // arriving before settleUntilMs_ are dropped
    // silently — no ACK, no NAK, no state advance,
    // no app-buf write — so a pre-lock noise
    // frame can't be ACKed (which would advance
    // the peer's gbnBase) or NAKed (which would
    // retx into an undrained app buf). Set by
    // lockOk_unlocked, cleared by onTimer when
    // the deadline passes. Pinned by
    // SettleGateTest.
    uint32_t settleUntilMs_ = 0;
    // Count of CRC-failed frames swallowed by the settle
    // window. Surfaced in Stats.settleDrops.
    uint64_t settleDrops_ = 0;
    // Hold-induced-gap NAK suppression (see onPayload's HoldAck
    // and Gap branches): the seq we last NAK'd because the app buf
    // was full — deliberately held, not lost, so out-of-order
    // arrivals behind it must not re-NAK it into a resend storm.
    bool holdNakActive_ = false;
    uint8_t holdNakSeq_ = 0xFF;
    // Stale frames dropped without ACK because their seq was
    // wrap-ambiguous (see the Stale branch in onPayload).
    uint64_t staleAmbiguous_ = 0;
    // Session-resume v2 (see reset_unlocked): true while a
    // recovery reset's preserved delivery state is waiting for
    // the relock; lockOk_unlocked consumes it to skip the
    // app-buf wipe.
    uint32_t lastDiscMs_ = 0;
    int recentDiscs_ = 0;
    // recentDiscs_ reaching this many inside the 10 s window means
    // the "proven" preferredBaud_ isn't proven; reset_unlocked
    // stops fast-relocking there and forces a full P1 walk.
    static constexpr int DISC_STORM_THRESHOLD = 3;

    uint8_t txSeq = 0;
    bool rxSeqSet = false;
    uint8_t rxSeq = 0;
    uint64_t gaps = 0, stale = 0;
    uint64_t lostMsgs = 0;


    uint8_t lastAckSeq_ = 0xFF;

    uint8_t lastNakSeq_ = 0xFF;

    uint8_t lastRxSeq_ = 0xFF;

    // Session epoch carried in the seq byte of sweep frames.
    // Bumped on every real reset (same gate discCount uses) so a
    // peer that restarted mid-session — without delivering a BREAK
    // the still-OK side would have seen — is detected the moment a
    // sweep frame arrives carrying a different epoch. Receiving an
    // unknown epoch on a still-OK link forces a resync instead of
    // auto-ACKing, so the link drops into a real sweep rather than
    // walking the receiver through GAP...dropped storms from a peer
    // that's already on a fresh session.
    uint8_t sweepEpoch_ = 0;
    uint8_t peerSweepEpoch_ = 0;
    bool peerSweepEpochKnown_ = false;

    // A UART_BREAK event while OK is reported by the same driver
    // path for a genuine peer detach AND for a framing glitch —
    // observed in the field under sustained 512000-baud ASYNC
    // traffic with large messages, where the glitch is exactly
    // what seeds the epoch-bounce failure mode. The first BREAK
    // while OK arms a confirm window (checked at the next
    // onTimerOk_unlocked tick) instead of resetting immediately;
    // a still-healthy link keeps delivering valid frames through
    // the window and clears the suspicion, a genuinely dropped
    // peer goes silent and the deadline confirms the reset. A
    // second BREAK while one is already suspect confirms at once.
    uint32_t breakSuspectMs_ = 0;
    // Tracks whether a single late frame has already been
    // observed (so the next one will clear). Reset when
    // the suspicion window arms / clears. See
    // noteValidFrameOk_unlocked.
    uint8_t breakSuspectSeen_ = 0;
    // A frame arriving within this many ms of arming suspicion could
    // already have been queued on the wire before the BREAK — the
    // dying session's tail data, not proof the link is still alive.
    // Floored by breakGraceMs_unlocked to keep the suspend-wait
    // bounded at the fastest configured baud; the real value is
    // roundTripMs(lockedBaud) at the current lock.

    // Baud-derived grace: round-trip at the locked baud, with a
    // floor so a 512000 lock still has room for the
    // FIRST late-tail frame to be observed. A 20 ms fixed grace
    // is shorter than one 250 B chunk's flight time at 512000
    // (~5 ms tx + 1 RTT + 1 ms drain = ~7 ms each way; tail
    // data already in the UART FIFO arrives inside the 20 ms
    // window and is rejected), then BREAK_CONFIRM_MS=150
    // fires while the link is still passing data.
    // Coalesce window: two BREAK events within this many ms are
    // treated as the same electrical event (a single glitch
    // surfaces as multiple BREAK / framing-error interrupts at
    // sub-ms spacing on the ESP32 UART driver). Pinned by
    // BreakInterruptCoalesceTest.
    uint64_t breaksSuppressed_ = 0;

    // Per-reason reset count + last reset reason. Surfaces in
    // getDiag so a future OK -> SWP transition with a missing
    // log line (debug-flooded sink) is traceable. Pinned by
    // ResetReasonDiagTest.
    uint64_t resetReasonCounts_[(size_t)ResetReason::ResetReasonCount] = {};
    uint64_t resetCount_ = 0;
    ResetReason lastResetReason_ = ResetReason::Kickoff;

    // Consecutive successful locks (post-BREAK re-locks) that
    // produced zero valid application frames before the next
    // reset. The DropPeerBaudMismatch health action escalates
    // when this climbs past kPeerBaudMismatchThreshold instead
    // of running the same top-down sweep that just failed. Pinned
    // by PeerBaudMismatchTest.
    int locksWithoutRecv_ = 0;
    static constexpr int kPeerBaudMismatchThreshold = 3;
    // BREAK storm escalation: HAL fires onBreakStorm()
    // from the UART event task. Set a flag and consume
    // it at the top of onTimer() where the state
    // machine already owns the setSpd / startTimer
    // sequence. Pinned by BreakStormDefersToOnTimerTest.
    bool breakStormPending_ = false;
    // The reason the most recent sendMsg returned false, so
    // the app can log a precise cause (postLockQuiet /
    // gbnWindowFull / poolExhaust) instead of one conflated
    // "backpressure" label. Stamped under the link lock inside
    // sendMsg. Pinned by SendMsgReasonEnumTest.
    SendMsgReason lastSendMsgReason_ = SendMsgReason::Ok;

    LinkArq arq_;

    // Whole-window retransmit rounds since gbnBase_ last advanced.
    int gbnAttempts_ = 0;
    uint32_t gbnBackoffMs_ = 0;
    // 0xFF = no resend round yet; a live base is never 0xFF.
    uint8_t gbnLastRetxBase_ = 0xFF;
    // Storm-immune companion to LinkArq's per-slot sentAtMs_/
    // retxCount_ escalation clock. gbnResendWindow_unlocked (the
    // onNak fast-retransmit path) intentionally calls
    // arq_.applyRetx() on every NAK with no rate limit — that's
    // the "inline NAK retransmit" contract loopback_multichunk_test
    // pins. But applyRetx() also stamps sentAtMs_[bi]=now on every
    // call, so under a NAK burst (many out-of-order arrivals per
    // ms; 305k gbnResendWindow_unlocked calls
    // in 9s) the base's age-since-last-attempt is perpetually
    // reset to ~0 and never reaches ackRtoMs, so
    // decideSlot()/sweepRetx_unlocked() can never observe an
    // elapsed RTO and the maxRetx honest-drop never fires — a
    // livelock where the retry storm suppresses its own circuit
    // breaker. This pair tracks real wall-clock time since the
    // base last CHANGED value (true progress), immune to
    // same-base resend spam, as a second, independent input to
    // the same honest-drop decision. Pinned by
    // GbnBaseStuckLivelockTest.
    uint32_t gbnBaseStuckSinceMs_ = 0;
    uint8_t gbnBaseStuckTrackedSeq_ = 0xFF;
    // Wall-clock stamp of the most recent OK-timer tick
    // (sweepRetx_unlocked's onTimer entry). When
    // (now - lastOkTickMs_) exceeds ~3x the OK-tick
    // interval, the OK-timer task itself was starved
    // (CPU/log/log-transport wedge, or the uart_event_task
    // holding the link lock too long) and the base-stuck
    // clock has been counting the wrong absence — the
    // peer's ACKs may be sitting in the UART FIFO right
    // now, just undelivered because the link lock is held
    // elsewhere. Re-arm gbnBaseStuckSinceMs_ in that case
    // so the honest-drop verdict waits one more full
    // observation window. Pinned by
    // CpuStallReArmsBaseStuckTest.
    uint32_t lastOkTickMs_ = 0;
    // Baud-aware stuck threshold: the wall-clock window a
    // base needs to have been unchanged before the storm
    // honest-drop fires. Derived from
    // pendingBytes * 10000 / baud + 1 RTT (10 bits/byte,
    // bits/s -> bits/ms), so a 32-chunk
    // 250-byte window at 9600 (~8.3 s drain) won't trip
    // honest-drop before the drain completes. Recomputed
    // on every lockOk_unlocked. Pinned by
    // BaudAwareStuckThresholdTest.
    uint32_t gbnBaseStuckThresholdMs_ = 0;
    // Set when a test (or future operator override) seeds a
    // custom threshold via setGbnBaseStuckThresholdMsForTest.
    // While set, sweepRetx_unlocked skips the per-tick baud
    // recompute so the override survives. Production never
    // sets this; clear it on lockOk.
    bool gbnBaseStuckThresholdOverridden_ = false;
    // Consecutive Keep verdicts on a maxRetx base. Capped so a
    // dead peer whose floating RX line keeps stamping lastRxMs
    // cannot ride Keep forever. Pinned by GbnKeepRearmTest.
    int consecutiveKeep_ = 0;
    static constexpr int DEFAULT_GBN_KEEP_RESCUE_CAP = 3;
    void gbnResendWindow_unlocked(uint32_t now);
    // Retransmits the base + rebuilds gbnAttempts_/gbnBackoffMs_/
    // gbnLastRetxBase_, then restarts gbnBaseStuckSinceMs_ — shared
    // by sweepRetx_unlocked's Retx branch and its storm-stuck
    // forced-retx fallback. Pinned by GbnStuckForcesRetxTest.
    void gbnRetxBaseAndRearm_unlocked(uint32_t now);

    bool linkPaused_ = false;
    bool kickedOff_ = false;

    bool onPayload(uint8_t cobsSeq, const uint8_t *b, int n) override;
    bool onAck(uint8_t ackedCobsSeq, uint16_t bytesRecvd);
    bool onNak(uint8_t missingCobsSeq);
    bool onFrameError() override;

#ifdef AUTOLINK_HOST_TEST
    SyncOp testOp_;
    uint8_t testOpBuf_[MAX_CHUNK];
#endif

    void sendPongAck_unlocked();
    int phase1ArmMs();

    void sendFrame_unlocked(uint8_t payload);
    void sendSweepFrame_unlocked(uint8_t payload);
    void sendFrame(uint8_t payload) override { sendFrame_unlocked(payload); }
    void sendSweepFrame(uint8_t payload) override {
        sendSweepFrame_unlocked(payload);
    }
    void buildAndTxCobsFrame_unlocked(uint8_t seq, const uint8_t *b, int n);
    void sendCobsFrame_unlocked(const uint8_t *b, int n);
    int sendMsg_unlocked(const uint8_t *b, int len);

    bool buildAndSendMsg_unlocked(const uint8_t *b, int len,
                                  uint8_t *outLastSeq);
    uint8_t sendCobsFrameAcked_unlocked(const uint8_t *b, int n,
                                        uint8_t baseSeq);
    void resendCobsFrame_unlocked(uint8_t seq, const uint8_t *b, int n);
    static constexpr uint8_t NO_BASE = 0xFF;
    void sendAckFrame_unlocked(uint8_t ackedCobsSeq, uint16_t bytesRecvd = 0);
    void sendNakFrame_unlocked(uint8_t missingCobsSeq);
    void txSmallCobs_unlocked(uint8_t *u, size_t rawLen);
    void sendCtrlCobsFrame_unlocked(uint8_t type, uint8_t seq);

    void changeState_unlocked(State s);
    int bestSpd_unlocked() const;
    int readStream(uint8_t *b, int n);
    void resetSeq_unlocked();
    void lockOk_unlocked(int idx, const char *tag);
    bool ctrlFrameReady_unlocked(uint8_t cobsSeq, uint8_t payload,
                                 State curState);

    bool processCtrlFrame_unlocked(State cur);
    bool handleSwp_unlocked(uint8_t cobsSeq, uint8_t payload);

    bool applyMasterSwpAction_unlocked(SwpPhaseAction a);
    bool applyPongSwpAction_unlocked(SwpPhaseAction a);
    HealthAction applyHealth_unlocked(uint32_t now);
    bool sweepRetx_unlocked(uint32_t now);
    bool onTimerOk_unlocked();
    void onTimerSwp_unlocked();
    void reset_unlocked(bool count, bool preservePreferredBaud = false,
                        ResetReason reason = ResetReason::Kickoff);
    int okTickMs() const;
    void retxSeq_unlocked(uint8_t seq);
    int findMsgHeaderResync_unlocked(int max_scan);

    void hwLock() override { hw.lock(); }
    void hwUnlock() override { hw.unlock(); }
    uint32_t hwNowMs() const override { return hw.nowMs(); }

    // Ceiling on the exponential backoff, so a permanently-stuck
    // base still trips maxRetx inside the idle watchdog window.
    uint32_t gbnBackoffCapMs_unlocked() const {
        uint32_t cap = (uint32_t)cfg.syncAckTimeoutMs * 8u;
        if (cap < (uint32_t)cfg.syncAckTimeoutMs)
            cap = (uint32_t)cfg.syncAckTimeoutMs;
        return cap;
    }
    int gbnKeepRescueCap_unlocked() const {
        return DEFAULT_GBN_KEEP_RESCUE_CAP;
    }
    // Baud-aware stuck threshold (in ms): the time the
    // in-flight window needs to fully drain at the
    // locked baud. pendingBytes * 10000 / baud gives the
    // drain time at 10 bits/byte (start + 8 data + stop,
    // the UART's own framing), and the +1 RTT covers
    // the peer's ACK turnaround. Recomputed on every
    // lockOk_unlocked. Floor at syncAckTimeoutMs (one
    // RTO) so a quiet single-chunk link still has a
    // reasonable window — without the floor, the
    // threshold is 0 for an empty window and any
    // single RTO trip would fire the honest-drop
    // immediately. Pinned by
    // BaudAwareStuckThresholdTest.
    uint32_t baudAwareStuckThresholdMs_unlocked() const {
        if (spdI < 0 || spdI >= cfg.allowedBaudsCount)
            return (uint32_t)cfg.syncAckTimeoutMs;
        uint32_t baud = cfg.allowedBaudSafe(spdI);
        if (baud == 0)
            return (uint32_t)cfg.syncAckTimeoutMs;
        int pending = arq_.pendingCount();
        if (pending <= 0)
            return (uint32_t)cfg.syncAckTimeoutMs;
        int pendingBytes = pending * (MAX_CHUNK + MSG_HDR);
        // Drain time = bytes * 10 bits/byte * 1000 ms/s / baud
        // (bits/s), one RTT for ACK turnaround. The 1000 factor
        // converts baud's per-second rate to per-ms — dropping it
        // yields 0 ms at 512000 baud (pending=32) and 8 ms at
        // 9600, a thousandfold under this comment's own documented
        // "≈8.3 s at 9600". The 500 ms floor below masked it at
        // every baud tested. Floor at syncAckTimeoutMs so a quiet
        // single-chunk link still has a reasonable window. Pinned
        // by BaudAwareStuckThresholdPerTickTest.
        uint32_t drain =
            (uint32_t)((uint64_t)pendingBytes * 10000ull / baud);
        int rt = roundTripMs(baud);
        uint32_t total = drain + (uint32_t)rt;
        uint32_t floor = (uint32_t)cfg.syncAckTimeoutMs;
        return total < floor ? floor : total;
    }
    // RTO scaled to the locked baud: a low-baud link (9600)
    // needs ~2 ms per 5-byte round-trip, a high-baud link
    // (512000) needs ~0.04 ms. syncAckTimeoutMs is the
    // default floor (keeps the GBN retx sweep alive even
    // before lockOk_unlocked stamps a baud). Floored at
    // syncAckTimeoutMs so a static config-time value still
    // rules the health machine when the link is in SWP (no
    // baud to scale from). Pinned by LinkHealthBaudAwareTest.
    int baudAwareRtoMs_unlocked() const {
        // RTO must cover a real RTT at the locked baud for
        // a 250-byte MAX_CHUNK payload (the actual chunk
        // size on the wire, not the 10-byte control frame
        // roundTripMs() models). A 9600-baud link spends
        // ~260 ms transmitting one chunk — half the entire
        // 500 ms syncAckTimeoutMs — and an RTO of 500 ms
        // fires DropAsymIdle before the queued payload
        // has time to actually land on the peer. The RTO is
        // computed as 2 round-trips + transmit at the
        // locked baud: 2 * (MAX_CHUNK + frame_overhead) *
        // 10 / baud * 1000 ms. At 9600 this is 2 * 260 ms
        // = 520 ms; at 512000 it's 2 * 5 ms = 10 ms (still
        // floored at syncAckTimeoutMs). Pinned by
        // LinkHealthBaudAwareTest.
        uint32_t baud = cfg.allowedBaudSafe(spdI);
        if (baud == 0)
            return cfg.syncAckTimeoutMs;
        // (MAX_CHUNK + frame overhead) * 10 bits per byte,
        // per round trip, in milliseconds.
        uint32_t chunkBytes =
            (uint32_t)MAX_CHUNK + 4u; // preamble + cobs-ovh + seq + CRC + delim
        uint32_t txMs = chunkBytes * 10u * 1000u / baud;
        uint32_t rto = txMs * 2u;
        return rto > (uint32_t)cfg.syncAckTimeoutMs ? (int)rto
                                                    : cfg.syncAckTimeoutMs;
    }
    void hwSetSpd(uint32_t b) override { hw.setSpd(b); }
    void hwStartTimer(int ms) override { hw.startTimer(ms); }
    uint8_t reorderExpectedSeq() const;

    bool masterRole() const override { return isMaster; }
    int currentSpdI() const override { return spdI; }
    void setCurrentSpdI(int i) override { spdI = i; }
    void resetSweepPongCount() override { sweepPongCount_ = 0; }

    int allowedBaudsCount() const override {
        if (cfg.allowedBaudsCount < 0)
            return 0;
        if (cfg.allowedBaudsCount > AUTOLINK_MAX_BAUDS)
            return AUTOLINK_MAX_BAUDS;
        return cfg.allowedBaudsCount;
    }
    uint32_t allowedBaud(int i) const override {
        return cfg.allowedBaudSafe(i);
    }
    int delayMs() const override { return cfg.delayMs; }

public:
    int arqPendingCount() const { return arq_.pendingCount(); }

    // Consecutive successful locks (post-BREAK re-locks)
    // that produced ZERO valid application frames before
    // the next reset. A non-zero value means the link
    // is locking at a baud the peer isn't actually
    // on (the other side is in a relock cycle of its
    // own, or the relock-to-preserved-baud path is
    // firing on stale data). The DropPeerBaudMismatch
    // health action escalates when this climbs past
    // kPeerBaudMismatchThreshold instead of running
    // the same top-down sweep that just failed. The
    // peer-baud-mismatch escalation signal is exposed
    // through the friend shim (test /common/accessors/
    // LinkTestAccessor.h), not the public API — per
    // AGENTS rule 18 (tests access internals through
    // LinkTestAccessor, not the public surface).
    // Pinned by PeerBaudMismatchTest.

    uint16_t bytesRecvdFor(uint8_t cobsSeq) const {
        return arq_.bytesFor(cobsSeq);
    }

    uint16_t bytesRecvdForMessage(uint8_t baseSeq) const {
        return arq_.bytesForMessage(baseSeq);
    }

    uint8_t lastAckSeq() const {
        hw.lock();

        uint8_t s = lastAckSeq_;
        hw.unlock();
        return s;
    }

    uint8_t lastNakSeq() const {
        hw.lock();
        uint8_t s = lastNakSeq_;
        hw.unlock();
        return s;
    }

    uint8_t lastRxSeq() const {
        hw.lock();
        uint8_t s = lastRxSeq_;
        hw.unlock();
        return s;
    }

    Link(IHal &hw, IArqCache &cache, bool isMasterNode,
         const AutoLinkConfig &config = AutoLinkConfig());
    ~Link();

    void begin();
    void err();
    bool err_unlocked();
    void clearErr();

    int available() const;
    int peek();
    int read();
    int read(uint8_t *b, int max_len);
    int write(const uint8_t *b, int len);
    void flush();
    void flushRx();

    bool sendMsg(const uint8_t *b, int len, uint8_t *outBaseSeq = nullptr);
    // The reason the most recent sendMsg returned false.
    // Pinned by SendMsgReasonEnumTest. Stamped under the
    // link lock inside sendMsg so the app can read it after
    // a false return without racing the link thread.
    SendMsgReason lastSendMsgReason() const { return lastSendMsgReason_; }
    void dropLink();
    void setLinkPaused(bool p) { linkPaused_ = p; }

    void kickoff();

    void setMode(AutoLinkConfig::Mode m) { cfg.mode = m; }
    AutoLinkConfig::Mode mode() const { return cfg.mode; }

    size_t maxMsg() const { return cfg.maxMsg; }

    void setTxDelayMs(int ms) { cfg.txDelayMs = ms < 0 ? 0 : ms; }
    int txDelayMs() const { return cfg.txDelayMs; }

    int recvMsg(uint8_t *b, int max_len);

    int pendingAcks() const;
    bool isAcked(uint8_t cobsSeq) const;

    void getStats(Stats &s) const;
    void resetStats();
    void resetErrors();
    void resetDiag();

    const AutoLinkConfig &getConfig() const { return cfg; }

    State getState() const;
    int getErrCount() const;
    int getCurrentSpdIndex() const;
    uint32_t getCurrentBaud() const;
    void getDiag(Diag &d) const;

    void onRx(const uint8_t *data, int len) override;
    void onBreak() override;
    void onBreakStorm() override;
    void onTimer() override;

public:
    int spdIAcc() const { return spdI; }
    const AutoLinkConfig &cfgAcc() const { return cfg; }

private:
#ifdef AUTOLINK_HOST_TEST
    // Host-only hooks: SYNC sendMsg blocks on the injected clock,
    // so tests drive it in two non-blocking halves.
    bool test_sendMsgBegin(const uint8_t *b, int len);
    bool test_sendMsgStillWaiting();
    bool test_syncRtoStep();
    int test_syncAttempt() const;
#endif
};

} // namespace autolink
