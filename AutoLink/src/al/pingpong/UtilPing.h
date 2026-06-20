// UtilPing.h — plug-and-play AutoLink ping node for the ping-pong echo test.
// Sends random-length messages; UtilPong echoes each one back. Match
// is by (len, crc) per pending slot — wire-layer cobsSeq guarantees
// ordering/freshness so a wire-byte shift can't desync the matcher.
#pragma once
#ifdef ARDUINO

#include "al/pingpong/UtilMain.h"
#include "al/util/UtilCrc.h"
#include <string.h>

namespace autolink {

class UtilPing : public UtilMain {
public:
    // Sequential = deterministic ASCII hex digits (eyeball-friendly in
    // wire captures). Random = random bytes (legacy default).
    enum class FillMode : uint8_t { SEQUENTIAL = 0, RANDOM = 1 };

    UtilPing(uint32_t    debugBaud,
             uart_port_t uartNum,
             int         rxPin,
             int         txPin,
             const char* ssid     = nullptr,
             const char* password = nullptr,
             uint16_t    webPort  = 8765)
        : UtilMain(debugBaud, uartNum, rxPin, txPin, /*isPing=*/true,
                   ssid, password, webPort)
    {
        // s_active_ must be set before mon_.begin() (in setupCommon)
        // so the /mode endpoint is wired before the HTTP server starts.
        s_active_ = this;
    }
    ~UtilPing() { if (s_active_ == this) s_active_ = nullptr; }

    UtilPing(const UtilPing&)            = delete;
    UtilPing& operator=(const UtilPing&) = delete;

    void setup() {
        log_.debug("Ping", "setup: seeding RNG, calling setupCommon");
        randomSeed(esp_random());
        if (ssid_) installWebHooks();
        setupCommon();
        // v5.1.31: paused_ defaults true. Propagate to protocol so
        // it suppresses idle-drop and keepalive emissions from the
        // very first onTimer tick (not just after /pausemsg fires).
        // Without this, the first ~5 s after boot would still try
        // keepalives and might still drop the link if the operator
        // is slow to click Start.
        comm_.setLinkPaused(paused_);
        log_.debug("Ping",
            "setup complete  WINDOW=%d  MAX_TX_PER_LOOP=%d  "
            "SETTLE_MS=%lu  STALL_MS=%lu  BUF_SIZE=%d",
            WINDOW, MAX_TX_PER_LOOP,
            (unsigned long)SETTLE_MS, (unsigned long)STALL_MS, BUF_SIZE);
        log_.info("Ping", "mode=Ping  ready");
    }

