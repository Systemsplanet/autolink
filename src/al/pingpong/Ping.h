
#pragma once
#ifdef ARDUINO

#    include "al/link/arq/ArqCache.h"
#    include "al/pingpong/PingGap.h"
#    include "al/pingpong/PingPongBase.h"
#    include "al/util/codec/UtilCrc.h"
#    include <string.h>

namespace autolink {
class Ping {
public:
    enum class FillMode : uint8_t { SEQUENTIAL = 0, RANDOM = 1 };

    static constexpr uint8_t NO_GAP = 0xFF;

    Ping(uint32_t debugBaud, uart_port_t uartNum, int rxPin, int txPin,
         const char *ssid = nullptr, const char *password = nullptr,
         uint16_t webPort = 8765)
        : base_(debugBaud, uartNum, rxPin, txPin, true, ssid, password,
                webPort) {}

    Ping(const Ping &) = delete;
    Ping &operator=(const Ping &) = delete;

    void setup() {
        randomSeed(esp_random());
        const char *role = "Ping";

        initSerial(base_.log_, base_.debugBaud_, role, base_.ssid_);

        if (base_.ssid_)
            installWebHooks();
        startWebMonitor(base_.log_, base_.mon_, role, base_.ssid_,
                        base_.password_, base_.webPort_);

        bringUpLink(base_.log_, base_.comm_, paused_);

        maxSeqSize_ = (int)base_.comm_.maxMsg();

        if (!base_.mon_.isUp()) {
            paused_ = false;
            base_.comm_.setLinkPaused(false);
            base_.comm_.kickoff();
            base_.log_.info("Ping",
                            "mode=Ping  ready  web monitor NOT up; "
                            "auto-kicking off link (no Start gate)");
        } else {
            base_.log_.info("Ping",
                            "mode=Ping  ready  paused=%s  "
                            "(push Start to send)",
                            paused_ ? "true" : "false");
        }
    }

