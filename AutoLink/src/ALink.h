// ALink.h — AutoLink protocol core: class declaration, AutoLinkConfig struct,
// and protocol constants (state enum, command bytes, frame size limits).
//
// ALink is hardware-free; it talks to the physical layer only through the
// injected ILink interface. This makes the full protocol stack runnable in
// the host test suite without an ESP32.
//
// v4.0.0 wire format (NOT interop-compatible with v3.x):
//   * Every COBS frame carries a 1-byte cobsSeq (0..255, wraps).
//   * The receiver drops frames whose cobsSeq is not (lastRxCobsSeq_+1)%256.
//   * The FIFO length/CRC compare in UtilPing is gone — echoes are matched
//     by cobsSeq, so a wire-byte shift no longer desyncs the message layer.
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
// Length of the cobsSeq field at index 2 — repeated here so tests/logs
// don't have to magic-number it.
constexpr int CTRL_FRAME_SEQ_IDX = 2;
// Length of the payload field at index 3.
constexpr int CTRL_FRAME_PAYLOAD_IDX = 3;
// Length of the CRC8 field at index 4.
constexpr int CTRL_FRAME_CRC_IDX = 4;

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
    int emptySweeps_;           // Pong: consecutive full sweeps with 0 PINGs at any baud
    UtilBaudSweep baudSweep;    // per-baud decode scoring on the Pong side

    // v4.0.0: control-frame accumulator. Reads {0xAA, 0x55, cobsSeq, payload,
    // CRC8} from the byte stream and dispatches to the same SWP/LCK handlers
    // as v3.x. cobsSeq is consumed by the receiver to detect out-of-order /
    // duplicate command frames but is NOT used for ordering in SWP (each
    // command frame is independent of the next) — only reliable-mode data
    // frames use cobsSeq for ordering.
    uint8_t rxBuf[CTRL_FRAME_SIZE];
    int rxIdx;
    int swpRxBytes_ = 0;   // raw bytes received while in SWP (reset each baud window)

    UtilFrameRx frameRx;   // reliable-mode RX accumulator (calls back below)

    // Message reassembly state (read side).
    int      rxMsgLen;   // -1 = waiting on header
    uint16_t rxMsgCrc;
    int      lckRetries;  // REQ_CMD attempts since entering LCK (Ping only)

    // Idle watchdog / keepalive clocks (hw.nowMs() domain).
    uint32_t  lastRxMs;   // last RX activity while in OK
    uint32_t  lastTxMs;   // last TX while in OK; drives the keepalive

    // Throughput counters (app stream bytes).
    uint64_t txBytes;
    uint64_t rxBytes;

    // Total disconnect events observed since the last resetErrors().
    // Counts 1 per OK->SWP transition. Spurious onBreak() calls and
    // threshold trips that happen while already in SWP/LCK do not
    // inflate the count. Never decreases on its own -- resetStats()
    // leaves it alone, link drops leave it alone. Use this for
    // longevity testing ("how many bounces did this link survive?").
    uint64_t totalErrs;

    // Cumulative count of every frame error (bad CRC, malformed COBS, buffer
    // overflow) since the last resetErrors(). Unlike the rolling errs counter
    // -- which clearErr() and a successful frame reset to 0 -- this only ever
    // increases. This is what the web dashboard's "Errors" card shows: a
    // lifetime tally rather than a transient that's almost always 0.
    uint64_t lifetimeErrs;

    // ---- v4.0.0: cobsSeq state ----
    // Sender's next cobsSeq to use on the next frame sent in OK/reliable mode.
    // Increments by 1 per frame, wraps at 256. Persists across calls; only
    // reset on link drop / re-sweep.
    uint8_t cobsSeq_ = 0;
    // Last cobsSeq successfully received (passed CRC, passed gap check, handed
    // to onPayload). false = no frame received yet on this link.
    bool     lastRxCobsSeqSet_ = false;
    uint8_t  lastRxCobsSeq_    = 0;
    // Total gaps detected (received cobsSeq != (lastRxCobsSeq_+1)%256). For
    // diagnostics — gap events cause a frame drop and a re-sweep, not a
    // reset, so a high gap count means the wire is lossy but the protocol
    // is recovering cleanly.
    uint64_t cobsGaps_ = 0;
    // Total stale frames dropped (received cobsSeq outside the expected
    // window). Distinct from gaps: a gap is "the next expected seq didn't
    // arrive"; a stale is "a seq from an earlier session / a wraparound
    // duplicate arrived". Both are handled by dropping without invoking the
    // message parser.
    uint64_t cobsStale_ = 0;

    // UtilFrameRx::Listener (called under the lock from onRx).
    // v4.0.0: now takes the cobsSeq of the validated frame so ALink can
    // do gap/stale detection before handing the payload to the app buffer.
    bool onPayload(uint8_t cobsSeq, const uint8_t* b, int n) override;
    bool onFrameError() override;

    void sendFrame(uint8_t payload);              // assign cobsSeq, send 5-byte control frame
    void sendFrame_unlocked(uint8_t payload);    // caller holds the lock
    void sendCobsFrame(const uint8_t* b, int n); // reliable mode: COBS(cobsSeq | payload) | CRC | 0x00
    void sendCobsFrame_unlocked(const uint8_t* b, int n);
    void changeState_unlocked(State newState);
    int  bestSpd_unlocked() const;        // highest baud index that scored reliably
    int  readStream(uint8_t* b, int n);   // pull up to n bytes from the app buffer
    void resetCobsSeq_unlocked();         // cobsSeq_ = 0, lastRxCobsSeqSet_ = false
    void logCobsSeq_unlocked(const char* tag, const char* dir, uint8_t seq) const;

    // Reset all link state, retune to allowedBauds[0], Ping arms the sweep
    // timer. Idempotent. Caller must hold the lock. Counts as one disconnect
    // event for the lifetime error counter *only when the link was actually
    // up* (state == OK at entry) -- a single "thing happened" from the app's
    // perspective. Spurious onBreak() calls and threshold trips that happen
    // while already in SWP/LCK (e.g. a Pong emitting multiple BREAKs while
    // rebooting) are part of the same recovery and do not inflate the count.
    // begin() uses reset_unlocked() to do the same work without counting.
    void dropLink_unlocked();
    void reset_unlocked();      // like dropLink_unlocked but doesn't count
    int  okTickMs() const;   // watchdog/keepalive poll interval while in OK

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
    int  writeLocked(const uint8_t* b, int len); // caller holds hw.lock()
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

    // Throughput. Counters are app-stream bytes since the last reset.
    // The 3-arg form also returns the lifetime disconnect count (one
    // per OK->SWP transition; survives resetStats() and link drops;
    // only zeroed by resetErrors()).
    void getStats(uint64_t& tx, uint64_t& rx) const;
    void getStats(uint64_t& tx, uint64_t& rx, uint64_t& errors) const;
    void resetStats();     // zeros tx/rx only; leaves errors alone
    void resetErrors();    // zeros the lifetime error counter

    // The active config (with any auto-size applied). Useful for tests
    // and for the dashboard to show the actual buffer sizes.
    const AutoLinkConfig& getConfig() const { return cfg; }

    State getState() const;
    int getErrCount() const;
    uint64_t getLifetimeErrors() const; // cumulative frame errors since last resetErrors()
    int getCurrentSpdIndex() const;
    uint32_t getCurrentBaud() const; // current UART baud rate (0 if allowedBauds is empty)

    // v4.0.0 diagnostics.
    uint8_t  getCobsSeq()        const { return cobsSeq_; }
    bool     getLastRxCobsSeqSet() const { return lastRxCobsSeqSet_; }
    uint8_t  getLastRxCobsSeq()  const { return lastRxCobsSeq_; }
    uint64_t getCobsGaps()       const { return cobsGaps_; }
    uint64_t getCobsStale()      const { return cobsStale_; }

    // HAL callbacks — called by EspHal from the UART event task and FreeRTOS
    // timer. Not part of the user-facing API; do not call from application code.
    void onRx(const uint8_t* data, int len);
    void onBreak();
    void onTimer();
};

} // namespace autolink
