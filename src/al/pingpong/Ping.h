// Ping role: drives wire, verifies echoes.
// Holds PingPongBase by composition.
#pragma once
#ifdef ARDUINO

#    include "al/pingpong/PingPongBase.h"
#    include "al/util/UtilCrc.h"
#    include <string.h>

namespace autolink
{
class Ping
{
public:
    enum class FillMode : uint8_t { SEQUENTIAL = 0, RANDOM = 1 };

    Ping(uint32_t debugBaud, uart_port_t uartNum, int rxPin, int txPin,
         const char *ssid = nullptr, const char *password = nullptr,
         uint16_t webPort = 8765)
        : base_(debugBaud, uartNum, rxPin, txPin, true, ssid, password, webPort)
    {
    }

    Ping(const Ping &) = delete;
    Ping &operator=(const Ping &) = delete;

    void setup()
    {
        base_.log_.debug("Ping", "setup: seeding RNG, calling setupCommon");
        randomSeed(esp_random());
        if (base_.ssid_)
            installWebHooks();
        base_.setupCommon();
        base_.comm_.setLinkPaused(paused_);
        base_.log_.info("Ping", "mode=Ping  ready");
    }

    void loop()
    {
        uint32_t now = millis();

        if (!base_.comm_.ready()) {
            if (base_.wasReady_) {
                base_.log_.info("Ping", "link lost  pending=%d", count_);
                base_.wasReady_ = false;
                clearQueue_();
                tSweepStall_ = now;
                base_.resetStatBaseline();
            } else {
                if (now - tNotReady_ >= 1000) {
                    base_.log_.debug("Ping", "not ready  swpAge=%lu ms",
                                     (unsigned long)(now - tSweepStall_));
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
                    "pipeline stall — WINDOW=%d full for %lu ms. Clearing local pending table only (NO flushRx, NO BREAK). pending=%d  consec=%lu",
                    WINDOW, (unsigned long)(now - tStall_), count_,
                    (unsigned long)consecTransient_);
                clearQueue_();
                consecTransient_++;
            }
        } else {
            tStall_ = 0;
        }

        if (paused_) {
            uint8_t echoBuf[PingPongBase::BUF_SIZE];
            int n;
            while ((n = base_.comm_.recv(echoBuf, sizeof echoBuf)) > 0) {
                matchEcho_(n, echoBuf);
            }
            return;
        }

        int sentThisLoop = 0;
        const int maxTx = (base_.comm_.mode() == AutoLinkConfig::Mode::SYNC)
            ? 1
            : MAX_TX_PER_LOOP;
        while (count_ < WINDOW && sentThisLoop < maxTx) {
            int n = random(1, 1024);
            fillBuf_(sendBuf_, n);
            uint16_t crc = UtilCrc::crc16(sendBuf_, n);

            if (!base_.comm_.send(sendBuf_, n)) {
                base_.log_.debug(
                    "Ping",
                    "send failed (ARQ cache full or link not OK)  n=%d  pending=%d",
                    n, count_);
                break;
            }
            queue_[tail_].len = n;
            queue_[tail_].crc = crc;
            tail_ = (tail_ + 1) % WINDOW;
            count_++;
            sentThisLoop++;
            consecTransient_ = 0;
        }

        int got;
        while ((got = base_.comm_.recv(recvBuf_, sizeof recvBuf_)) > 0) {
            matchEcho_(got, recvBuf_);
        }
        if (got < 0) {
            Diag d;
            base_.comm_.getDiag(d);
            base_.log_.error(
                "Ping",
                "recv rejected (CRC/desync)  pending=%d  gap=%llu stale=%llu — clearing local pending only (NO flushRx, NO BREAK)",
                count_, (unsigned long long)d.gaps,
                (unsigned long long)d.stale);
            clearQueue_();
            consecTransient_++;
        } else {
            consecTransient_ = 0;
        }

        base_.logStats("Ping", successEchoCount_, mismatchCount_);
    }

