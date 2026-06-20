// ALink.h — AutoLink protocol core: state machine, AutoLinkConfig,
// frame constants, and the Stats / Diag return structs.
//
// Hardware-free: talks to the physical layer only through ILink, which
// is what makes the full protocol stack runnable in host tests.
//
// Wire format (not interop with v3.x):
//   * Every COBS frame carries a 1-byte cobsSeq (0..255, wraps).
//   * Receiver drops frames whose cobsSeq is not (lastRx+1) mod 256.
//   * Control frames: [0xAA 0x55 cobsSeq payload CRC8(first-4)] = 5 bytes.
#pragma once
#include "al/hal/ILink.h"
#include "al/protocol/UtilFrameRx.h"
#include "al/protocol/UtilBaudSweep.h"
#include <stdint.h>
#include <stddef.h>

namespace autolink {

enum class State { OK, SWP, LCK };

const char* StateToStr(State s);

// Command bytes must not collide with preamble 0xAA or 0x55.
constexpr uint8_t PING_CMD = 0x22;
constexpr uint8_t REQ_CMD  = 0x11;

// Control-frame layout {0xAA, 0x55, cobsSeq, payload, CRC8}.
constexpr int CTRL_FRAME_SIZE         = 5;
constexpr int CTRL_FRAME_SEQ_IDX     = 2;
constexpr int CTRL_FRAME_PAYLOAD_IDX = 3;
constexpr int CTRL_FRAME_CRC_IDX     = 4;

constexpr int COBS_SEQ_WRAP = 256;

// v5.1.14 (audit #9): removed vestigial MAX_GAP_RESYNC constant.
// It was a forward-jump cap on gap detection that's no longer
// used; the resync scan now runs to the full available buffer.

// Max user payload per wire frame. Keep <= 251 so a frame fits in 256 B.
constexpr int MAX_CHUNK = 250;

// Message header: len(4, LE) + crc16(2, LE) of the payload.
constexpr int MSG_HDR = 6;

#ifndef AUTOLINK_MAX_BAUDS
#define AUTOLINK_MAX_BAUDS 16
#endif

struct AutoLinkConfig {
    // Auto-baud sweep order. Ping tests entries in order; locks at the
    // highest baud meeting minAcceptRate. High bauds (>=230400) need
    // short wiring. Default list covers the common case.
    uint32_t allowedBauds[AUTOLINK_MAX_BAUDS] = {115200, 57600, 38400, 19200, 9600};
    int      allowedBaudsCount = 5;
    // Consecutive frame errors before dropping the link. 20 absorbs
    // boot-time garbage without dropping on a single burst.
    int errThreshold = 20;
    int delayMs = 50;
    bool reliableMode = true;
    size_t rxBufferSize = 1024;
    // TX ring size in the UART driver. 0 = auto-size from maxMsg.
    size_t txBufferSize = 0;
    size_t streamBufferSize = 2048;
    // Largest message send/recv will accept. AutoLink facade auto-grows
    // streamBufferSize to fit this.
    size_t maxMsg = 1024;
    int ledPin = 2;  // GPIO2 = onboard blue LED on most ESP32 dev boards
    // Idle-channel watchdog: drop + re-sweep if no RX bytes for this long.
    // 0 disables. Each side also sends a 1-byte keepalive (reliable mode)
    // when its TX has been quiet for idleTimeoutMs/3.
    int idleTimeoutMs = 5000;
    // Per-baud PING samples for the reliability sweep. 1 = old
    // "first one wins" (fast but flaky). >1 favors stability.
    int pingSamplesPerBaud = 4;
    // Pong picks a baud only if at least this fraction of PINGs decoded.
    float minAcceptRate = 0.75f;
    // Fast ack: Pong sends BEST_CMD as soon as it has enough PINGs to
    // trust the current baud, instead of waiting for REQ. Locks in 4
    // ticks instead of N*M. Set false for legacy "sweep every baud".
    bool fastBaudLock = true;
};

// Throughput + lifetime counters. discCount and frameErrs are reset
// only by resetErrors(); tx/rx are reset by resetStats(). Dashboard
// shows the lifetime view.
struct Stats {
    uint64_t tx;
    uint64_t rx;
    uint64_t discCount;   // OK->SWP transitions
    uint64_t frameErrs;   // bad CRC, malformed COBS, oversize
};

// cobsSeq diagnostics. gaps = forward-jump events; stale = duplicate /
// backwards-jump frames; lostMsgs = sum of missed cobsSeq numbers
// across gaps (>= gaps, larger when a multi-seq burst went missing).
struct Diag {
    uint8_t  txSeq;
    bool     rxSeqSet;
    uint8_t  rxSeq;
    uint64_t gaps;
    uint64_t stale;
    uint64_t lostMsgs;
};

// ALink — protocol core. Owns the SWP/LCK/OK state machine, reliable
// COBS+CRC-8 framing with cobsSeq ordering, CRC-16 messages, error
// thresholding, idle watchdog, keepalive.
class ALink : private UtilFrameRx::Listener {
    ILink& hw;
    bool isMaster;
    AutoLinkConfig cfg;

