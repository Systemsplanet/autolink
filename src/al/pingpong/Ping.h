// Ping role: drives the wire, reads peer wire-ACKs
// (Pong does not echo payloads). Sequential mode grows
// msg size 1..maxMsg then wraps; random mode picks
// 1..maxMsg. On a peer NAK, sending gap-stops until the
// gap seq is ACKed. Success log: "echo <seq> <bytes>
// <pending>".
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
        randomSeed(esp_random());
        const char *role = "Ping";

        initSerial(base_.log_, base_.debugBaud_, role, base_.ssid_);

        // Hooks must be installed before mon.begin() so the
        // bg task has valid callbacks when httpd comes up.
        if (base_.ssid_)
            installWebHooks();
        startWebMonitor(base_.log_, base_.mon_, role, base_.ssid_,
                        base_.password_, base_.webPort_);

        // prePaused: no break until the user hits Start.
        bringUpLink(base_.log_, base_.comm_, paused_);
        // Live cap from the facade, not a hard-coded max.
        maxSeqSize_ = (int)base_.comm_.maxMsg();

        // Web monitor up: gate on Start. Down: auto-kick
        // so the wire isn't left silent.
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
                gapSeq_ = NO_GAP;
                backpressureCoolUntilMs_ = 0;
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
            // Flush the half-received frame a link reset
            // truncates, or the next recvMsg fails CRC.
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
            // Drain stale pre-pause data; wire ACKs carry
            // nothing for the app layer to match.
            uint8_t sink[PingPongBase::BUF_SIZE];
            (void)base_.comm_.recv(sink, sizeof sink);
            return;
        }

        // ASYNC gap-stop: on a peer NAK, stop sending
        // until the gap seq is ACKed. Transition table:
        // PingGap.h (host-tested). The read must be
        // unconditional or the entry edge never engages.
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
            // Branch on gapSeq_, not the action: Stay is
            // ambiguous (no-gap steady state vs in-gap
            // waiting).
            if (gapSeq_ != NO_GAP) {
                // Drain rx + advance the queue on ARQ
                // state, then return without sending.
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
            // Resume falls through to the send loop.
        }

        int sentThisLoop = 0;
        const int maxTx = (base_.comm_.mode() == AutoLinkConfig::Mode::SYNC)
            ? 1
            : PingPongBase::MAX_TX_PER_LOOP;
        const int txDelayMs = base_.comm_.txDelayMs();
        if (txDelayMs > 0 && tNextSendMs_ == 0)
            tNextSendMs_ = now;
        while (count_ < WINDOW && sentThisLoop < maxTx) {
            // Two orthogonal gates: txDelayMs paces
            // user-requested delay; the backpressure
            // cooldown fires regardless (ASYNC flood has
            // txDelayMs=0 and must still throttle).
            if (txDelayMs > 0 && (int32_t)(now - tNextSendMs_) < 0)
                break;
            if (backpressureCoolUntilMs_ != 0 &&
                (int32_t)(now - backpressureCoolUntilMs_) < 0)
                break;
            int n = pickMsgSize_(fillMode_);
            fillBuf_(sendBuf_, n, fillMode_);
            uint16_t crc = UtilCrc::crc16(sendBuf_, n);
            uint8_t seq = 0;
            if (!base_.comm_.sendMsg(sendBuf_, n, &seq)) {
                // Never dropLink from here: a not-OK link
                // is self-recovering, and backpressure on a
                // saturated cache is not a dead peer — a
                // counter-driven drop resweeps into an
                // immediately re-flooded cache and cascades
                // disc on both peers. Cooldown only.
                if (!base_.comm_.ready()) {
                    break;
                }
                base_.log_.debug("Ping",
                                 "send failed (backpressure)  n=%d  "
                                 "arqPending=%d",
                                 n, base_.comm_.arqPendingCount());
                // Wall-clock cooldown so the link task can
                // drain the cache (SYNC blocks inline and
                // never trips this).
                backpressureCoolUntilMs_ = millis() + BACKPRESSURE_COOLDOWN_MS;
                break;
            }
            queue_[tail_].len = n;
            queue_[tail_].crc = crc;
            base_.comm_.blinkWait(1);
            // seq = FIRST chunk's cobsSeq (the message
            // baseSeq); later chunks are link-ARQ-tracked
            // but silent at the app layer.
            queue_[tail_].seq = seq;
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
            (void)got; // no payload echo; ACKs are wire-level
        }

        // Free head slots whose FIRST chunk is ACKed —
        // that's the message-completion signal; the link
        // ARQ tracks intermediate chunks.
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
            // Proactive resume: drop stale pre-pause
            // expectations here rather than letting the
            // reactive mismatch path catch them.
            clearQueue_();
            resetStatBaseline(stat_);
            tNextSendMs_ = 0;
            gapSeq_ = NO_GAP;
            tSweepStall_ = millis();
            // kickoff is idempotent (kickedOff_ guard).
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
            int s = seqSize_;
            if (s < 1)
                s = 1;
            return s;
        }
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
        gapSeq_ = NO_GAP;
    }

    static constexpr int WINDOW = AUTOLINK_ARQ_PIPELINE_WINDOW;

    static constexpr uint32_t STALL_MS = 10000;
    static constexpr uint32_t SETTLE_MS = 100;
    // 1-byte floor keeps random mode spanning
    // 1..maxSeqSize_ (exercises every chunk shape).
    static constexpr int RANDOM_MIN_BYTES = 1;
    // Wall-clock gap after a backpressure sendMsg
    // failure; ~one syncAckTimeoutMs round trip for
    // the ARQ loop to make cache room.
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
    // Backpressure cooldown stamp; honored regardless
    // of txDelayMs (tNextSendMs_ is not when delay=0).
    uint32_t backpressureCoolUntilMs_ = 0;
    uint32_t consecTransient_ = 0;
    // ASYNC gap-stop seq (NO_GAP = none): send loop
    // paused until the gap seq is ACKed.
    uint8_t gapSeq_ = NO_GAP;
    // Sequential size cursor, bumped after each send.
    int seqSize_ = 1;
    int maxSeqSize_ = (int)AUTOLINK_DEFAULT_MAX_MSG;
    uint64_t successEchoCount_ = 0;
    uint64_t mismatchCount_ = 0;

    uint8_t sendBuf_[PingPongBase::BUF_SIZE];
    uint8_t recvBuf_[PingPongBase::BUF_SIZE];

    StatBaseline stat_;

    FillMode fillMode_ = FillMode::SEQUENTIAL;
    // Boot paused: nothing flows until Start.
    bool paused_ = true;

    PingPongBase base_;
};

} // namespace autolink
#endif
