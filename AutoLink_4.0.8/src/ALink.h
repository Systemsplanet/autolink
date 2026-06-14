// ALink.h — AutoLink protocol core: class declaration, AutoLinkConfig struct,
// and protocol constants (state enum, command bytes, frame size limits).
//
// ALink is hardware-free; it talks to the physical layer only through the
// injected ILink interface. This makes the full protocol stack runnable in
// the host test suite without an ESP32.
//
// v4.0.0 wire format (NOT interop-compatible with v3.x):
//   * Every COBS frame carries a 1-byte cobsSeq (0..255, wraps).
//   * The receiver drops frames whose cobsSeq is not (lastRxCobsSeq+1)%COBS_SEQ_WRAP.
//   * The FIFO length/CRC compare in UtilPing is gone — echoes are matched by
//     cobsSeq, so a wire-byte shift no longer desyncs the message layer.
//   * Command frames (PING, REQ, best-ack) are 5 bytes: [0xAA 0x55 cobsSeq
//     payload CRC8(first-4)].
#pragma once
#include "ILink.h"
#include "util/UtilFrameRx.h"
#include "util/UtilBaudSweep.h"
#include <vector>
#include <stdint.h>
#include <stddef.h>

namespace autolink {

enum class State { OK, SWP, LCK };

const char* StateToStr(State s);

// Command bytes must not collide with preamble bytes 0xAA or 0x55.
constexpr uint8_t PING_CMD = 0x22;
constexpr uint8_t REQ_CMD  = 0x11;

// v4.0.0: control-frame layout is {0xAA, 0x55, cobsSeq, payload, CRC8}.
constexpr int CTRL_FRAME_SIZE = 5;
// Index of the cobsSeq field inside the control frame.
constexpr int CTRL_FRAME_SEQ_IDX     = 2;
// Index of the payload field inside the control frame.
constexpr int CTRL_FRAME_PAYLOAD_IDX = 3;
// Index of the CRC8 field inside the control frame.
constexpr int CTRL_FRAME_CRC_IDX     = 4;

// cobsSeq is a single byte; the wraparound is the modulus of all gap/stale
// arithmetic. Named so the "mod 256" / "+1 % 256" comments don't have to
// magic-number it.
constexpr int COBS_SEQ_WRAP = 256;

// Heuristic for "this looks like a recently-lost frame, not a previous-session
// leftover": (cobsSeq - lastRxCobsSeq) mod COBS_SEQ_WRAP in (0, MAX_GAP_RESYNC].
// Beyond this window the frame is treated as stale (drop without re-counting).
// 3 is empirically enough: with cobsSeq=4 lost in a row, the 5th arrives at
// diff=4 which is treated as stale, but the receiver re-syncs on the very next
// expected+1 frame anyway because we don't advance lastRxCobsSeq on a gap.
constexpr int MAX_GAP_RESYNC = 3;

// Max user payload per wire frame (before COBS + CRC8). Drives the static
// scratch buffers below; keep <= 251 so a frame fits in 256 bytes.
constexpr int MAX_CHUNK = 250;

// Message-layer header: len(4, LE) + crc16(2, LE) of the payload.
constexpr int MSG_HDR = 6;

// ----------------------------------------------------------------------------
// AutoLinkConfig — all tunable parameters for an AutoLink instance.
//
// Construct one, override only the fields you care about, and pass it to the
// AutoLink constructor. The AutoLink facade auto-sizes streamBufferSize from
// maxMsg, so in most sketches only maxMsg (and optionally allowedBauds and
// ledPin) need to be touched.
// ----------------------------------------------------------------------------
struct AutoLinkConfig {
    // Auto-baud sweep order. The Ping tests the first entry first and locks
    // at the highest baud that meets the reliability threshold. The default
    // list is conservative — all five bauds work reliably with ordinary jumper
    // wires on standard ESP32 boards. High-speed bauds (≥230400) require short
    // wiring and good signal integrity; if needed, prepend them explicitly:
    //   cfg.allowedBauds = {921600, 460800, 230400, 115200, 57600, 38400, 19200, 9600};
    std::vector<uint32_t> allowedBauds = {115200, 57600, 38400, 19200, 9600};
    // Consecutive frame errors before the link is dropped. A burst that
    // overruns the peer (or mutual-sweep garbage at boot) can produce a short
    // run of COBS desyncs with no good frame between to reset the counter; a
    // low threshold turns that transient into a drop->BREAK storm. 20 absorbs
    // the transient while still catching a genuinely broken line (which never
    // produces a good frame to reset the count).
    int errThreshold = 20;
    int delayMs = 50;
    bool reliableMode = true;          // framed bytes + message API on by default
    size_t rxBufferSize = 1024;
    // TX ring buffer size in the UART driver. Must be large enough to hold a
    // complete COBS-encoded message without blocking uart_write_bytes while
    // the ALink protocol lock is held — if uart_write_bytes blocks (TX ring
    // full), the UART event task cannot drain the RX ring (it needs the lock
    // too), causing RX overflow and silent data loss. AutoLink auto-sizes
    // this from maxMsg; only override if you know what you're doing.
    size_t txBufferSize = 0;  // 0 = auto-size from maxMsg in AutoLink facade
    size_t streamBufferSize = 2048;
    // Largest message send()/recv() will accept. The AutoLink facade auto-grows
    // streamBufferSize to fit this, so you normally set only this (or nothing).
    size_t maxMsg = 1024;
    int ledPin = 2;   // status LED used by AutoLink::blinkWait(); GPIO2 = onboard blue LED
    // Idle-channel watchdog. While in OK, if no RX bytes arrive for this many
    // ms the link is dropped and re-swept. 0 disables. While in OK each side
    // also sends a 1-byte keepalive (reliable mode only) when its TX has been
    // quiet for idleTimeoutMs/3, so a healthy-but-silent link never bounces.
    int idleTimeoutMs = 5000;
    // Auto-baud reliability sweep. The Ping sends `pingSamplesPerBaud`
    // PINGs at each candidate baud; the Pong scores the success rate and
    // picks the highest baud whose rate is >= minAcceptRate. A 1-PING sweep
    // is fast but flaky: a single missed PING drops the Pong to a slower
    // baud. Multiple samples favor stability over sweep speed. Set
    // pingSamplesPerBaud=1 to get the old "first one wins" behavior.
    int pingSamplesPerBaud = 4;
    // Minimum fraction of PINGs that must decode at a baud for it to be
    // considered reliable. 0.5 = at least half; 0.8 = strict; 0 = any
    // success counts. The Pong falls back to lower bauds if the top one
    // doesn't meet the threshold.
    float minAcceptRate = 0.75f;
    // Enable the "fast ack" path: the Pong sends BEST_CMD (with its
    // current best baud index) as soon as it has enough PINGs to trust
    // the current baud, instead of waiting for the Ping to sweep every
    // baud and send REQ. With this on, a healthy top-baud link locks in
    // 4 ticks instead of N*M ticks. Set to false to use the legacy
    // "sweep every baud, then REQ" path.
    bool fastBaudLock = true;
};

// ----------------------------------------------------------------------------
// Stats — the one-and-only stats return struct. Replaces the old 2-arg / 3-arg
// getStats overloads plus getLifetimeErrors(); the field names are the source
// of truth, the two counters are never separated into "lifetime" vs
// "session" — they're just one set of monotonic counters that get zeroed
// (or not) by the reset calls.
// ----------------------------------------------------------------------------
struct Stats {
    uint64_t tx;          // app-stream bytes sent since resetStats()
    uint64_t rx;          // app-stream bytes received since resetStats()
    uint64_t discCount;   // OK->SWP transitions since resetErrors()
    uint64_t frameErrs;   // cumulative frame errors (bad CRC, malformed COBS,
                          // oversize) since resetErrors() — never decreases
};

// ----------------------------------------------------------------------------
// Diag — the one-and-only diagnostics return struct. Replaces the five
// getCobs* getters on the facade. Internal plumbing; callers that need it
// pass a reference to getDiag().
// ----------------------------------------------------------------------------
struct Diag {
    uint8_t  txSeq;        // sender's next cobsSeq to use
    bool     rxSeqSet;     // false until the first valid frame on this link
    uint8_t  rxSeq;        // last cobsSeq accepted by onPayload
    uint64_t gaps;         // total gap events (one per out-of-order arrival)
    uint64_t stale;        // total stale events (out-of-window or duplicates)
    uint64_t lostMsgs;     // total messages lost on the wire (sum of
                           // (cobsSeq - rxSeq - 1) across gap events; equals
                           // gaps only when a single seq is missing, and is
                           // larger when a burst went missing). Distinct
                           // from gaps so the dashboard can show "X
                           // disconnects, Y frames lost" without conflating
                           // a single big gap with many small ones.
};

// ----------------------------------------------------------------------------
// ALink — the AutoLink protocol core: auto-baud negotiation state machine
// (SWP/LCK/OK), reliable COBS+CRC-8 framing with cobsSeq-based
// desync-immune frame ordering, boundary-preserving CRC-16 messages, error
// thresholding, idle watchdog, and keepalive. Hardware-free: everything
// physical goes through the injected ILink, so the whole protocol runs
// natively in the host test suite.
// ----------------------------------------------------------------------------
class ALink : private UtilFrameRx::Listener {
    ILink& hw;
    bool isMaster;
    AutoLinkConfig cfg;

