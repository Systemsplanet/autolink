// Wire-protocol state machine.
// Pure decisions in LinkDecision.h.
// All I/O through IHal only.
//
// Link is a coordinator. The actual state lives
// in three owned helpers:
//   LinkArq     — per-cobsSeq ACK/retx tables
//   LinkReorder — out-of-order frame hold buffer
//   LinkSweep   — baud-sweep phase machine
// Each helper takes Link& for I/O callbacks.
#pragma once
#include "al/hal/IHal.h"
#include "al/link/IArqCache.h"
#include "al/link/LinkFrameRx.h"
#include "al/link/LinkBaudSweep.h"
#include "al/link/LinkArq.h"
#include "al/link/LinkReorder.h"
#include "al/link/LinkSweep.h"
#include <stdint.h>
#include <stddef.h>

namespace autolink {
enum class State { OK, SWP, LCK };
const char *StateToStr(State s);

constexpr uint8_t PING_CMD = 0x22;
constexpr uint8_t PONG_CMD = 0x33;
constexpr uint8_t REQ_CMD = 0x11;
constexpr uint8_t LOCK_CMD = 0x44;
constexpr int LOCK_CMD_BASE = 0x44;

constexpr int CTRL_FRAME_SIZE = 5;
constexpr int CTRL_FRAME_SEQ_IDX = 2;
constexpr int CTRL_FRAME_PAYLOAD_IDX = 3;
constexpr int CTRL_FRAME_CRC_IDX = 4;

// Data seq wraps at COBS_SEQ_MAX (0xFD);
// 0xFE/0xFF are NAK/ACK wire discriminators.
constexpr int MAX_CHUNK = 250;
constexpr int MSG_HDR = 6;

#ifndef AUTOLINK_MAX_BAUDS
#    define AUTOLINK_MAX_BAUDS 16
#endif

struct AutoLinkConfig {
    uint32_t allowedBauds[AUTOLINK_MAX_BAUDS] = { 115200, 57600, 38400, 19200,
                                                  9600 };
    int allowedBaudsCount = 5;
    int errThreshold = 100;
    int delayMs = 50;
    size_t rxBufferSize = 2048;
    size_t txBufferSize = 0;
    size_t streamBufferSize = 2048;
    size_t maxMsg = 1024;
    int ledPin = 2;
    int idleTimeoutMs = 10000;
    int pingSamplesPerBaud = 3;
    float minAcceptRate = 0.5f;
    bool baudPreference = true;
    int baudRetryLimit = 2;
    int errRateWindow = 30;

    // Out-of-order frames held up to this
    // long; expired slots → lostMsgs++.
    int reorderHoldMs = 1500;

    // SYNC: stop-and-wait. One message in
    // flight at a time. Sender blocks for
    // the receiver ACK before sending the
    // next. No ARQ cache use, no reorder
    // buffer reserve, cobsSeq gaps dropped
    // instead of held. Default — works on
    // any wire that carries COBS+CRC
    // frames. ~half the throughput of
    // ASYNC at the cost of being boring
    // and reliable.
    // ASYNC: today's pipeline — ARQ cache,
    // reorder buffer, many in flight, async
    // retransmits on NAK / ACK-timeout.
    // Faster under good conditions, falls
    // apart under sustained noise. Both
    // boards must run the same mode.
    enum class Mode : uint8_t { SYNC = 0, ASYNC = 1 };
#ifdef AUTOLINK_HOST_TEST
    // Host tests have no FreeRTOS link task
    // to deliver ACKs and no wall clock;
    // SYNC's poll-with-yield wait would
    // spin. Default the host build to ASYNC
    // so existing tests don't hang. Arduino
    // sketches default to SYNC (boring and
    // reliable out of the box).
    Mode mode = Mode::ASYNC;
#else
    Mode mode = Mode::SYNC;
#endif

    // SYNC only: how long send() blocks
    // waiting for the receiver ACK before
    // timing out and returning 0. Also
    // used as the ASYNC ARQ retransmit
    // timeout (retransmit if no ACK
    // arrives within this window).
    int syncAckTimeoutMs = 500;

    // ARQ retransmit budget per chunk.
    // After this many unacknowledged
    // retransmits, the chunk is dropped
    // and the link is reset.
    uint8_t maxRetx = 5;

