// Wire-protocol state machine.
// Pure decisions in LinkDecision.h.
// All I/O through IHal only.
//
// Link is a coordinator. The actual state lives
// in three owned helpers:
//   LinkArq     — per-cobsSeq ACK/retx tables
//   LinkReorder — out-of-order frame hold buffer
//   LinkSweep   — baud-sweep phase machine
// Each helper takes LinkContext& for I/O
// callbacks (Link implements that interface).
// Helpers are not friends — they reach the
// link only through LinkContext.
#pragma once
#include "al/AutoLinkConfig.h"
#include "al/hal/IHal.h"
#include "al/link/arq/IArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/LinkContext.h"
#include "al/link/LinkFrameRx.h"
#include "al/link/LinkReorder.h"
#include "al/link/sweep/LinkBaudSweep.h"
#include "al/link/sweep/LinkSweep.h"
#include <stdint.h>
#include <stddef.h>

namespace autolink {
enum class State { OK, SWP, LCK };
const char *StateToStr(State s);

// CTRL command codes and MAX_CHUNK are
// declared in LinkContext.h so the
// helpers can see them without pulling
// in Link.h. Wire-protocol constants
// belong on the cross-helper I/O contract.
constexpr int CTRL_FRAME_SIZE = 5;
constexpr int CTRL_FRAME_SEQ_IDX = 2;
constexpr int CTRL_FRAME_PAYLOAD_IDX = 3;
constexpr int CTRL_FRAME_CRC_IDX = 4;

// Data seq wraps at COBS_SEQ_MAX (0xFD);
// 0xFE/0xFF are NAK/ACK wire discriminators.
constexpr int MSG_HDR = 6;

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

class Link : private UtilFrameRx::Listener, public LinkContext {
    friend class AutoLink;
    friend class LinkTestAccessor;
    IHal &hw;
    // ARQ cache lives in IArqCache / ArqCache;
    // Link holds a reference — the cache must
    // outlive the Link, and a reference makes
    // that contract unbreakable at compile
    // time. AutoLink owns the cache by value
    // and constructs the Link first, so dtor
    // order is safe.
    IArqCache &arqCache_;
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
    int rxIdx;

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
    bool kickedOff_ = false;

    // MAX_RETX / ACK_RTO_MS moved to
    // AutoLinkConfig as cfg.maxRetx /
    // cfg.syncAckTimeoutMs. Both were
    // private constants here; now they're
    // public knobs on the config struct.

    bool hasPendingRetx_ = false;
    uint8_t pendingRetxBase_ = 0;

    bool onPayload(uint8_t cobsSeq, const uint8_t *b, int n) override;
    bool onAck(uint8_t ackedCobsSeq);
    bool onNak(uint8_t missingCobsSeq);
    bool onFrameError() override;

    void sendPongAck_unlocked();
    int phase1ArmMs();

    void sendFrame_unlocked(uint8_t payload);
    void sendFrame(uint8_t payload) override { sendFrame_unlocked(payload); }
    void buildAndTxCobsFrame_unlocked(uint8_t seq, const uint8_t *b, int n);
    void sendCobsFrame_unlocked(const uint8_t *b, int n);
    int sendMsg_unlocked(const uint8_t *b, int len);
    // Shared MSG frame-build path used by
    // sendMsg() (production) and
    // test_sendMsgBegin() (SYNC host hook).
    // Builds hdr, coalesces short messages,
    // emits one or more wire frames. Caller
    // owns ARQ bookkeeping (onSent / cache
    // insert / waitForAck) so the test path
    // can skip the inline wait. Returns the
    // base seq (seq of the FIRST emitted
    // frame) and writes the seq of the LAST
    // emitted frame to *outLastSeq.
    bool buildAndSendMsg_unlocked(const uint8_t *b, int len,
                                  uint8_t *outLastSeq);
    uint8_t sendCobsFrameAcked_unlocked(const uint8_t *b, int n,
                                        uint8_t baseSeq);
    void resendCobsFrame_unlocked(uint8_t seq, const uint8_t *b, int n);
    static constexpr uint8_t NO_BASE = 0xFF;
    void sendAckFrame_unlocked(uint8_t ackedCobsSeq);
    void sendNakFrame_unlocked(uint8_t missingCobsSeq);
    void sendCtrlCobsFrame_unlocked(uint8_t type, uint8_t seq);

    void changeState_unlocked(State s);
    int bestSpd_unlocked() const;
    int readStream(uint8_t *b, int n);
    void resetSeq_unlocked();
    void lockOk_unlocked(int idx, const char *tag);
    bool ctrlFrameReady_unlocked(uint8_t cobsSeq, uint8_t payload,
                                 State curState);
    // Validate rxBuf as a CTRL frame and
    // dispatch (OK / SWP / LCK). Returns
    // true iff a link reset was triggered.
    bool processCtrlFrame_unlocked(State cur);
    bool handleSwp_unlocked(uint8_t cobsSeq, uint8_t payload);
    bool handleLck_unlocked(uint8_t cobsSeq, uint8_t payload);
    void onTimerOk_unlocked();
    void onTimerSwp_unlocked();
    void onTimerLck_unlocked();
    void reset_unlocked(bool count);
    int okTickMs() const;
    int findMsgHeaderResync_unlocked(int max_scan);

    // LinkContext impl. Each is a one-line
    // forward to IHal or a member. Helpers
    // (LinkArq / LinkReorder / LinkSweep)
    // drive the link through this surface
    // rather than reaching into privates;
    // see LinkContext.h for the rationale.
    void hwLock() override { hw.lock(); }
    void hwUnlock() override { hw.unlock(); }
    uint32_t hwNowMs() const override { return hw.nowMs(); }
    void hwSetSpd(uint32_t b) override { hw.setSpd(b); }
    void hwStartTimer(int ms) override { hw.startTimer(ms); }
    int reorderPushAppBuf(const uint8_t *b, int n) override {
        return hw.pushAppBuf(b, n);
    }
    void reorderSendAck(uint8_t seq) override { sendAckFrame_unlocked(seq); }
    uint8_t reorderExpectedSeq() const override;
    void reorderAdvanceRxSeq(uint8_t seq) override;
    void reorderCountBytes(int n) override { rxBytes += (uint32_t)n; }

    bool masterRole() const override { return isMaster; }
    int currentSpdI() const override { return spdI; }
    void setCurrentSpdI(int i) override { spdI = i; }
    int allowedBaudsCount() const override { return cfg.allowedBaudsCount; }
    uint32_t allowedBaud(int i) const override { return cfg.allowedBauds[i]; }
    int delayMs() const override { return cfg.delayMs; }

public:
    uint8_t peekTxSeq() const { return txSeq; }

    Link(IHal &hw, IArqCache &cache, bool isMasterNode,
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

    // Fire the wire-side start of the SWP handshake:
    // master sends a break and enters sweep phase 1;
    // slave arms the SWP listener (timer + baud).
    // Safe to call only after begin() and only when
    // !linkPaused_. No-op when already running. Used
    // by Ping to defer the break until the user
    // pushes the dashboard's Start button.
    void kickoff();

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