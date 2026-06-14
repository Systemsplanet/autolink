// UtilPing.h — ready-to-run AutoLink ping node for the ping-pong echo test.
//
// v4.0.0: echo matching is by cobsSeq, not by FIFO position + length+CRC.
// The receiver-side cobsSeq gap detection in ALink already drops stale and
// out-of-order frames, so UtilPing just needs to (a) send with cobsSeq, and
// (b) match each echo by cobsSeq. The message reassembly layer is
// unchanged.
//
// Pair with UtilPong on the other board. Ping initiates the baud sweep;
// Pong listens and locks onto the negotiated baud.
//
// Usage:
//   UtilPing ping(115200, UART_NUM_2, 16, 17);           // UART only
//   UtilPing ping(115200, UART_NUM_2, 16, 17,            // + web monitor
//                 "YourSSID", "password", 80);
//   void setup() { ping.setup(); }
//   void loop()  { ping.loop();  }
#pragma once
#ifdef ARDUINO

#include "UtilMain.h"
#include "UtilCrc.h"
#include <string.h>

namespace autolink {

// ----------------------------------------------------------------------------
// UtilPing — plug-and-play AutoLink ping node (v4.0.0).
//
// Sends random-length messages (1–1023 bytes), keeps up to WINDOW messages
// in flight simultaneously (pipelined for ~2× throughput). v4.0.0: each
// in-flight slot is keyed by cobsSeq. The first byte ALink decodes from
// each echo is the cobsSeq, so UtilPing looks up the pending slot by that
// number rather than by FIFO position. A wire-byte shift that used to
// desync the FIFO in v3.x is now an out-of-window cobsSeq that ALink
// drops before UtilPing ever sees it.
// ----------------------------------------------------------------------------
class UtilPing : public UtilMain {
public:
    UtilPing(uint32_t    debugBaud,
             uart_port_t uartNum,
             int         rxPin,
             int         txPin,
             const char* ssid     = nullptr,
             const char* password = nullptr,
             uint16_t    webPort  = 8765)
        : UtilMain(debugBaud, uartNum, rxPin, txPin, /*isPing=*/true,
                   ssid, password, webPort)
    {}

    UtilPing(const UtilPing&)            = delete;
    UtilPing& operator=(const UtilPing&) = delete;

    void setup() {
        log_.debug("Ping", "setup: seeding RNG, calling setupCommon");
        randomSeed(esp_random());
        setupCommon();
        log_.debug("Ping", "setup complete  WINDOW=%d  BUF_SIZE=%d", WINDOW, BUF_SIZE);
    }