    void loop() {
        if (!comm_.ready()) {
            uint32_t now = millis();
            if (wasReady_) {
                log_.info("Ping", "link lost  pending=%d", pendingCount_);
                wasReady_ = false;
                resetPending_();
                tSweepStall_ = now;
            } else {
                if (now - tSweepStall_ > SWEEP_STALL_MS) {
                    log_.info("Ping",
                        "SWP stall — no sweep progress for %lu ms, forcing BREAK to restart",
                        (unsigned long)(now - tSweepStall_));
                    comm_.dropLink();
                    tSweepStall_ = now;
                }
                if (now - tNotReady_ >= 1000) {
                    log_.debug("Ping", "not ready  swpAge=%lu ms",
                        (unsigned long)(now - tSweepStall_));
                    tNotReady_ = now;
                }
            }
            comm_.blinkWait(3, 100, 100, 0);
            return;
        }
        if (!wasReady_) {
            log_.debug("Ping", "link up  baud=%lu  settling %lu ms",
                (unsigned long)comm_.getCurrentBaud(),
                (unsigned long)SETTLE_MS);
            // Drain stale bytes before settle (cobsSeq also catches them).
            int drained = 0;
            while (comm_.recv(recvBuf_, sizeof recvBuf_) > 0) drained++;
            if (drained) log_.debug("Ping", "drained %d stale echo(s) pre-settle", drained);
            comm_.blinkWait(4);
            tReady_ = millis();
            wasReady_ = true;
        }

        if (millis() - tReady_ < SETTLE_MS) {
            log_.debug("Ping", "settling  %lu ms remaining",
                (unsigned long)(SETTLE_MS - (millis() - tReady_)));
            return;
        }

        // Pipeline stall: full window with no drain for STALL_MS.
        // With cobsSeq matching, gaps auto-recover in one frame — a
        // 3 s full-window stall means Ping is talking to itself or
        // the wire is dead.
        uint32_t now = millis();
        if (pendingCount_ == WINDOW) {
            if (tStall_ == 0) tStall_ = now;
            if (now - tStall_ > STALL_MS) {
                log_.error("Ping",
                    "pipeline stall — WINDOW=%d full for %lu ms, no echoes. "
                    "Clearing pending. pending=%d",
                    WINDOW, (unsigned long)(now - tStall_), pendingCount_);
                resetPending_("stall", /*dropLink=*/true);
            }
        } else {
            tStall_ = 0;
        }

        // Cap per-loop sends so the pipeline fills over a few ticks
        // instead of one burst (a single burst overruns Pong's RX).
        //
        // v5.1.29: respect device-side pause. When paused_, return
        // before the send loop so the wire stays quiet. Pong will still
        // process echoes from anything already in flight, but nothing
        // new leaves Ping. Recv/echo handling is unaffected — Pong
        // echoes come back through whatever's in flight when pause was
        // toggled. The dashboard's Pause/Resume button now POSTs
        // /pausemsg which flips paused_ via the hook.
        if (paused_) {
            // Keep a small liveness log so the operator can see pause
            // is active (not just silent). One line every 5 s.
            static uint32_t lastPausedLog_ = 0;
            if (now - lastPausedLog_ > 5000) {
                log_.debug("Ping", "paused (waiting for /pausemsg?p=0 from dashboard)");
                lastPausedLog_ = now;
            }
            // Still drain the app buf for any in-flight echoes; just
            // don't originate new sends.
            uint8_t echoBuf[BUF_SIZE];
            int n;
            while ((n = comm_.recv(echoBuf, sizeof echoBuf)) > 0) {
                // Track echoed sends; pendingCount_ decrements because
                // the echo CRC matches one of our slots. This is the
                // normal pipeline completion path; we just don't refill.
                for (int i = 0; i < WINDOW; i++) {
                    if (pending_[i].active && pending_[i].len == n) {
                        // Cheap len-match; could verify CRC but for a
                        // random payload, len alone is enough to mark
                        // it completed. Pong echoes bytes verbatim.
                        pending_[i].active = false;
                        if (pendingCount_ > 0) pendingCount_--;
                        break;
                    }
                }
            }
            return;
        }

        int sentThisLoop = 0;
        while (pendingCount_ < WINDOW && sentThisLoop < MAX_TX_PER_LOOP) {
            int n = random(1, 1024);
            fillBuf_(sendBuf_, n);
            // (len, crc) is unique per send for random payloads
            // (which fill_ produces by default); cobsSeq at the wire
            // layer has already guaranteed ordering.
            uint16_t crc = UtilCrc::crc16(sendBuf_, n);
            int slot = -1;
            for (int i = 0; i < WINDOW; i++) {
                if (!pending_[i].active) { slot = i; break; }
            }
            if (slot < 0) break;

            if (!comm_.send(sendBuf_, n)) {
                log_.debug("Ping",
                    "send failed (link dropped)  n=%d  pending=%d", n, pendingCount_);
                break;
            }
            pending_[slot].active = true;
            pending_[slot].len = n;
            pending_[slot].crc = crc;
            pendingCount_++;
            sentThisLoop++;
        }
        if (sentThisLoop > 0) {
            Diag d; comm_.getDiag(d);
            log_.debug("Ping", "sent %d msgs  pending=%d  cobsSeq gap=%llu stale=%llu",
                sentThisLoop, pendingCount_,
                (unsigned long long)d.gaps,
                (unsigned long long)d.stale);
        }

        int got;
        while ((got = comm_.recv(recvBuf_, sizeof recvBuf_)) > 0) {
            if (pendingCount_ == 0) {
                log_.error("Ping",
                    "recv %d bytes with no in-flight send (stale echo?) — discarding",
                    got);
                continue;
            }
            comm_.blinkWait(1);

            uint16_t gotCrc = UtilCrc::crc16(recvBuf_, got);
            int slot = -1;
            for (int i = 0; i < WINDOW; i++) {
                if (pending_[i].active
                    && pending_[i].len == got
                    && pending_[i].crc == gotCrc) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                Diag d; comm_.getDiag(d);
                log_.error("Ping",
                    "STALE echo: no matching pending slot for %d bytes crc=0x%04X "
                    "(pending=%d, gap=%llu stale=%llu) — dropping",
                    got, (unsigned)gotCrc, pendingCount_,
                    (unsigned long long)d.gaps,
                    (unsigned long long)d.stale);
                continue;
            }
            pending_[slot].active = false;
            pendingCount_--;
            log_.debug("Ping", "echo ok slot=%d  %d bytes  crc=0x%04X  pending=%d",
                slot, got, (unsigned)gotCrc, pendingCount_);
        }
        if (got < 0) {
            // Link-layer CRC/desync reject. cobsSeq catches most of
            // the cases that used to land here. Clear pending and
            // let the next round start fresh.
            Diag d; comm_.getDiag(d);
            log_.error("Ping",
                "recv rejected (CRC/desync)  pending=%d  gap=%llu stale=%llu "
                "— clearing pending",
                pendingCount_,
                (unsigned long long)d.gaps,
                (unsigned long long)d.stale);
            resetPending_("recv reject", /*dropLink=*/true);
        }

        logStats("Ping");
    }

private:
    FillMode fillMode_ = FillMode::SEQUENTIAL;
    bool paused_       = true; // v5.1.29: boot paused; dashboard /pausemsg flips this

public:
    // Safe to call from the GUI handler (HTTP task). Takes effect on
    // the next fillBuf_() call.
    void setFillMode(FillMode m) {
        log_.info("Ping", "fill mode changed: %s -> %s",
            m == FillMode::SEQUENTIAL ? "sequential" : "random",
            m == FillMode::SEQUENTIAL ? "sequential" : "random");
        fillMode_ = m;
    }