    State state;
    int errs;
    int spdI;
    int pingSample;            // Ping: which sample of N at the current baud
    int emptySweeps;           // Pong: consecutive full sweeps with 0 PINGs
    UtilBaudSweep baudSweep;   // per-baud decode scoring on the Pong side

    // Control-frame accumulator. Reads 5-byte [0xAA 0x55 cobsSeq payload
    // CRC8] from the byte stream and dispatches to SWP/LCK handlers.
    uint8_t rxBuf[CTRL_FRAME_SIZE];
    int rxIdx;
    int swpRxBytes = 0;        // raw bytes received while in SWP (reset per baud)

    UtilFrameRx frameRx;        // reliable-mode RX accumulator

    int      rxMsgLen = -1;     // -1 = waiting on header
    uint16_t rxMsgCrc;
    int      lckRetries;        // REQ_CMD attempts since entering LCK (Ping only)

    uint32_t lastRxMs;          // watchdog: last RX activity while OK
    uint32_t lastTxMs;          // keepalive: last TX while OK

    uint64_t txBytes;
    uint64_t rxBytes;
    uint64_t discCount;         // OK->SWP transitions since resetErrors()
    uint64_t frameErrs;         // cumulative frame errors since resetErrors()

    // cobsSeq state — sender's next to use; receiver's last accepted.
    uint8_t  txSeq = 0;
    bool     rxSeqSet = false;
    uint8_t  rxSeq = 0;
    uint64_t gaps = 0;          // forward-jump events
    uint64_t stale = 0;         // duplicate / backwards-jump frames
    uint64_t lostMsgs = 0;      // sum of missed cobsSeq numbers across gaps

    // ARQ (v5) per-cobsSeq pending state. sendAcked_unlocked fills a
    // slot; onAck clears it; onTimerOk_unlocked retransmits slots whose
    // RTO has expired. A cobsSeq is "acked" iff ackedPending_[seq]
    // is false AND the slot is still in flight (i.e. we sent it and
    // haven't heard back yet). retxCount_[seq] tracks retransmit
    // attempts; once it exceeds MAX_RETX the link is dropped.
    //
    // Index by cobsSeq (0..255). Memory is 256 bytes for the bool
    // array + 256 bytes for the retx counter + a few sent-time
    // fields = ~1 KB. Negligible vs the per-message pending arrays
    // in UtilPing/UtilPong.
    bool    ackedPending_[256] = {};   // true = sent, waiting for ACK
    uint8_t retxCount_[256]    = {};   // retransmit attempts (0..MAX_RETX)
    uint32_t sentAtMs_[256]    = {};   // millis() at last send/retx
    // For multi-chunk messages, every chunk's cobsSeq (base+1, base+2, ...)
    // maps back to the base cobsSeq so the retransmit hook can re-send
    // the whole message from a single cache entry. Set by
    // sendCobsFrameAcked_unlocked when the chunk is the first of a
    // message (and any subsequent chunks inherit it). Cleared on ACK.
    uint8_t  baseSeq_[256]     = {};
    bool    retxNeeded_        = false;// set by onTimerOk_unlocked, cleared by pop
    // v5.1.19: deferred-retransmit slot. onTimerOk_unlocked() sets
    // hasPendingRetx_=true + pendingRetxBase_=base when an ACK
    // timeout is detected. ALink::onTimer() then dispatches the
    // facade callback AFTER releasing hw.lock(), so the callback's
    // resend path (AutoLink::retx_resend -> link->sendMsg() ->
    // hw.lock()) doesn't re-enter the same non-recursive mutex.
    // Without this deferral the user's Pong node crashed on boot
    // (2026-06-19): the first OK-state timer tick after the first
    // sent message triggered a retx, the retx callback re-locked,
    // and the second lock attempt deadlocked the task.
    bool    hasPendingRetx_    = false;
    uint8_t pendingRetxBase_   = 0;

    // v5.1.31: facade-driven link pause. When true, onTimerOk_unlocked
    // skips BOTH the idle-drop check AND the keepalive emission —
    // Ping/Pong is in "operator inspecting the link" mode and the
    // user has not asked the device to send anything yet. The peer
    // sees no traffic from this side; the peer's idle watchdog is
    // independent and will still bite on the peer side after
    // idleTimeoutMs of silence from us. (If both sides are paused,
    // both watchdogs are suppressed and the link stays up indefinitely
    // until manual resume / dropLink().)
    bool    linkPaused_        = false;
    static constexpr uint8_t MAX_RETX = 5;     // give up after this many
    static constexpr uint32_t ACK_RTO_MS = 100; // retransmit timeout (one OK tick)