    // Per-transmit delay (ms) honored by
    // Ping::loop after each send. Lets
    // the GUI throttle the wire without
    // recompiling. 0 = no delay (run as
    // fast as the link allows). Works in
    // both SYNC and ASYNC; the SYNC
    // sender is already blocked on the
    // receiver ACK, so the additional
    // txDelayMs only adds idle time when
    // the wire is fast enough to clear
    // the ACK before the delay expires.
    int txDelayMs = 0;

    bool _test_forwardResync = false;
};

struct Stats {
    uint64_t tx, rx;
    uint64_t discCount, frameErrs;
};

struct Diag {
    uint8_t txSeq;
    bool rxSeqSet;
    uint8_t rxSeq;
    uint64_t gaps, stale, lostMsgs;
    uint64_t baudRetries;
    uint8_t preferredBaud;
};

class LinkArq;
class LinkReorder;
class LinkSweep;
class LinkTestAccessor;

class Link : private UtilFrameRx::Listener {
    friend class AutoLink;
    friend class LinkArq;
    friend class LinkReorder;
    friend class LinkSweep;
    friend class LinkTestAccessor;
    IHal &hw;
    bool isMaster;
    AutoLinkConfig cfg;

    State state;
    int errs, spdI, pingSample;
    int emptySweeps;
    UtilBaudSweep baudSweep;

    LinkSweep sweep_;
    bool wasEverOk_ = false;

    int heartbeatPingsMissed_ = 0;
    uint32_t lastHeartbeatMs_ = 0;

    static constexpr uint8_t NO_PREFERRED_BAUD = 0xFF;
    uint8_t preferredBaud_ = NO_PREFERRED_BAUD;
    int baudRetries_ = 0;
    uint32_t errWindowStartMs_ = 0;
    int errWindowCount_ = 0;

    uint8_t rxBuf[CTRL_FRAME_SIZE];
    int rxIdx, swpRxBytes = 0;

    UtilFrameRx frameRx;
    int rxMsgLen = -1;
    uint16_t rxMsgCrc;
    int lckRetries;

    uint32_t lastRxMs, lastTxMs;
    uint64_t txBytes, rxBytes;
    uint64_t discCount, frameErrs;

    uint8_t txSeq = 0;
    bool rxSeqSet = false;
    uint8_t rxSeq = 0;
    uint64_t gaps = 0, stale = 0;
    uint64_t lostMsgs = 0;

    LinkArq arq_;
    LinkReorder reorder_;
    bool linkPaused_ = false;

    // MAX_RETX / ACK_RTO_MS moved to
    // AutoLinkConfig as cfg.maxRetx /
    // cfg.syncAckTimeoutMs. Both were
    // private constants here; now they're
    // public knobs on the config struct.

    // ARQ cache lives in IArqCache / ArqCache;
    // Link holds a raw IArqCache* borrowed
    // from the facade. The cache must outlive
    // the Link; AutoLink owns the cache by
    // value and constructs the Link first, so
    // dtor order is safe.

    IArqCache *arqCache_ = nullptr;
    bool retxNeeded_ = false;
    bool hasPendingRetx_ = false;
    uint8_t pendingRetxBase_ = 0;

    bool onPayload(uint8_t cobsSeq, const uint8_t *b, int n) override;
    bool onAck(uint8_t ackedCobsSeq);
    bool onNak(uint8_t missingCobsSeq);
    bool onFrameError() override;

    void sendPongAck_unlocked();
    int phase1ArmMs();
    void computeDwells_unlocked();

    void sendFrame(uint8_t payload);
    void sendFrame_unlocked(uint8_t payload);
    void sendCobsFrame(const uint8_t *b, int n);
    void sendCobsFrame_unlocked(const uint8_t *b, int n);
    int sendMsg_unlocked(const uint8_t *b, int len);
    uint8_t sendCobsFrameAcked_unlocked(const uint8_t *b, int n,
                                        uint8_t baseSeq);
    void resendCobsFrame_unlocked(uint8_t seq, const uint8_t *b, int n);
    static constexpr uint8_t NO_BASE = 0xFF;
    void sendAckFrame_unlocked(uint8_t ackedCobsSeq);
    void sendNakFrame_unlocked(uint8_t missingCobsSeq);