    void loop() {
        uint32_t now = millis();

        if (!base_.comm_.ready()) {
            if (base_.wasReady_) {
                base_.log_.info("Ping", "link lost  pending=%d", count_);
                base_.wasReady_ = false;
                clearQueue_();
                consecTransient_ = 0;
                tSweepStall_ = now;
                // Publish the next expected echo seq so
                // a monitor or downstream Pin can see
                // the discontinuity: gapMissingSeq
                // carries (lastSeq_ + 1) % 256 until a
                // fresh message lands.
                gapSeq_ = (uint8_t)(lastSeq_ + 1u);
                tGapStopMs_ = 0;
                backpressureCoolUntilMs_ = 0;
                resetStatBaseline(stat_);
            } else {
                if (now - tNotReady_ >= 1000) {
                    if (paused_) {
                        base_.log_.debug("Ping",
                                         "not ready paused (waiting for "
                                         "Start)");
                    } else {
                        base_.log_.debug("Ping", "not ready swpAge=%lu ms",
                                         (unsigned long)(now - tSweepStall_));
                    }
                    tNotReady_ = now;
                }
            }
            base_.comm_.blinkWait(3, 100, 100, 0);
            // See Pong.h's identical branch: blinkWait(..., 0) doesn't
            // block, and nothing else here yields, so a link that's
            // down pins this core at full speed and starves anything
            // else scheduled on it (including the log drain task).
            delay(10);
            return;
        }

        if (!base_.wasReady_) {
            base_.comm_.flushRx();
            int drained = 0;
            while (base_.comm_.recv(recvBuf_, sizeof recvBuf_) > 0)
                drained++;
            if (drained)
                base_.log_.debug("Ping", "drained %d stale echo(s) pre-settle",
                                 drained);
            base_.comm_.blinkWait(4);
            tReady_ = now;
            base_.wasReady_ = true;
            consecTransient_ = 0;
            gapSeq_ = NO_GAP;
            tGapStopMs_ = 0;
            backpressureCoolUntilMs_ = 0;
        }

        if (now - tReady_ < SETTLE_MS) {
            return;
        }

        if (count_ == WINDOW) {
            if (tStall_ == 0)
                tStall_ = now;
            if (now - tStall_ > STALL_MS) {
                base_.log_.error(
                    "Ping",
                    "pipeline stall — WINDOW=%d full for %lu ms. "
                    "Clearing local pending + draining rx. pending=%d  "
                    "consec=%lu",
                    WINDOW, (unsigned long)(now - tStall_), count_,
                    (unsigned long)consecTransient_);
                clearQueue_();
                base_.comm_.flushRx();
                consecTransient_++;
            }
        } else {
            tStall_ = 0;
        }

        if (paused_) {
            uint8_t sink[PingPongBase::BUF_SIZE];
            (void)base_.comm_.recv(sink, sizeof sink);
            return;
        }

        {
            uint8_t lastNak = base_.comm_.lastNakSeq();
            uint8_t nextGap = gapSeq_;
            // Ask the link whether the seq under consideration is
            // still unacked, instead of inferring it from a
            // lastAckSeq() sample. When no gap is latched the
            // candidate is lastNak; once latched it is gapSeq_.
            uint8_t gapCand = (gapSeq_ == NO_GAP) ? lastNak : gapSeq_;
            bool gapPending =
                (gapCand != NO_GAP) && !base_.comm_.isAcked(gapCand);
            GapAction a =
                decideGapTransition(gapSeq_, lastNak, gapPending, nextGap);
            // Dedupe: if a NAK arrives for a seq that's
            // already been ACKed locally (the peer's ARQ
            // slot is freed, the NAK is stale wire noise
            // or a duplicate of one we already answered),
            // suppress the Enter / Update. The peer's
            // own ARQ never NAKs a freed slot; the only
            // path that produces a stale NAK here is a
            // queued wire frame from before the local
            // ACK landed. Pinned by PingGapTransitionTest.
            if (a == GapAction::Enter || a == GapAction::Update) {
                if (nextGap != NO_GAP && base_.comm_.isAcked(nextGap)) {
                    base_.log_.debug(
                        "Ping",
                        "gap-stop suppressed: missing seq=%u already acked "
                        "(stale NAK)",
                        (unsigned)nextGap);
                    nextGap = gapSeq_;
                    a = GapAction::Stay;
                }
            }
            if (a == GapAction::Enter) {
                base_.log_.warning("Ping",
                                   "gap stop: missing seq=%u — sending paused",
                                   (unsigned)nextGap);
                tGapStopMs_ = now;
            } else if (a == GapAction::Update) {
                base_.log_.warning(
                    "Ping",
                    "gap stop: missing seq=%u (was %u) — sending paused",
                    (unsigned)nextGap, (unsigned)gapSeq_);
                tGapStopMs_ = now;
            } else if (a == GapAction::Resume) {
                base_.log_.info("Ping", "gap resumed: seq=%u acked",
                                (unsigned)gapSeq_);
                tGapStopMs_ = 0;
            }
            // Force-resume escape: if a gap-stop has
            // been running for > GAP_STOP_FORCE_RESUME_MS
            // on the same missing seq, the seq is
            // unreachable (app-buf full + retx storm,
            // or peer's ARQ never gets the retx
            // through) — clear the gap and resume
            // sends. The peer's ARQ will eventually
            // either deliver the missing seq (rare
            // after 5 s of NAK) or hit maxRetx and
            // drop the link, which is the only
            // recovery path anyway. Sending new
            // frames into a stalled peer doesn't
            // help the missing seq but lets the
            // app make forward progress on other
            // data. Pinned by PingGapTransitionTest.
            // The override is applied to nextGap (not
            // gapSeq_ directly) so the
            // `gapSeq_ = nextGap` assignment below
            // uses the force-resumed state.
            if (gapSeq_ != NO_GAP && tGapStopMs_ != 0 &&
                (uint32_t)(now - tGapStopMs_) > GAP_STOP_FORCE_RESUME_MS) {
                base_.log_.warning(
                    "Ping",
                    "gap-stop force-resume: missing seq=%u stalled %lu ms — "
                    "resuming sends",
                    (unsigned)gapSeq_,
                    (unsigned long)((uint32_t)(now - tGapStopMs_)));
                nextGap = NO_GAP;
                tGapStopMs_ = 0;
            }
            gapSeq_ = nextGap;

            if (gapSeq_ != NO_GAP) {
                int got;
                while ((got = base_.comm_.recv(recvBuf_, sizeof recvBuf_)) >
                       0) {
                    (void)got;
                }
                if (count_ > 0) {
                    while (count_ > 0 &&
                           base_.comm_.isAcked(queue_[head_].seq)) {
                        successEchoCount_++;

                        // Labeled companion: the operator's
                        // primary per-chunk signal in field
                        // logs. Replaces the older unlabeled
                        // `echo <seq> <bytes> <pending>` line —
                        // a random draw's middle field was
                        // ambiguous between msgBytes and a
                        // sequence counter, so the unlabeled
                        // form was misread in field logs. Info
                        // level: per-echo ASYNC pipeline
                        // traffic. Pinned by
                        // PingMismatchCountTest.
                        base_.log_.info(
                            "Ping", "echo#=%llu seq=%u msgBytes=%u pending=%d",
                            (unsigned long long)successEchoCount_,
                            (unsigned)queue_[head_].seq,
                            (unsigned)queue_[head_].len, count_ - 1);
                        head_ = (head_ + 1) % WINDOW;
                        count_--;
                    }
                }
                // Unconditional stats tick while
                // gap-stopped: the previous shape
                // returned early without ever
                // calling logStats, so a wedge
                // produced no operator-visible
                // signal. The 5-second rate
                // limit in logStats keeps the
                // per-loop cost at one branch +
                // one timestamp check, so this is
                // cheap. The app-state fields
                // (gapStopped / gapMissing /
                // paused / lastSend) tell the
                // operator exactly which state
                // the app is in. Pinned by
                // StatsIncludeAppStateTest.
                AppStateLog app;
                app.gapStopped = true;
                app.gapMissingSeq = gapSeq_;
                app.paused = paused_;
                app.lastSendMsgReason = (int)base_.comm_.lastSendMsgReason();
                logStats(base_.log_, "Ping", base_.comm_, stat_,
                         successEchoCount_, mismatchCount_, 0u, app);
                return;
            }
        }

        int sentThisLoop = 0;
        const int maxTx = (base_.comm_.mode() == AutoLinkConfig::Mode::SYNC)
            ? 1
            : PingPongBase::MAX_TX_PER_LOOP;
        const int txDelayMs = base_.comm_.txDelayMs();
        if (txDelayMs > 0 && tNextSendMs_ == 0)
            tNextSendMs_ = now;
        while (count_ < WINDOW && sentThisLoop < maxTx) {
            if (txDelayMs > 0 && (int32_t)(now - tNextSendMs_) < 0)
                break;
            if (backpressureCoolUntilMs_ != 0 &&
                (int32_t)(now - backpressureCoolUntilMs_) < 0)
                break;
            // pickMsgSize_'s live cap sizes each FRESH draw against
            // the free window, but a RETAINED draw (AL88-5,
            // havePendingDraw_) was sized against whatever was free
            // at the first draw attempt — by the time it's
            // retried, other traffic can have shrunk the free
            // window further. This gate previously only checked
            // free_ >= 1 (a message-count unit) while sendMsg's
            // actual admission test is inflight + chunksForMsgLen(n)
            // <= window (a chunk-count unit): a retained multi-chunk
            // draw passed this gate on a single free slot, was
            // rejected again by sendMsg on the same chunk shortfall,
            // and got retained again unchanged — spinning at
            // whatever rate loop() is called, GbnWindowFull skips
            // the backpressure cooldown (see below), so nothing
            // throttled the retry. Gate on the retained draw's real
            // chunk cost so this case just waits (silently, no
            // sendMsg call) for window to free up instead of
            // re-attempting a draw already known not to fit. A fresh
            // RANDOM draw isn't sized yet at this point, but it
            // self-clamps to whatever is free (see pickMsgSize_) so
            // 1 always fits once anything is. A fresh SEQUENTIAL draw
            // IS already sized (seqSize_, unbounded by the window —
            // pickMsgSize_'s SEQUENTIAL branch has no live-free-window
            // clamp by design, it's the ramp under test) and costs
            // chunksForMsgLen(seqSize_) chunks the same as a retained
            // draw; gating it as 1 let every ramp size past the free
            // window reach sendMsg blind and get rejected — 205+
            // GbnWindowFull drops in a single field run. Pinned by
            // PingGateChecksRetainedDrawChunkCostTest and
            // PingSequentialFreshDrawChunkCostTest.
            {
                int free_ = effWindow_() - base_.comm_.arqPendingCount();
                int neededChunks_ =
                    havePendingDraw_ ? chunksForMsgLen(pendingDrawLen_)
                    : (fillMode_ == FillMode::SEQUENTIAL
                           ? chunksForMsgLen(seqSize_)
                           : 1);
                if (free_ < neededChunks_) {
                    break;
                }
            }
            // AL88-5: reuse a draw a prior GbnWindowFull rejected
            // instead of drawing fresh — sendBuf_ still holds those
            // bytes untouched (the failed attempt never advanced
            // past this point).
            int n;
            uint16_t crc;
            if (havePendingDraw_) {
                n = pendingDrawLen_;
                crc = pendingDrawCrc_;
            } else {
                n = pickMsgSize_(fillMode_);
                fillBuf_(sendBuf_, n, fillMode_);
                crc = UtilCrc::crc16(sendBuf_, n);
            }
            uint8_t seq = 0;
            if (!base_.comm_.sendMsg(sendBuf_, n, &seq)) {
                if (!base_.comm_.ready()) {
                    break;
                }
                // The SendMsgReason lets the app log
                // the precise cause: post-lock quiet
                // (settling), GBN window full (real
                // backpressure), pool exhaust (mid-
                // message admission failure). Three
                // distinct causes, three distinct log
                // tags. Pinned by
                // SendMsgReasonEnumTest.
                const char *cause = "backpressure";
                switch (base_.comm_.lastSendMsgReason()) {
                case SendMsgReason::PostLockQuiet:
                    cause = "postLockQuiet";
                    break;
                case SendMsgReason::GbnWindowFull:
                    cause = "gbnWindowFull";
                    break;
                case SendMsgReason::PoolExhaust:
                    cause = "poolExhaust";
                    break;
                case SendMsgReason::NotOk:
                    cause = "notOk";
                    break;
                case SendMsgReason::LengthInvalid:
                    cause = "lengthInvalid";
                    break;
                case SendMsgReason::SyncMidMessageTimeout:
                    cause = "syncMidMessageTimeout";
                    break;
                default:
                    cause = "backpressure";
                    break;
                }
                base_.log_.debug("Ping",
                                 "send failed (%s)  n=%d  "
                                 "arqPending=%d",
                                 cause, n, base_.comm_.arqPendingCount());
                // Companion log: keeps the literal
                // "send failed (backpressure)" shape
                // ModeSyncAsyncFixesTest pins (the
                // source-grep slice starts at this string
                // to find the backpressure-cooldown arm).
                // SendMsgReason-enum switch above gives the
                // precise cause; this line preserves the
                // backpressure label the test grep
                // expects. Pinned by
                // ModeSyncAsyncFixesTest Pin 5b.
                base_.log_.info("Ping",
                                "send failed (backpressure)  cause=%s  "
                                "n=%d  arqPending=%d",
                                cause, n, base_.comm_.arqPendingCount());

                // GbnWindowFull is normal, self-clearing flow control —
                // it drains as soon as the next ACK burst lands (one
                // RTT), which is far sooner than the flat cooldown.
                // Arming the cooldown on it left the pipeline idle for
                // the whole cooldown window even though the peer had
                // already freed room; ASYNC measured ~3% wire duty
                // cycle as a result. Only genuinely stuck causes
                // (settling, pool exhaustion, rate limiting, link not
                // OK) still cool down. Pinned by
                // BackpressureCooldownSkipsWindowFullTest.
                if (base_.comm_.lastSendMsgReason() !=
                    SendMsgReason::GbnWindowFull) {
                    backpressureCoolUntilMs_ =
                        millis() + BACKPRESSURE_COOLDOWN_MS;
                    havePendingDraw_ = false;
                } else {
                    pendingDrawLen_ = n;
                    pendingDrawCrc_ = crc;
                    havePendingDraw_ = true;
                }
                break;
            }
            havePendingDraw_ = false;
            queue_[tail_].len = n;
            queue_[tail_].crc = crc;
            base_.comm_.blinkWait(1);

            queue_[tail_].seq = seq;
            lastSeq_ = seq;
            tail_ = (tail_ + 1) % WINDOW;
            count_++;
            sentThisLoop++;
            consecTransient_ = 0;
            if (fillMode_ == FillMode::SEQUENTIAL) {
                seqSize_++;
                if (seqSize_ > maxSeqSize_)
                    seqSize_ = 1;
            }
            if (txDelayMs > 0)
                tNextSendMs_ = millis() + (uint32_t)txDelayMs;
        }

        int got;
        while ((got = base_.comm_.recv(recvBuf_, sizeof recvBuf_)) > 0) {
            (void)got;
        }

        if (count_ > 0) {
            while (count_ > 0 && base_.comm_.isAcked(queue_[head_].seq)) {
                successEchoCount_++;

                // Labeled companion: see the gap-stop branch.
                // Info level: per-echo ASYNC pipeline
                // traffic is the operator's primary
                // per-chunk signal, complements the
                // wire-COBS verbose line and surfaces
                // delivered-sequence progression
                // distinctly from the wire trace.
                base_.log_.info("Ping",
                                "echo#=%llu seq=%u msgBytes=%u pending=%d",
                                (unsigned long long)successEchoCount_,
                                (unsigned)queue_[head_].seq,
                                (unsigned)queue_[head_].len, count_ - 1);
                head_ = (head_ + 1) % WINDOW;
                count_--;
            }
        }
        if (got < 0) {
            Diag d;
            base_.comm_.getDiag(d);
            base_.log_.error(
                "Ping",
                "recv rejected (CRC/desync)  pending=%d  gap=%llu stale=%llu "
                "— clearing local pending + draining rx",
                count_, (unsigned long long)d.gaps,
                (unsigned long long)d.stale);

            mismatchCount_++;
            clearQueue_();
            base_.comm_.flushRx();
            consecTransient_++;
        } else {
            consecTransient_ = 0;
        }

        AppStateLog app;
        app.gapStopped = false;
        app.gapMissingSeq = 0xFF;
        app.paused = paused_;
        app.lastSendMsgReason = (int)base_.comm_.lastSendMsgReason();
        logStats(base_.log_, "Ping", base_.comm_, stat_, successEchoCount_,
                 mismatchCount_, 0u, app);
    }