    // v5 ARQ hook types — declared before the members that use them.
    using ArqAckCallback  = bool (*)(uint8_t ackedSeq, void* ctx);
    using ArqRetxCallback = bool (*)(uint8_t retxSeq,  void* ctx);

    // Facade hooks. Null by default (no facade = no retransmit = old
    // best-effort behavior, though in v5 we REQUIRE the facade for
    // reliability). Set via setArqHooks().
    ArqAckCallback  arqAckCallback_  = nullptr;
    ArqRetxCallback arqRetxCallback_ = nullptr;
    void*           arqCtx_          = nullptr;

    bool onPayload(uint8_t cobsSeq, const uint8_t* b, int n) override;
    // v5 ARQ: onAck is non-virtual; the facade registers a callback
    // via setArqHooks() that the protocol layer calls after clearing
    // its own state. Function-pointer style matches the existing
    // setFillModeHook() pattern (no inheritance, no vtable bloat).
    bool onAck(uint8_t ackedCobsSeq);
    bool onFrameError() override;

    // sendFrame takes the lock; sendFrame_unlocked assumes the caller holds it.
    void sendFrame(uint8_t payload);
    void sendFrame_unlocked(uint8_t payload);
    // sendCobsFrame / sendCobsFrame_unlocked: reliable-mode data frames.
    // n=0 emits a keepalive (cobsSeq-bearing zero-payload frame).
    void sendCobsFrame(const uint8_t* b, int n);
    void sendCobsFrame_unlocked(const uint8_t* b, int n);
    // v5.1.19: sendMsg_unlocked — multi-chunk write that assumes the
    // caller already holds hw.lock(). Used by the ARQ retransmit
    // path (AutoLink::retx_resend), which runs inside the protocol
    // timer callback while hw.lock() is already held. Calling the
    // locked sendMsg() from there re-enters hw.lock() on a
    // non-recursive mutex → deadlock / crash. Disclosed as the root
    // cause of the v5.1.17 boot-crash reported by the user (2026-06-19).
    int  sendMsg_unlocked(const uint8_t* b, int len);

    // v5 ARQ: arms the retransmit timer for the cobsSeq used.
    // Returns the cobsSeq assigned so the caller can sequence
    // multi-chunk messages. Pass baseSeq=NO_BASE for a single-frame
    // message; pass the message's base seq for subsequent chunks so
    // the retransmit hook can re-send the whole message from one
    // cache entry.
    uint8_t sendCobsFrameAcked_unlocked(const uint8_t* b, int n, uint8_t baseSeq);
    // Sentinel: "this chunk is the base of its message". Used by
    // sendCobsFrameAcked_unlocked when called for the first chunk
    // of a message (or for a 1-chunk message where chunk == base).
    static constexpr uint8_t NO_BASE = 0xFF;
    // Send an ACK frame for the given cobsSeq.
    void sendAckFrame_unlocked(uint8_t ackedCobsSeq);

    void changeState_unlocked(State newState);

    int  bestSpd_unlocked() const;  // highest baud idx with minHitsForReliable()
    int  readStream(uint8_t* b, int n);
    void resetSeq_unlocked();

    // Transition to OK at baud `idx`. Sets baud, clears errs, refreshes
    // watchdog clocks, switches state. Caller holds the lock.
    void lockOk_unlocked(int idx, const char* tag);

    // Returns true if the link was dropped (caller should send BREAK
    // after releasing the lock). All assume the lock is held.
    bool ctrlFrameReady_unlocked(uint8_t cobsSeq, uint8_t payload, State curState);
    bool handleSwp_unlocked(uint8_t cobsSeq, uint8_t payload);
    bool handleLck_unlocked(uint8_t cobsSeq, uint8_t payload);

    void onTimerOk_unlocked();
    void onTimerSwp_unlocked();
    void onTimerLck_unlocked();

    // Retune to allowedBauds[0], Ping arms the sweep timer. Idempotent.
    // count=true for app-triggered drops (bumps discCount); count=false
    // for begin() / Pong's initial SWP entry (fresh start, don't bump).
    void reset_unlocked(bool count);

    int  okTickMs() const;

    // v5 ARQ facade hook: called from onTimerOk_unlocked when a
    // slot's RTO has expired. Default impl is a no-op (the protocol
    // layer alone can't retransmit because it doesn't cache
    // Inspect/control the ARQ state. setRetxPending() tells the
    // protocol layer "there's still work to do, fire me again" so
    // the facade can pop multiple expired slots in a single tick.
    bool    retxNeeded() const   { return retxNeeded_; }
    void    setRetxPending(bool v = true) { retxNeeded_ = v; }
    // Pop one cobsSeq whose ACK has expired. Returns -1 if none.
    // Caller must hold hw.lock().
    int     popRetransmitSlot();