    void changeState_unlocked(State s);
    int bestSpd_unlocked() const;
    int readStream(uint8_t *b, int n);
    void resetSeq_unlocked();
    void lockOk_unlocked(int idx, const char *tag);
    bool ctrlFrameReady_unlocked(uint8_t cobsSeq, uint8_t payload,
                                 State curState);
    bool handleSwp_unlocked(uint8_t cobsSeq, uint8_t payload);
    bool handleLck_unlocked(uint8_t cobsSeq, uint8_t payload);
    void onTimerOk_unlocked();
    void onTimerSwp_unlocked();
    void onTimerLck_unlocked();
    void reset_unlocked(bool count);
    int okTickMs() const;
    int findMsgHeaderResync_unlocked(int max_scan);
    int popRetransmitSlot();

    // Narrow accessors for the helpers. Each
    // is a one-line forward to IHal or a
    // member; they exist so LinkArq /
    // LinkReorder / LinkSweep can do I/O
    // through Link without reaching into
    // IHal directly. Friends see them.
    void hwLock() { hw.lock(); }
    void hwUnlock() { hw.unlock(); }
    uint32_t hwNowMs() const { return hw.nowMs(); }
    void hwSetSpd(uint32_t b) { hw.setSpd(b); }
    void hwStartTimer(int ms) { hw.startTimer(ms); }
    int reorderPushAppBuf(const uint8_t *b, int n) {
        return hw.pushAppBuf(b, n);
    }
    void reorderSendAck(uint8_t seq) { sendAckFrame_unlocked(seq); }
    uint8_t reorderExpectedSeq() const;
    void reorderAdvanceRxSeq(uint8_t seq);
    void reorderCountBytes(int n) { rxBytes += (uint32_t)n; }

    bool masterRole() const { return isMaster; }
    int currentSpdI() const { return spdI; }
    void setCurrentSpdI(int i) { spdI = i; }
    int allowedBaudsCount() const { return cfg.allowedBaudsCount; }
    uint32_t allowedBaud(int i) const { return cfg.allowedBauds[i]; }
    int delayMs() const { return cfg.delayMs; }
    int preferredBaudIndex() const { return (int)preferredBaud_; }
    int baudRetryLimit() const { return cfg.baudRetryLimit; }
    int baudRetries() const { return baudRetries_; }
    void incBaudRetries() { baudRetries_++; }
    void clearBaudRetries() { baudRetries_ = 0; }
    void clearPreferredBaud() { preferredBaud_ = NO_PREFERRED_BAUD; }

public:
    uint8_t peekTxSeq() const { return txSeq; }

    // Borrowed raw pointer. The cache
    // must outlive the link; see the
    // comment block above.
    void setArqCache(IArqCache *c) { arqCache_ = c; }

    Link(IHal &hw, bool isMasterNode,
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
    void dropLink();
    void setLinkPaused(bool p) { linkPaused_ = p; }

    void setMode(AutoLinkConfig::Mode m) { cfg.mode = m; }
    AutoLinkConfig::Mode mode() const { return cfg.mode; }

    // Runtime-mutable throttle (used by the
    // web dashboard's delay-ms widget).
    void setTxDelayMs(int ms) { cfg.txDelayMs = ms < 0 ? 0 : ms; }
    int txDelayMs() const { return cfg.txDelayMs; }

    // Public wrapper for the itest. The
    // itest runs Link standalone without
    // an ArqCache and re-sends a frame
    // directly via this entry point.
    // Production callers (AutoLink
    // facade) go through the link's
    // arqCache_ path inside onTimer.
    void resendCobsFrame(uint8_t seq, const uint8_t *b, int n) {
        hw.lock();
        resendCobsFrame_unlocked(seq, b, n);
        hw.unlock();
    }
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

    void onRx(const uint8_t *data, int len);
    void onBreak();
    void onTimer();

private:
    // Host-test hooks: friends of
    // LinkTestAccessor only; production
    // sketches have no path to call these.
    void test_markAckedPending(uint8_t s) { arq_.setPending(s, true); }

    // SYNC-mode host test hooks. Split
    // sendMsg() into a non-blocking "begin"
    // (sends the frame and remembers the
    // seq) and a "still waiting" check.
    // The test pumps time between calls so
    // the wire can deliver the ACK.
    bool test_sendMsgBegin(const uint8_t *b, int len);
    bool test_sendMsgStillWaiting();

    // Direct reorder-buffer inspection
    // for LinkReorderTest.
    bool test_reorderSlotInUse(uint8_t cobsSeq) const {
        return reorder_.slotInUse(cobsSeq);
    }
    uint16_t test_reorderSlotLen(uint8_t cobsSeq) const {
        return reorder_.slotLen(cobsSeq);
    }
};

} // namespace autolink