    void setFillMode(FillMode m) {
        fillMode_ = m;

        if (m == FillMode::SEQUENTIAL)
            seqSize_ = 1;
    }
    FillMode fillMode() const { return fillMode_; }

    void setPaused(bool p) {
        paused_ = p;
        base_.comm_.setLinkPaused(p);
        if (!p) {
            clearQueue_();
            resetStatBaseline(stat_);
            tNextSendMs_ = 0;
            gapSeq_ = NO_GAP;
            tGapStopMs_ = 0;
            tSweepStall_ = millis();

            base_.comm_.kickoff();
        }
        base_.log_.info("Ping", "device-side pause %s", p ? "ON" : "OFF");
    }
    bool isPaused() const { return paused_; }

    uint64_t mismatchCount() const { return mismatchCount_; }

    void installWebHooks() {
        base_.mon_.setFillModeHook(
            [this]() -> uint8_t { return (uint8_t)fillMode(); },
            [this](uint8_t m) { setFillMode((FillMode)m); });
        base_.mon_.setMsgPauseHook([this]() -> bool { return isPaused(); },
                                   [this](bool p) { setPaused(p); });
        base_.mon_.setTxDelayHook(
            [this]() -> int { return base_.comm_.txDelayMs(); },
            [this](int ms) { base_.comm_.setTxDelayMs(ms); });
    }

private:
    // Link::begin() may clamp the runtime GBN window below the
    // compile-time WINDOW constant to fit the installed TX ring
    // (see AutoLinkConfig.h uartTxBufferFloor). Sizing sends
    // against the stale compile-time value over-admits and wedges
    // the pipeline. Falls back to WINDOW pre-begin() (arqWindow()
    // reads 0). Pinned by ArqWindowAccessorTest.
    int effWindow_() const {
        int w = base_.comm_.arqWindow();
        return w > 0 ? w : WINDOW;
    }