    FillMode fillMode() const { return fillMode_; }

    // AutoLinkWeb takes plain C function pointers (no user_data), so
    // we route through a static instance pointer set by the
    // constructor. A single UtilPing is ever live per board (PingPong
    // holds a Ping OR a Pong, never both).
    static UtilPing* s_active_;
    static uint8_t fillModeReaderThunk_() {
        return s_active_ ? (uint8_t)s_active_->fillMode() : 0;
    }
    static void fillModeWriterThunk_(uint8_t m) {
        if (s_active_) s_active_->setFillMode((FillMode)m);
    }

    // v5.1.29: device-side message pause. Ping always boots paused
    // (paused_=true) and stays paused until the dashboard POSTs
    // /pausemsg?p=0 (or POSTs ?p=1 to re-pause). The Pause/Resume
    // button in the dashboard was previously a JS-only toggle that
    // affected log polling but not Ping's send loop — Ping would
    // blast bytes from the moment the link settled, regardless of
    // what the operator did in the UI.
    void        setPaused(bool p) {
        paused_ = p;
        // v5.1.31: also tell the protocol layer to stop its idle
        // watchdog and keepalive emissions while paused. Without
        // this, the link would drop after idleTimeoutMs (5 s) of
        // silence, forcing Ping to re-sweep before the operator even
        // clicks Start. With this, the link stays up silently.
        comm_.setLinkPaused(p);
        log_.info("Ping", "device-side pause %s", p ? "ON" : "OFF");
    }
    bool        isPaused() const { return paused_; }
    static bool pausedReaderThunk_() {
        return s_active_ ? s_active_->isPaused() : false;
    }
    static void pausedWriterThunk_(bool p) {
        if (s_active_) s_active_->setPaused(p);
    }