    void setFillMode(FillMode m) { fillMode_ = m; }
    FillMode fillMode() const { return fillMode_; }

    void setPaused(bool p)
    {
        paused_ = p;
        base_.comm_.setLinkPaused(p);
        if (!p)
            base_.resetStatBaseline();
        base_.log_.info("Ping", "device-side pause %s", p ? "ON" : "OFF");
    }
    bool isPaused() const { return paused_; }

    uint64_t successEchoCount() const { return successEchoCount_; }
    uint64_t mismatchCount() const { return mismatchCount_; }

    void installWebHooks()
    {
        base_.mon_.setFillModeHook(
            [this]() -> uint8_t { return (uint8_t)fillMode(); },
            [this](uint8_t m) { setFillMode((FillMode)m); });
        base_.mon_.setMsgPauseHook([this]() -> bool { return isPaused(); },
                                   [this](bool p) { setPaused(p); });
    }

private:
    void fillBuf_(uint8_t *b, int n)
    {
        if (fillMode_ == FillMode::SEQUENTIAL)
            fillSequential_(b, n);
        else
            fillRandom_(b, n);
    }

    void fillSequential_(uint8_t *b, int n)
    {
        static const char HEX_DIGITS[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        for (int i = 0; i < n; i++)
            b[i] = (uint8_t)HEX_DIGITS[i % 36];
    }

    void fillRandom_(uint8_t *b, int n)
    {
        for (int i = 0; i < n; i++)
            b[i] = (uint8_t)random(256);
    }

    void matchEcho_(int got, const uint8_t *buf)
    {
        base_.comm_.blinkWait(1);
        if (count_ == 0) {
            base_.log_.error(
                "Ping",
                "recv %d bytes with empty pending queue (stale echo?) — discarding",
                got);
            return;
        }
        const Slot &head = queue_[head_];
        uint16_t gotCrc = UtilCrc::crc16(buf, got);
        if (head.len == got && head.crc == gotCrc) {
            successEchoCount_++;
            base_.log_.debug("Ping", "echo ok  %d bytes  pending=%d", got,
                             count_ - 1);
            head_ = (head_ + 1) % WINDOW;
            count_--;
        } else {
            Diag d;
            base_.comm_.getDiag(d);
            base_.log_.error(
                "Ping",
                "echo MISMATCH: got len=%d crc=0x%04X, expected len=%d crc=0x%04X (pending=%d gap=%llu stale=%llu) — clearing local pending",
                got, (unsigned)gotCrc, head.len, (unsigned)head.crc, count_,
                (unsigned long long)d.gaps, (unsigned long long)d.stale);
            mismatchCount_++;
            clearQueue_();
            consecTransient_++;
        }
    }

    void clearQueue_()
    {
        if (count_ > 0) {
            base_.log_.error("Ping", "pending cleared  dropped=%d", count_);
        }
        head_ = 0;
        tail_ = 0;
        count_ = 0;
        tStall_ = 0;
    }

    static constexpr int WINDOW = 32;
    static constexpr int MAX_TX_PER_LOOP = 16;

    static constexpr uint32_t STALL_MS = 10000;
    static constexpr uint32_t SETTLE_MS = 100;

    struct Slot {
        int len = 0;
        uint16_t crc = 0;
    };
    Slot queue_[WINDOW];
    int head_ = 0;
    int tail_ = 0;
    int count_ = 0;

    uint32_t tStall_ = 0;
    uint32_t tReady_ = 0;
    uint32_t tSweepStall_ = 0;
    uint32_t tNotReady_ = 0;
    uint32_t consecTransient_ = 0;
    uint64_t successEchoCount_ = 0;
    uint64_t mismatchCount_ = 0;

    uint8_t sendBuf_[PingPongBase::BUF_SIZE];
    uint8_t recvBuf_[PingPongBase::BUF_SIZE];

    FillMode fillMode_ = FillMode::SEQUENTIAL;
    bool paused_ = false;

    PingPongBase base_;
};

} // namespace autolink
#endif