    State state;
    int errs;
    int spdI;
    int pingSample;             // Ping: which sample of N at the current baud
    int emptySweeps;            // Pong: consecutive full sweeps with 0 PINGs at any baud
    UtilBaudSweep baudSweep;    // per-baud decode scoring on the Pong side

    // v4.0.0: control-frame accumulator. Reads {0xAA, 0x55, cobsSeq, payload,
    // CRC8} from the byte stream and dispatches to the same SWP/LCK handlers
    // as v3.x. cobsSeq is consumed by the receiver to detect out-of-order /
    // duplicate command frames but is NOT used for ordering in SWP (each
    // command frame is independent of the next) — only reliable-mode data
    // frames use cobsSeq for ordering.
    uint8_t rxBuf[CTRL_FRAME_SIZE];
    int rxIdx;
    int swpRxBytes = 0;     // raw bytes received while in SWP (reset each baud window)

    UtilFrameRx frameRx;     // reliable-mode RX accumulator (calls back below)

    // Message reassembly state (read side).
    int      rxMsgLen;      // -1 = waiting on header
    uint16_t rxMsgCrc;
    int      lckRetries;    // REQ_CMD attempts since entering LCK (Ping only)

    // Idle watchdog / keepalive clocks (hw.nowMs() domain).
    uint32_t  lastRxMs;     // last RX activity while in OK
    uint32_t  lastTxMs;     // last TX while in OK; drives the keepalive