    // Scan the app buffer forward for the next valid MSG_HDR boundary.
    // Returns bytes to drop, or -1 if nothing found within `max_scan`.
    //
    // v5.1.14 (audit #3): the `_unlocked` suffix indicates this
    // method must only be called with hw.lock() held (the caller is
    // the RX task, which holds the link lock around onRx / resync).
    // Kept in the private section (default for `class`) so the
    // suffix is enforced — a public declaration would let external
    // code call it without the lock.
    int  findMsgHeaderResync_unlocked(int max_scan);

    // v5 ARQ: peek the next cobsSeq that sendCobsFrameAcked would
    // assign. Returns the value of txSeq BEFORE the next bump. The
    // facade uses this to key its payload cache under the same seq
    // the protocol layer is about to stamp on the wire.

public:

    // v5 ARQ: peek the next cobsSeq that sendCobsFrameAcked would
    // assign. Returns the value of txSeq BEFORE the next bump. The
    // facade uses this to key its payload cache under the same seq
    // the protocol layer is about to stamp on the wire.
    uint8_t peekTxSeq() const { return txSeq; }

    // v5 ARQ: register facade hooks. arqAckCallback_(seq, ctx) is
    // called after clearing the protocol-layer ARQ state for the
    // acked cobsSeq (facade frees its cache slot). arqRetxCallback_
    // (seq, ctx) is called when a slot's RTO has expired (facade
    // re-sends from its cache). Either callback returning true
    // asks the protocol layer to drop the link.
    void setArqHooks(ArqAckCallback ack, ArqRetxCallback retx, void* ctx) {
        arqAckCallback_  = ack;
        arqRetxCallback_ = retx;
        arqCtx_          = ctx;
    }

    ALink(ILink& hw, bool isMasterNode, const AutoLinkConfig& config = AutoLinkConfig());

    void begin();   // call after HAL begin()

    void err();
    bool err_unlocked();   // returns true if threshold tripped
    void clearErr();

    // Stream-compatible byte API.
    int  available() const;
    int  peek();
    int  read();
    int  read(uint8_t* b, int max_len);
    int  write(const uint8_t* b, int len);  // bytes accepted while OK
    void flush();
    // Discard RX app buffer + reset message reassembly. Does NOT drop
    // the link — call after an app-side FIFO reset to prevent stale
    // echoes from desyncing the message parser.
    void flushRx();

    // Message API. sendMsg returns true if fully queued. recvMsg:
    // >0 = message length, 0 = nothing complete, -1 = error/drop.
    //
    // v5 ARQ: sendMsg is now the reliable path. Each cobsSeq-bearing
    // data frame is retransmitted up to MAX_RETX times if no ACK comes
    // back within ACK_RTO_MS. recvMsg returns messages in the order
    // they were sent (cobsSeq ordering is now strict because every
    // gap triggers a retransmit, not a drop).
    bool sendMsg(const uint8_t* b, int len);
    void dropLink();   // send BREAK, transition to SWP

    // v5.1.31: when the facade (Ping side, paused_=true) wants to
    // inspect the link without tearing it down, it can suppress the
    // idle watchdog. While paused, onTimerOk_unlocked() will not
    // drop the link on idle and will not emit keepalive frames.
    // Pong will still see its own keepalives — Pong's idle watchdog
    // is independent. If both sides are paused, no link activity is
    // expected and the link stays up indefinitely (until manual
    // resume or an explicit dropLink()). Default false.
    void setLinkPaused(bool p) { linkPaused_ = p; }
    int  recvMsg(uint8_t* b, int max_len);

    // ARQ inspection (for tests + diagnostics). pendingAcks() returns
    // the count of cobsSeq numbers currently waiting for ACK.
    int  pendingAcks() const;
    bool isAcked(uint8_t cobsSeq) const;

    void getStats(Stats& s) const;
    void resetStats();    // zeros tx/rx only
    void resetErrors();   // zeros discCount + frameErrs
    void resetDiag();     // zeros gaps, stale, lostMsgs

    const AutoLinkConfig& getConfig() const { return cfg; }

    State     getState() const;
    int       getErrCount() const;
    int       getCurrentSpdIndex() const;
    uint32_t  getCurrentBaud() const;

    void getDiag(Diag& d) const;

    // HAL callbacks — invoked from the UART event task and timer. Not
    // part of the user-facing API; do not call from application code.
    void onRx(const uint8_t* data, int len);
    void onBreak();
    void onTimer();
};

} // namespace autolink