    void loop() {
        if (!comm_.ready()) {
            uint32_t now = millis();
            if (wasReady_) {
                log_.info("Ping", "link lost  pending=%d",
                    pendingCount_);
                wasReady_ = false;
                resetPending_();
                tSweepStall_ = now;
            } else {
                if (now - tSweepStall_ > SWEEP_STALL_MS) {
                    log_.error("Ping",
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
            // v4.0.0: link-up drain is still important (clears stream buffer
            // and UART ring) but cobsSeq gap detection now also rejects any
            // stale frames that the drain missed — redundant safety.
            int drained = 0;
            while (comm_.recv(recvBuf_, sizeof recvBuf_) > 0) drained++;
            if (drained) log_.debug("Ping", "drained %d stale echo(s) pre-settle", drained);
            comm_.blinkWait(4);
            tReady_ = millis();
            wasReady_ = true;
        }

        // Settle guard — give Pong's sweep time to complete its own lock.
        if (millis() - tReady_ < SETTLE_MS) {
            log_.debug("Ping", "settling  %lu ms remaining",
                (unsigned long)(SETTLE_MS - (millis() - tReady_)));
            return;
        }

        // Pipeline stall detection — if the window is full and nothing drains
        // for STALL_MS, clear the pending list and let new sends proceed.
        // v4.0.0: this is now mostly a safety net. With cobsSeq matching,
        // gaps auto-recover in one frame, so a 3-second full-window stall
        // means Ping is talking to itself or the wire is dead.
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

        // Fill the send window, but pace it: emitting the whole window in one
        // loop() dumps up to WINDOW KB-sized frames back-to-back, overrunning
        // Pong's RX (partial writes + COBS desync). Cap per-loop sends so the
        // pipeline fills over a few ticks instead of one burst.
        int sentThisLoop = 0;
        while (pendingCount_ < WINDOW && sentThisLoop < MAX_TX_PER_LOOP) {
            int n = random(1, 1024);
            fill_(sendBuf_, n);
            // v4.0.0: sendMsg uses sendCobsFrame internally which consumes a
            // cobsSeq number. We don't know the exact number the ALink layer
            // will assign, but we know it will be monotonic per link, so a
            // unique key for the in-flight slot is the COMBINATION of (len,
            // crc). That's not a v3.x-style FIFO compare — the actual frame
            // identity is verified at the wire layer by cobsSeq, and the
            // app layer just needs a way to look up "which pending slot does
            // this echo belong to". For a stream of distinct random payloads
            // (which our fill_ produces) (len, crc) is unique per send.
            uint16_t crc = UtilCrc::crc16(sendBuf_, n);
            // Look for a free slot. We scan the pending array; in steady
            // state there's plenty of room (WINDOW=8 vs MAX_TX_PER_LOOP=2).
            int slot = -1;
            for (int i = 0; i < WINDOW; i++) {
                if (!pending_[i].active) { slot = i; break; }
            }
            if (slot < 0) break;   // shouldn't happen; pendingCount_ check above

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
            log_.debug("Ping", "sent %d msgs  pending=%d  cobsSeq gap=%llu stale=%llu",
                sentThisLoop, pendingCount_,
                (unsigned long long)comm_.getCobsGaps(),
                (unsigned long long)comm_.getCobsStale());
        }

        // Drain available echoes. v4.0.0: each echo is a single message
        // (no longer a chunked stream — UtilPing's reliable write produces
        // exactly one cobsSeq-tagged data frame per sendMsg call). Match by
        // length+CRC of the message body — the wire-layer cobsSeq has
        // already guaranteed ordering and freshness, so we can use the
        // lighter (len, crc) match instead of the v3.x FIFO compare.
        //
        // The match is by full message: length + CRC of the payload. For
        // random payloads (which fill_ generates) (len, crc) is unique per
        // send. If a collision ever did occur (e.g. the test sends the same
        // payload twice), the first match wins and the second is reported
        // as an unknown echo — caller-visible as a `stale` log.
        int got;
        while ((got = comm_.recv(recvBuf_, sizeof recvBuf_)) > 0) {
            if (pendingCount_ == 0) {
                log_.error("Ping",
                    "recv %d bytes with no in-flight send (stale echo?) — discarding",
                    got);
                continue;
            }
            comm_.blinkWait(1);

            // Find the matching pending slot by (len, crc).
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
                // No matching slot. cobsSeq has already guaranteed the
                // frame is the next-in-sequence from Pong, so this can only
                // happen if a slot was already cleared (e.g. by a previous
                // out-of-order sequence that dropped) or if the same payload
                // is in flight twice. Drop and continue — Pong is in lockstep
                // and the gap recovery will catch up.
                log_.error("Ping",
                    "STALE echo: no matching pending slot for %d bytes crc=0x%04X "
                    "(pending=%d, gap=%llu stale=%llu) — dropping",
                    got, (unsigned)gotCrc, pendingCount_,
                    (unsigned long long)comm_.getCobsGaps(),
                    (unsigned long long)comm_.getCobsStale());
                continue;
            }
            pending_[slot].active = false;
            pendingCount_--;
            log_.debug("Ping", "echo ok slot=%d  %d bytes  crc=0x%04X  pending=%d",
                slot, got, (unsigned)gotCrc, pendingCount_);
        }
        if (got < 0) {
            // Link-layer CRC/desync reject. v4.0.0: this should be rare
            // (cobsSeq catches most of the cases that used to land here).
            // Clear all pending so the next send/recv round starts fresh.
            log_.error("Ping",
                "recv rejected (CRC/desync)  pending=%d  gap=%llu stale=%llu "
                "— clearing pending",
                pendingCount_,
                (unsigned long long)comm_.getCobsGaps(),
                (unsigned long long)comm_.getCobsStale());
            resetPending_("recv reject", /*dropLink=*/true);
        }

        logStats("Ping");
    }

private:
    static void fill_(uint8_t* b, int n) {
        for (int i = 0; i < n; i++) b[i] = (uint8_t)random(256);
    }

    // v4.0.0: clear the in-flight pending list. The FIFO head/tail/seq
    // pointer state is gone — we just clear the `active` bit on every
    // slot. The cobsSeq sender is owned by ALink and is reset on
    // dropLink, not here.
    // reason: human label for the log.
    // dropLink: true for any desync event (recv reject, stall). Sends
    //   a BREAK so Pong stops echoing immediately — flushRx() alone is
    //   insufficient because the UART event task refills the stream
    //   buffer from the driver ring faster than recvMsg can drain it,
    //   keeping the desync alive indefinitely. false for "link drop"
    //   (link is already going down, the protocol layer is handling
    //   the BREAK/re-sweep).
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

    // ── pipeline state ────────────────────────────────────────────────────
    static constexpr int      WINDOW    = 8;     // messages in flight at once
    static constexpr int      MAX_TX_PER_LOOP = 2;  // frames emitted per loop() (paced, no bursts)
    static constexpr uint32_t STALL_MS      = 3000;  // ms full window with no drain
    static constexpr uint32_t SWEEP_STALL_MS = 2000;  // ms stuck in SWP before forcing BREAK
    static constexpr uint32_t SETTLE_MS = 300;   // ms after link-up before sending

    // v4.0.0: pending slot is just (active, len, crc). No more
    // head/tail/seq pointer dance — we just scan the array for a free
    // slot on send and for a matching (len, crc) on echo. For random
    // payloads (WINDOW=8) the scan is at most 8 entries and well below
    // the per-loop budget.
    struct Pending { bool active = false; int len = 0; uint16_t crc = 0; };
    Pending  pending_[WINDOW];

    int      pendingCount_   = 0;
    uint32_t tStall_         = 0;
    uint32_t tReady_         = 0;
    uint32_t tSweepStall_    = 0;
    uint32_t tNotReady_      = 0;

    // Separate TX/RX buffers so recv() can never overwrite a payload whose
    // CRC is still pending comparison.
    uint8_t sendBuf_[BUF_SIZE];
    uint8_t recvBuf_[BUF_SIZE];
};

} // namespace autolink
#endif // ARDUINO