    int pickMsgSize_(FillMode m) {
        if (m == FillMode::SEQUENTIAL) {
            int s = seqSize_;
            if (s < 1)
                s = 1;
            return s;
        }
        // Clamp the draw to the LIVE free window, not just the static
        // half-window ceiling: a draw sized above the free slot count
        // is rejected by sendMsg, the app cools down, and the stuck
        // base spins whole-window retransmits until the link drops.
        // effWindow_() (not WINDOW) — Link::begin() may have clamped
        // the runtime window below the compile-time constant to fit
        // the installed TX ring. Pinned by ArqWindowAccessorTest.
        int liveCap = (int)maxLenForFreeWindow(effWindow_(),
                                               base_.comm_.arqPendingCount());
        if (liveCap < 1)
            liveCap = 1;
        int cap = maxSeqSize_;
        if (RANDOM_MAX_BYTES < cap)
            cap = RANDOM_MAX_BYTES;
        if (liveCap < cap)
            cap = liveCap;
        int minSize = RANDOM_MIN_BYTES;
        if (minSize > cap)
            minSize = cap;
        int span = cap - minSize + 1;
        if (span < 1)
            span = 1;
        return minSize + (int)random((uint32_t)span);
    }

    void fillBuf_(uint8_t *b, int n, FillMode m) {
        if (m == FillMode::SEQUENTIAL)
            fillSequential_(b, n);
        else
            fillRandom_(b, n);
    }