    // Throughput counters (app stream bytes).
    uint64_t txBytes;
    uint64_t rxBytes;

    // Total disconnect events observed since the last resetErrors().
    // Counts 1 per OK->SWP transition. Spurious onBreak() calls and
    // threshold trips that happen while already in SWP/LCK do not
    // inflate the count. Never decreases on its own -- resetStats()
    // leaves it alone, link drops leave it alone. Use this for
    // longevity testing ("how many bounces did this link survive?").
    uint64_t discCount;

    // Cumulative count of every frame error (bad CRC, malformed COBS, buffer
    // overflow) since the last resetErrors(). Unlike the rolling errs counter
    // -- which clearErr() and a successful frame reset to 0 -- this only ever
    // increases. This is what the web dashboard's "Errors" card shows: a
    // lifetime tally rather than a transient that's almost always 0.
    uint64_t frameErrs;

    // ---- v4.0.0: cobsSeq state ----
    // Sender's next cobsSeq to use on the next frame sent in OK/reliable mode.
    // Increments by 1 per frame, wraps at COBS_SEQ_WRAP. Persists across
    // calls; only reset on link drop / re-sweep.
    uint8_t txSeq = 0;
    // Last cobsSeq successfully received (passed CRC, passed gap check, handed
    // to onPayload). false = no frame received yet on this link.
    bool    rxSeqSet = false;
    uint8_t rxSeq    = 0;
    // Total gaps detected (received cobsSeq != (rxSeq+1)%COBS_SEQ_WRAP). For
    // diagnostics — gap events cause a frame drop and a re-sweep, not a
    // reset, so a high gap count means the wire is lossy but the protocol
    // is recovering cleanly.
    uint64_t gaps = 0;
    // Total stale frames dropped (received cobsSeq outside the expected
    // window). Distinct from gaps: a gap is "the next expected seq didn't
    // arrive"; a stale is "a seq from an earlier session / a wraparound
    // duplicate arrived". Both are handled by dropping without invoking the
    // message parser.
    uint64_t stale = 0;
    // Total messages lost on the wire (sum of missed cobsSeq numbers
    // across gap events; equals gaps when only a single seq is missing
    // per event, and is larger for a multi-seq burst loss). Distinct
    // from `gaps` (gap events) so the dashboard can show "X disconnects,
    // Y frames lost" without conflating a single big gap with many
    // small ones. Reset only by link drop.
    uint64_t lostMsgs = 0;

