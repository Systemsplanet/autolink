// Ping role: drives wire, reads peer ACKs.
// Holds PingPongBase by composition.
//
// this release behavior changes:
//   - Pong does NOT echo the payload back. Pong's
//     response is the wire-level ACK frame (extended
//     with bytes-recvd; see LinkTx::sendAckFrame_unlocked).
//     Ping's matchEcho_() matches against an ACK, not
//     an echoed message.
//   - Sequential mode: msg size grows 1 byte per send
//     up to maxMsg, then wraps back to 1 byte.
//   - Random mode: random size 1024..maxMsg with random
//     data (NOT 1..1024 like prior releases).
//   - ASYNC gap detection: when the peer sends a NAK
//     (peer detected a gap), Ping stops sending until
//     the gap is retransmitted and ACKed. The gap seq
//     is tracked via lastNakSeq() / lastAckSeq() from
//     the Link.
//   - Consecutive-send-failure counter: 5 send() failures
//     in a row trigger dropLink() + clearQueue_() so
//     a dead peer (or app-buf-full NAK loop) bounces
//     the link back to SWP rather than spinning
//     silently.
//   - Log format on a successful ack:
//       <time> D Ping echo <seq> <bytes> <pending>
//     bytes is what the peer reported in the wire ACK.
#pragma once
#ifdef ARDUINO

#    include "al/link/arq/ArqCache.h"
#    include "al/pingpong/PingGap.h"
#    include "al/pingpong/PingPongBase.h"
#    include "al/util/UtilCrc.h"
#    include <string.h>

namespace autolink {
class Ping {
public:
    enum class FillMode : uint8_t { SEQUENTIAL = 0, RANDOM = 1 };

    // NO_GAP / NO_SEQ sentinels. 0xFF is reserved as a
    // wire NAK discriminator (LinkFrameRx.h); reuse it
    // here so the "no gap / no seq" state is a single
    // byte the link layer already uses.
    static constexpr uint8_t NO_GAP = 0xFF;

    Ping(uint32_t debugBaud, uart_port_t uartNum, int rxPin, int txPin,
         const char *ssid = nullptr, const char *password = nullptr,
         uint16_t webPort = 8765)
        : base_(debugBaud, uartNum, rxPin, txPin, true, ssid, password,
                webPort) {}

    Ping(const Ping &) = delete;
    Ping &operator=(const Ping &) = delete;

    void setup() {
        // Step 0: seed RNG — no deps.
        randomSeed(esp_random());
        const char *role = "Ping";

        // Step 1: Serial + boot banner first so we can see
        // what happens during WiFi/httpd bring-up.
        initSerial(base_.log_, base_.debugBaud_, role, base_.ssid_);

        // Step 2: WiFi + httpd (up to 5 s quick-start window;
        // bg task keeps retrying forever after that).
        // Hooks must be installed before mon.begin() so the
        // bg task has valid callbacks when httpd comes up.
        if (base_.ssid_)
            installWebHooks();
        startWebMonitor(base_.log_, base_.mon_, role, base_.ssid_,
                        base_.password_, base_.webPort_);

        // Step 3: bring up the link layer. prePaused=true so
        // no break fires until the user hits Start.
        bringUpLink(base_.log_, base_.comm_, paused_);
        // Sequential mode grows the per-send size up
        // to the configured max. Pull it from the
        // facade now so loop()'s pickMsgSize_ sees
        // the live cap, not the pre-fix hard-coded
        // 1024 (which capped sequential even when the
        // user configured a larger maxMsg).
        maxSeqSize_ = (int)base_.comm_.maxMsg();

        // Step 4: decide gate mode.
        // If web monitor is up: wait for user to hit Start.
        // If not: auto-kickoff immediately (don't leave wire silent).
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
                consecSendFail_ = 0;
                tSweepStall_ = now;
                gapSeq_ = NO_GAP;
                resetStatBaseline(stat_);
            } else {
                if (now - tNotReady_ >= 1000) {
                    if (paused_) {
                        base_.log_.debug("Ping",
                                         "not ready  paused (waiting for "
                                         "Start)");
                    } else {
                        base_.log_.debug("Ping", "not ready  swpAge=%lu ms",
                                         (unsigned long)(now - tSweepStall_));
                    }
                    tNotReady_ = now;
                }
            }
            base_.comm_.blinkWait(3, 100, 100, 0);
            return;
        }