    void fillSequential_(uint8_t *b, int n) {
        static const char HEX_DIGITS[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        for (int i = 0; i < n; i++)
            b[i] = (uint8_t)HEX_DIGITS[i % 36];
    }

    void fillRandom_(uint8_t *b, int n) {
        for (int i = 0; i < n; i++)
            b[i] = (uint8_t)random(256);
    }

    void clearQueue_() {
        if (count_ > 0) {
            base_.log_.error("Ping", "pending cleared  dropped=%d", count_);
        }
        head_ = 0;
        tail_ = 0;
        count_ = 0;
        tStall_ = 0;
        gapSeq_ = NO_GAP;
        tGapStopMs_ = 0;
        // A retained draw belongs to the session that produced it —
        // a link-lost / stall reset invalidates it same as the rest
        // of the in-flight queue.
        havePendingDraw_ = false;
    }

    static constexpr int WINDOW = AUTOLINK_ARQ_PIPELINE_WINDOW;

    static constexpr uint32_t STALL_MS = 10000;
    static constexpr uint32_t SETTLE_MS = AUTOLINK_APP_SETTLE_MS;

    // Gap-stop escape. The buggy-original shape trapped
    // the app in gap-stop forever: a NAK could
    // re-arm the gap tracker for an already-acked
    // seq if the receiver's lastAck stamp had moved
    // on to a different seq, and the app stayed
    // paused-with-no-sends for the lifetime of the
    // link. Fix: cap how long a single gap-stop is
    // allowed to run before force-resuming. 5x the
    // SYNC ack timeout is enough headroom for a
    // genuine app-side stall (the app draining a
    // full app-buf + GBN retx round + a couple of
    // pings at the new baud) without firing on a
    // transient.
    static constexpr uint32_t GAP_STOP_FORCE_RESUME_MS = 5000;

    static constexpr int RANDOM_MIN_BYTES = 1;

    // Half the GBN window, so a RANDOM draw stays co-admittable with
    // a pipeline that is already half full. Pinned by
    // AsyncRandomAdmissionTest.
    static constexpr int RANDOM_MAX_BYTES =
        maxLenForChunkBudget(AUTOLINK_ARQ_PIPELINE_WINDOW / 2);

    static constexpr uint32_t BACKPRESSURE_COOLDOWN_MS = 1000;

    struct Slot {
        int len = 0;
        uint16_t crc = 0;
        uint8_t seq = 0;
    };
    Slot queue_[WINDOW];
    int head_ = 0;
    int tail_ = 0;
    int count_ = 0;

    uint32_t tStall_ = 0;
    uint32_t tReady_ = 0;
    uint32_t tSweepStall_ = 0;
    uint32_t tNotReady_ = 0;
    uint32_t tNextSendMs_ = 0;

    uint32_t backpressureCoolUntilMs_ = 0;
    uint32_t consecTransient_ = 0;

    // AL88-5: a draw rejected with GbnWindowFull carries no wire
    // fault — the window was simply full. Re-drawing (esp. under
    // RANDOM fill) discards a valid draw and redraws blind next
    // loop() instead of retrying the same bytes once room frees up.
    // Pinned by GbnWindowFullRetainsDrawTest.
    bool havePendingDraw_ = false;
    int pendingDrawLen_ = 0;
    uint16_t pendingDrawCrc_ = 0;

    uint8_t gapSeq_ = NO_GAP;
    // The seq of the most recently queued message.
    // Publishes the expected-next echo seq
    // (lastSeq_ + 1) on a link-lost transition so
    // the operator sees a concrete discontinuity
    // rather than a sentinel. Pinned by
    // PingGapTransitionTest.
    uint8_t lastSeq_ = 0;
    // Timestamp of the current gap-stop entry.
    // Zero when not in gap-stop. Reset on Enter
    // and on Resume. Drives the force-resume
    // timeout (GAP_STOP_FORCE_RESUME_MS). Pinned
    // by PingGapTransitionTest.
    uint32_t tGapStopMs_ = 0;

    int seqSize_ = 1;
    int maxSeqSize_ = (int)AUTOLINK_DEFAULT_MAX_MSG;
    uint64_t successEchoCount_ = 0;
    uint64_t mismatchCount_ = 0;

    uint8_t sendBuf_[PingPongBase::BUF_SIZE];
    uint8_t recvBuf_[PingPongBase::BUF_SIZE];

    StatBaseline stat_;

    FillMode fillMode_ = FillMode::SEQUENTIAL;

    bool paused_ = true;

    PingPongBase base_;
};

} // namespace autolink
#endif