    // UtilFrameRx::Listener (called under the lock from onRx).
    // v4.0.0: now takes the cobsSeq of the validated frame so ALink can
    // do gap/stale detection before handing the payload to the app buffer.
    bool onPayload(uint8_t cobsSeq, const uint8_t* b, int n) override;
    bool onFrameError() override;

    // Control-frame TX. sendFrame() takes the lock; sendFrame_unlocked()
    // assumes the caller already holds it (mirroring sendCobsFrame / write).
    void sendFrame(uint8_t payload);
    void sendFrame_unlocked(uint8_t payload);

    // Reliable-mode data frame TX. sendCobsFrame() takes the lock;
    // sendCobsFrame_unlocked() assumes the caller holds it. With n=0 this
    // emits a 0-payload data frame, which is the keepalive shape (a
    // cobsSeq-bearing frame so the receiver's gap detection sees traffic
    // even when the app has nothing to send).
    void sendCobsFrame(const uint8_t* b, int n);
    void sendCobsFrame_unlocked(const uint8_t* b, int n);

    void changeState_unlocked(State newState);

    // Highest baud index that has at least minHitsForReliable() decodes.
    // 0 if no baud has scored yet (so a fresh caller falls back to the
    // lowest baud). Caller holds the lock.
    int  bestSpd_unlocked() const;

    // Internal RX (called with the lock held by the message parser).
    int  readStream(uint8_t* b, int n);

    // cobsSeq sender + receiver state both zeroed. Caller holds the lock.
    void resetSeq_unlocked();

    // Transition from SWP/LCK to OK at baud index `idx`. Sets the baud,
    // clears the error counter, refreshes the watchdog clocks, switches
    // state, and (optionally) arms the keepalive timer. `tag` is the
    // log line label (e.g. "fast-ack", "REQ"). The five callsites that
    // used to paste this 6-line block now all funnel through here.
    // Caller holds the lock. Returns nothing — caller is responsible for
    // any post-lock work (e.g. sendFrame_unlocked) in the original order.
    void lockOk_unlocked(int idx, const char* tag);