        if (!base_.wasReady_) {
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
            consecSendFail_ = 0;
            gapSeq_ = NO_GAP;
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
            // Drain any stale ACKs from before the pause.
            // The wire-level ACK doesn't carry a payload
            // for Ping to match against, so just read and
            // discard. recv() in the new model returns 0
            // (no bytes — Pong sends ACKs through the link
            // layer's onAck, not as app-buffered data).
            uint8_t sink[PingPongBase::BUF_SIZE];
            (void)base_.comm_.recv(sink, sizeof sink);
            return;
        }

        // ASYNC gap detection. While gapSeq_ != NO_GAP,
        // a NAK from the peer is in flight: stop sending
        // new messages until the link layer has
        // retransmitted the gap chunk and the gap seq is
        // ACKed. lastNakSeq() / lastAckSeq() are
        // lock-guarded accessors on the Link; reading
        // them here is the gap-stop / gap-resume signal.
        //
        // The transition table lives in PingGap.h
        // (decideGapTransition) so the entry/transition/
        // resume logic is host-testable. The unconditional
        // read below is required — the entry edge (from
        // NO_GAP into gap-stop) has to observe a NAK
        // even when we're not already paused, otherwise
        // gap-stop never engages.
        {
            uint8_t lastNak = base_.comm_.lastNakSeq();
            uint8_t lastAck = base_.comm_.lastAckSeq();
            uint8_t nextGap = gapSeq_;
            GapAction a =
                decideGapTransition(gapSeq_, lastNak, lastAck, nextGap);
            if (a == GapAction::Enter) {
                base_.log_.warning("Ping",
                                   "gap stop: missing seq=%u — sending paused",
                                   (unsigned)nextGap);
            } else if (a == GapAction::Update) {
                base_.log_.warning(
                    "Ping",
                    "gap stop: missing seq=%u (was %u) — sending paused",
                    (unsigned)nextGap, (unsigned)gapSeq_);
            } else if (a == GapAction::Resume) {
                base_.log_.info("Ping", "gap resumed: seq=%u acked",
                                (unsigned)gapSeq_);
            }
            gapSeq_ = nextGap;
            // Suppress sends only when a gap is actually
            // active. Stay fires both for "no gap, no NAK"
            // (the normal steady state at startup and after
            // a resume) and for "in gap, waiting on the
            // retransmit" — they look the same in the action
            // enum but mean opposite things for the send
            // loop. Branch on gapSeq_ != NO_GAP instead:
            // Enter / Update / in-gap Stay all set nextGap to
            // the new gap; Stay-from-no-gap and Resume both
            // clear it.
            if (gapSeq_ != NO_GAP) {
                // Drain incoming bytes (none expected in
                // this release — Pong sends ACKs through
                // the link layer's onAck, not as
                // app-buffered data) and advance the queue
                // based on the link layer's ARQ state, then
                // return without sending.
                int got;
                while ((got = base_.comm_.recv(recvBuf_, sizeof recvBuf_)) >
                       0) {
                    (void)got;
                }
                if (count_ > 0) {
                    while (count_ > 0 &&
                           base_.comm_.isAcked(queue_[head_].seq)) {
                        successEchoCount_++;
                        // The peer's wire ACK reports the
                        // merged-chunk length pushed to its
                        // app buffer (6-byte MSG_HDR + the
                        // first chunk's payload for a
                        // multi-chunk message), not the
                        // user-visible message size. Log the
                        // message size from the local slot —
                        // that's what the operator wants.
                        base_.log_.debug("Ping", "echo %u %u %d",
                                         (unsigned)queue_[head_].seq,
                                         (unsigned)queue_[head_].len,
                                         count_ - 1);
                        head_ = (head_ + 1) % WINDOW;
                        count_--;
                    }
                }
                return;
            }
            // GapAction::Resume → fall through to the
            // send loop on this iteration. The drained
            // rx above already advanced the queue for any
            // ACKs that landed during the pause.
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
            int n = pickMsgSize_(fillMode_);
            fillBuf_(sendBuf_, n, fillMode_);
            uint16_t crc = UtilCrc::crc16(sendBuf_, n);
            uint8_t seq = 0;
            if (!base_.comm_.sendMsg(sendBuf_, n, &seq)) {
                consecSendFail_++;
                base_.log_.debug(
                    "Ping",
                    "send failed (ARQ cache full or link not OK)  n=%d  "
                    "pending=%d  consec=%lu",
                    n, count_, (unsigned long)consecSendFail_);
                if (consecSendFail_ >= MAX_SEND_FAIL) {
                    base_.log_.error("Ping",
                                     "send failed %lu times — dropping link",
                                     (unsigned long)consecSendFail_);
                    consecSendFail_ = 0;
                    clearQueue_();
                    base_.comm_.dropLink();
                }
                break;
            }
            queue_[tail_].len = n;
            queue_[tail_].crc = crc;
            // Blink once per send so a quiet
            // dashboard view shows wire activity
            // even when the peer never ACKs
            // (the gap-stop / dropLink paths
            // would otherwise be silent on
            // operators' eyes). blinkWait is
            // non-blocking in the no-delay
            // overload — fires a one-shot
            // flash on the LED.
            base_.comm_.blinkWait(1);
            // baseSeq is the FIRST chunk's cobsSeq
            // (which doubles as the message's baseSeq
            // for multi-chunk sends; see
            // Link::sendCobsFrameAcked_unlocked). The
            // peer's wire ACK for the FIRST chunk
            // carries the bytes-recvd Ping logs in its
            // "echo <seq> <bytes>" line; subsequent
            // chunks are ACKed silently at the Ping
            // app layer (the link ARQ still tracks
            // them).
            queue_[tail_].seq = seq;
            tail_ = (tail_ + 1) % WINDOW;
            count_++;
            sentThisLoop++;
            consecTransient_ = 0;
            consecSendFail_ = 0;
            // Sequential mode advances the size for the
            // NEXT send, not this one — the current send
            // already went out with n.
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
            // this release: Pong does NOT echo the payload
            // back. The wire-level ACK is the entire
            // Pong-side response; nothing to match in
            // the app buffer. Discard the read result.
            (void)got;
        }