    void installWebHooks() {
        mon_.setFillModeHook(&UtilPing::fillModeReaderThunk_,
                             &UtilPing::fillModeWriterThunk_);
        mon_.setMsgPauseHook(&UtilPing::pausedReaderThunk_,
                             &UtilPing::pausedWriterThunk_);
    }

private:
    void fillBuf_(uint8_t* b, int n) {
        if (fillMode_ == FillMode::SEQUENTIAL) fillSequential_(b, n);
        else                                   fillRandom_(b, n);
    }

    // ASCII hex digits, 36 distinct values. Wraps every 36^2=1296 bytes.
    // (Renamed from HEX[] because Arduino's Print.h #defines HEX as 16.)
    void fillSequential_(uint8_t* b, int n) {
        static const char HEX_DIGITS[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        for (int i = 0; i < n; i++) b[i] = (uint8_t)HEX_DIGITS[i % 36];
    }

    void fillRandom_(uint8_t* b, int n) {
        for (int i = 0; i < n; i++) b[i] = (uint8_t)random(256);
    }

    // Clear the in-flight pending list.
    //   reason:    log label.
    //   dropLink:  true for desync events (stall, recv reject). Sends
    //              BREAK because flushRx() alone lets the UART event
    //              task refill the stream buffer faster than we drain.
    void resetPending_(const char* reason = "link drop", bool dropLink = false) {
        if (pendingCount_ > 0) {
            log_.error("Ping", "pending cleared (%s)  dropped=%d",
                reason, pendingCount_);
        }
        for (int i = 0; i < WINDOW; i++) pending_[i].active = false;
        pendingCount_ = 0;
        tStall_ = 0;
        if (dropLink) {
            log_.error("Ping",
                "BREAK sent (desync recovery: %s) — forcing re-sweep", reason);
            comm_.dropLink();
        } else {
            comm_.flushRx();
        }
    }

    // WINDOW=32 is a memory back-pressure cap, not a rate gate —
    // with the auto-sized TX ring, uart_write_bytes never blocks
    // while the link lock is held. Cap matters only in a real stall.
    static constexpr int      WINDOW    = 32;
    // MAX_TX_PER_LOOP=16 = WINDOW/2: half the window in one tick,
    // matching the "half the window per loop miss" rule.
    static constexpr int      MAX_TX_PER_LOOP = 16;
    static constexpr uint32_t STALL_MS        = 3000;
    static constexpr uint32_t SWEEP_STALL_MS  = 2000;
    // Belt-and-suspenders settle for Pong's lock; cobsSeq already
    // rejects stale frames, so 100 ms is enough.
    static constexpr uint32_t SETTLE_MS       = 100;

    struct Pending { bool active = false; int len = 0; uint16_t crc = 0; };
    Pending  pending_[WINDOW];

    int      pendingCount_ = 0;
    uint32_t tStall_       = 0;
    uint32_t tReady_       = 0;
    uint32_t tSweepStall_  = 0;
    uint32_t tNotReady_    = 0;

    // Separate TX/RX buffers so recv() can't overwrite a payload
    // whose CRC is still pending comparison.
    uint8_t sendBuf_[BUF_SIZE];
    uint8_t recvBuf_[BUF_SIZE];
};

// Single global — one UtilPing is ever live per board.
UtilPing* UtilPing::s_active_ = nullptr;

} // namespace autolink
#endif // ARDUINO