    // onRx helpers (all assume the lock is held on entry/exit).
    // ctrlFrameReady_unlocked consumes the assembled 5-byte control frame
    // and dispatches to the SWP / LCK handlers. handleSwp_unlocked and
    // handleLck_unlocked process one PING/REQ frame; they do NOT touch
    // the lock themselves. Returns true if the link was dropped (so the
    // caller should send a BREAK after releasing the lock).
    bool ctrlFrameReady_unlocked(uint8_t cobsSeq, uint8_t payload, State curState);
    bool handleSwp_unlocked(uint8_t cobsSeq, uint8_t payload);
    bool handleLck_unlocked(uint8_t cobsSeq, uint8_t payload);

    // onTimer helpers (assume the lock is held by the caller of onTimer;
    // the SWP-master branch in particular takes the lock once for the
    // whole branch instead of churning lock/unlock mid-call).
    void onTimerOk_unlocked();
    void onTimerSwp_unlocked();
    void onTimerLck_unlocked();

    // Reset all link state, retune to allowedBauds[0], Ping arms the sweep
    // timer. Idempotent. Caller must hold the lock. When `count` is true,
    // this counts as one disconnect event for discCount — the canonical
    // "drop the link" path. When false, it does the same work without
    // counting — used by begin() and the Pong's initial SWP entry, which
    // are the same "fresh start" but should not inflate discCount.
    void reset_unlocked(bool count);

    int  okTickMs() const;  // watchdog/keepalive poll interval while in OK

public:
    ALink(ILink& hw, bool isMasterNode, const AutoLinkConfig& config = AutoLinkConfig());

    void begin(); // Kicks off baud negotiation; must be called after HAL begin()

    void err();
    bool err_unlocked();           // caller holds the lock; returns threshold-tripped
    void clearErr();

    // Byte-stream API (Stream-compatible).
    int  available() const;
    int  peek();
    int  read();
    int  read(uint8_t* b, int max_len);
    int  write(const uint8_t* b, int len);   // returns bytes accepted while OK
    void flush();
    // Discard all bytes in the receive app buffer and reset the message
    // reassembly state. Call after a FIFO reset on the application side to
    // prevent stale echoes from desyncing the message parser on the next recv.
    // Does NOT drop the link or restart the sweep.
    void flushRx();

    // Message API (length + CRC16 framed; preserves boundaries). Requires
    // reliableMode for integrity. sendMsg returns true if fully queued.
    // recvMsg returns >0 = message length, 0 = nothing complete yet, -1 = error/drop.
    bool sendMsg(const uint8_t* b, int len);
    void dropLink();   // send BREAK and transition to SWP (restarts sweep from app code)
    int  recvMsg(uint8_t* b, int max_len);

    // Throughput + error counters, all in one struct return. Fields:
    //   tx           — app-stream bytes sent since resetStats()
    //   rx           — app-stream bytes received since resetStats()
    //   discCount    — OK->SWP transitions since resetErrors()
    //   frameErrs    — cumulative frame errors since resetErrors()
    void getStats(Stats& s) const;
    void resetStats();     // zeros tx/rx only; leaves discCount + frameErrs alone
    void resetErrors();    // zeros discCount + frameErrs

    // The active config (with any auto-size applied). Useful for tests
    // and for the dashboard to show the actual buffer sizes.
    const AutoLinkConfig& getConfig() const { return cfg; }

    State getState() const;
    int getErrCount() const;
    int getCurrentSpdIndex() const;
    uint32_t getCurrentBaud() const; // current UART baud rate (0 if allowedBauds is empty)

    // One-and-only diagnostics return — replaces the five getCobs* getters.
    void getDiag(Diag& d) const;

    // HAL callbacks — called by EspHal from the UART event task and FreeRTOS
    // timer. Not part of the user-facing API; do not call from application code.
    void onRx(const uint8_t* data, int len);
    void onBreak();
    void onTimer();
};

} // namespace autolink