        // this release: walk the pending queue from head and
        // free any slot whose first chunk has been ACKed.
        // In ASYNC mode each chunk gets its own wire ACK,
        // so the wire-side completion signal is the
        // FIRST chunk's ACK for the slot. The link ARQ
        // tracks intermediate chunks; Ping's pending
        // counter tracks messages. isAcked() is a public
        // Link accessor that reads arq_'s per-seq pending
        // bit without taking the lock twice.
        if (count_ > 0) {
            while (count_ > 0 && base_.comm_.isAcked(queue_[head_].seq)) {
                successEchoCount_++;
                // The peer's wire ACK reports the
                // merged-chunk length pushed to its
                // app buffer (6-byte MSG_HDR + the
                // first chunk's payload for a
                // multi-chunk message), not the
                // user-visible message size. Log the
                // message size from the local slot —
                // that's what the operator wants.
                base_.log_.debug("Ping", "echo %u %u %d",
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
            clearQueue_();
            base_.comm_.flushRx();
            consecTransient_++;
        } else {
            consecTransient_ = 0;
        }

        logStats(base_.log_, "Ping", base_.comm_, stat_, successEchoCount_,
                 mismatchCount_);
    }

    void setFillMode(FillMode m) {
        fillMode_ = m;
        // Reset the size counter so flipping to
        // SEQUENTIAL starts at 1 byte, not at whatever
        // random size the previous mode last used.
        if (m == FillMode::SEQUENTIAL)
            seqSize_ = 1;
    }
    FillMode fillMode() const { return fillMode_; }

    void setPaused(bool p) {
        paused_ = p;
        base_.comm_.setLinkPaused(p);
        if (!p) {
            // Proactive resume: drop the pending/expectation table
            // before the next echo can match against a fresh send.
            // Reactive recovery (CRC/length mismatch in matchEcho_)
            // would still catch it, but only after one spurious
            // mismatch is logged and one message lost. Clearing
            // here means stale in-flight echoes from the pre-pause
            // window are discarded as "expected empty queue" rather
            // than polluting mismatchCount_.
            clearQueue_();
            resetStatBaseline(stat_);
            tNextSendMs_ = 0;
            consecSendFail_ = 0;
            gapSeq_ = NO_GAP;
            // Stamp the sweep-stall baseline so the
            // "not ready  swpAge=..." debug line shows
            // wall-clock time from the user's Start push
            // rather than (millis - 0) = full millis
            // count. Without this stamp, the post-resume
            // swpAge is misleading on a paused boot.
            tSweepStall_ = millis();
            // Fire the wire-side SWP start now that the
            // user has pushed Start. Link::kickoff is
            // idempotent (kickedOff_ guard) so re-calls
            // after a pause/resume cycle are a no-op.
            base_.comm_.kickoff();
        }
        base_.log_.info("Ping", "device-side pause %s", p ? "ON" : "OFF");
    }
    bool isPaused() const { return paused_; }

    uint64_t successEchoCount() const { return successEchoCount_; }
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
    int pickMsgSize_(FillMode m) {
        if (m == FillMode::SEQUENTIAL) {
            // seqSize_ is bumped AFTER a successful send
            // (see loop()). Until the first send lands,
            // seqSize_ is 1.
            int s = seqSize_;
            if (s < 1)
                s = 1;
            return s;
        }
        // Random: 1k..maxMsg. The pre-this release shape was
        // 1..1024, which starved the link layer's
        // multi-frame ARQ path. The 1k floor forces
        // multi-chunk sends in the steady state so the
        // MS_HDR-then-chunk build path is exercised.
        int minSize = RANDOM_MIN_BYTES;
        if (minSize > maxSeqSize_)
            minSize = maxSeqSize_;
        int span = maxSeqSize_ - minSize + 1;
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
        consecSendFail_ = 0;
        gapSeq_ = NO_GAP;
    }

    static constexpr int WINDOW = AUTOLINK_ARQ_PIPELINE_WINDOW;

    static constexpr uint32_t STALL_MS = 10000;
    static constexpr uint32_t SETTLE_MS = 100;
    // Random mode: 1-byte floor. The previous 1024
    // floor matched the cap, collapsing the random
    // range to a single value (maxSeqSize_ - 1024 +
    // 1 == 1, so random always picked 1024).
    // Floor of 1 makes the random range
    // 1..maxSeqSize_ and exercises the chunk-build
    // path at every size.
    static constexpr int RANDOM_MIN_BYTES = 1;
    // Consecutive send() failures before Ping drops the
    // link. 5 was chosen to outlast a single
    // syncAckTimeoutMs window without being so long
    // that a dead peer stalls the wire silently.
    static constexpr uint32_t MAX_SEND_FAIL = 5;

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
    uint32_t consecTransient_ = 0;
    // Consecutive send() failures. Reset on a successful
    // send. When the counter reaches MAX_SEND_FAIL, Ping
    // clears its pending queue and drops the link so a
    // dead peer / app-buf-full NAK loop bounces back to
    // SWP instead of silently spinning.
    uint32_t consecSendFail_ = 0;
    // ASYNC gap-stop seq. NO_GAP = no gap. While
    // gapSeq_ != NO_GAP, Ping skips its send loop and
    // only drains incoming ACKs. Resumes when the link
    // layer reports the gap seq was ACKed.
    uint8_t gapSeq_ = NO_GAP;
    // Sequential-mode size cursor. 1..maxSeqSize_, wraps
    // back to 1. Bumped AFTER each successful send.
    int seqSize_ = 1;
    // Cached max seq size (= AutoLinkConfig::maxMsg).
    // Pulled out of the loop so the compiler can keep it
    // in a register across iterations.
    int maxSeqSize_ = 1024;
    uint64_t successEchoCount_ = 0;
    uint64_t mismatchCount_ = 0;

    uint8_t sendBuf_[PingPongBase::BUF_SIZE];
    uint8_t recvBuf_[PingPongBase::BUF_SIZE];

    // Owned by Ping — logStats is a
    // free function, the rate window
    // lives with the loop that resets
    // it.
    StatBaseline stat_;

    FillMode fillMode_ = FillMode::SEQUENTIAL;
    // Default to PAUSED so a fresh sketch boots
    // with the Start button pushed-in and no
    // messages flow until the user releases it.
    // The dashboard labels "Start" / "Pause"
    // already encode this state on the JS side.
    bool paused_ = true;

    PingPongBase base_;
};

} // namespace autolink
#endif
