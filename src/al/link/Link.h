
#pragma once
#include "al/AutoLinkConfig.h"
#include "al/hal/IHal.h"
#include "al/link/arq/IArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/ISweepCtx.h"
#include "al/link/LinkWire.h"
#include "al/link/LinkMsgCodec.h"
#include "al/link/LinkHealth.h"
#include "al/link/LinkFrameRx.h"
#include "al/link/sweep/LinkBaudSweep.h"
#include "al/link/sweep/LinkDecision.h"
#include "al/link/sweep/LinkSweep.h"
#include <stdint.h>
#include <stddef.h>

namespace autolink {
enum class State { OK, SWP };
const char *StateToStr(State s);

constexpr int CTRL_FRAME_SIZE = 5;
constexpr int CTRL_FRAME_SEQ_IDX = 2;
constexpr int CTRL_FRAME_PAYLOAD_IDX = 3;
constexpr int CTRL_FRAME_CRC_IDX = 4;

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
class LinkSweep;
class LinkTestAccessor;

class Link : private UtilFrameRx::Listener,
             public ISweepCtx,
             public ILinkEvents {
    friend class AutoLink;
    friend class LinkTestAccessor;
    IHal &hw;

    IArqCache &arqCache_;
    bool isMaster;
    AutoLinkConfig cfg;

    State state;
    int errs, spdI, pingSample;
    int emptySweeps;
    UtilBaudSweep baudSweep;

    LinkSweep sweep_;
    bool wasEverOk_ = false;

    static constexpr uint8_t NO_PREFERRED_BAUD = 0xFF;
    uint8_t preferredBaud_ = NO_PREFERRED_BAUD;
    // BREAK-triggered resweep consults preferredBaud_ and
    // tries a fast P3 re-lock at that baud. The P3 timeout
    // handler falls back to enterPhase1 when this is true
    // (rather than the sweep's normal "advance to next baud"
    // behavior) so a single bad preferred baud doesn't drag
    // the link off the proven one forever. Cleared on lock
    // OR on the enterPhase1 fallback.
    bool resweepPrefPending_ = false;
    int baudRetries_ = 0;
    uint32_t errWindowStartMs_ = 0;
    int errWindowCount_ = 0;

    uint8_t rxBuf[CTRL_FRAME_SIZE];
    int rxIdx;
    // OK-state CTRL candidate split across onRx delivery chunks:
    // hold the tail until the next chunk completes or disqualifies it.
    uint8_t okCarry_[CTRL_FRAME_SIZE];
    int okCarryLen_ = 0;

    UtilFrameRx frameRx;
    LinkMsgCodec msgRx_;

    uint32_t lastRxMs, lastTxMs;

    uint32_t txRejFirstMs_ = 0, txRejLastMs_ = 0;
    void noteTxReject_unlocked() {
        uint32_t t = hw.nowMs();
        if (t == 0)
            t = 1;
        if (txRejFirstMs_ == 0)
            txRejFirstMs_ = t;
        txRejLastMs_ = t;
    }
    uint64_t txBytes, rxBytes;
    uint64_t discCount, frameErrs;

    // ASYNC-mode inter-chunk gap: cfg.asyncChunkGapMs is the
    // microsecond-resolution wait between successive chunks
    // of a single multi-chunk send. Implemented as a single
    // hw.delayMs() call; on the host it's a no-op (no UART
    // to drain), on the ESP32 it spins the task long enough
    // for the peer's uart_event_task to pull the just-emitted
    // bytes out of the RX FIFO before the next chunk
    // arrives. Returns 0 in SYNC mode (one frame in flight,
    // ACK-gated, no burst shape to pace). Pinned by
    // AsyncChunkGapTest.
    int interChunkGapMs_unlocked() const {
        if (cfg.mode != AutoLinkConfig::Mode::ASYNC)
            return 0;
        return cfg.asyncChunkGapMs < 0 ? 0 : cfg.asyncChunkGapMs;
    }

    bool onSyncAckTimeout_unlocked(bool midMessage);

    struct SyncOp {
        uint8_t seq = 0;
        const uint8_t *raw = nullptr;
        int rawLen = 0;
        int attempt = 0;
    };
    bool syncRtoStep_unlocked(SyncOp &op);
    bool syncAwaitAcked_unlocked(SyncOp &op);
    bool txQuiet_unlocked() const;
    uint32_t lockedAtMs_ = 0;
    uint32_t lastDiscMs_ = 0;
    int recentDiscs_ = 0;

    uint8_t txSeq = 0;
    bool rxSeqSet = false;
    uint8_t rxSeq = 0;
    uint64_t gaps = 0, stale = 0;
    uint64_t lostMsgs = 0;


    uint8_t lastAckSeq_ = 0xFF;

    uint8_t lastNakSeq_ = 0xFF;

    uint8_t lastRxSeq_ = 0xFF;

    LinkArq arq_;

    // gbnBase_/gbnActive_ now live in arq_ (they describe
    // the cache ring). gbnAttempts_ counts retransmit
    // rounds of the WHOLE outstanding window since gbnBase_
    // last advanced (Go-Back-N: one RTO/NAK resends
    // everything from base through txSeq-1, not just base
    // alone).
    int gbnAttempts_ = 0;
    // gbnBackoffMs_ is the inter-resend cadence the OK-state
    // timer arm applies after a whole-window retransmit
    // round, computed by decideGbnBackoff(gbnAttempts_,
    // syncAckTimeoutMs, gbnBackoffCapMs_). Resets to 0 on any
    // forward progress (cumulative ACK that advances
    // gbnBase_, or pendingCount dropping). Caps at
    // gbnBackoffCapMs_ (8*syncAckTimeoutMs by default) so a
    // permanently-stuck base still hits maxRetx within a
    // bounded wall budget. See GbnBackoffTest for the
    // host-side pin.
    uint32_t gbnBackoffMs_ = 0;
    // Snapshot of gbnBase_ at the last whole-window resend
    // round, used to detect forward progress (base advanced)
    // between rounds. 0xFF sentinel for "no resend round yet"
    // — picked because the ARQ table initializes at seq=0 and
    // a fresh link never has gbnBase_ == 0xFF.
    uint8_t gbnLastRetxBase_ = 0xFF;
    void gbnResendWindow_unlocked(uint32_t now);

    bool linkPaused_ = false;
    bool kickedOff_ = false;

    bool onPayload(uint8_t cobsSeq, const uint8_t *b, int n) override;
    bool onAck(uint8_t ackedCobsSeq, uint16_t bytesRecvd);
    bool onNak(uint8_t missingCobsSeq);
    bool onFrameError() override;

#ifdef AUTOLINK_HOST_TEST
    bool testForwardResync_ = false;
    SyncOp testOp_;
    uint8_t testOpBuf_[MAX_CHUNK];
#endif

    void sendPongAck_unlocked();
    int phase1ArmMs();

    void sendFrame_unlocked(uint8_t payload);
    void sendFrame(uint8_t payload) override { sendFrame_unlocked(payload); }
    void buildAndTxCobsFrame_unlocked(uint8_t seq, const uint8_t *b, int n);
    void sendCobsFrame_unlocked(const uint8_t *b, int n);
    int sendMsg_unlocked(const uint8_t *b, int len);

    bool buildAndSendMsg_unlocked(const uint8_t *b, int len,
                                  uint8_t *outLastSeq);
    uint8_t sendCobsFrameAcked_unlocked(const uint8_t *b, int n,
                                        uint8_t baseSeq);
    void resendCobsFrame_unlocked(uint8_t seq, const uint8_t *b, int n);
    static constexpr uint8_t NO_BASE = 0xFF;
    void sendAckFrame_unlocked(uint8_t ackedCobsSeq, uint16_t bytesRecvd = 0);
    void sendNakFrame_unlocked(uint8_t missingCobsSeq);
    void txSmallCobs_unlocked(uint8_t *u, size_t rawLen);
    void sendCtrlCobsFrame_unlocked(uint8_t type, uint8_t seq);

    void changeState_unlocked(State s);
    int bestSpd_unlocked() const;
    int readStream(uint8_t *b, int n);
    void resetSeq_unlocked();
    void lockOk_unlocked(int idx, const char *tag);
    bool ctrlFrameReady_unlocked(uint8_t cobsSeq, uint8_t payload,
                                 State curState);

    bool processCtrlFrame_unlocked(State cur);
    bool handleSwp_unlocked(uint8_t cobsSeq, uint8_t payload);

    bool applyMasterSwpAction_unlocked(SwpPhaseAction a);
    bool applyPongSwpAction_unlocked(SwpPhaseAction a);
    HealthAction applyHealth_unlocked(uint32_t now);
    bool sweepRetx_unlocked(uint32_t now);
    bool onTimerOk_unlocked();
    void onTimerSwp_unlocked();
    void reset_unlocked(bool count, bool preservePreferredBaud = false);
    int okTickMs() const;
    void retxSeq_unlocked(uint8_t seq);
    int findMsgHeaderResync_unlocked(int max_scan);

    void hwLock() override { hw.lock(); }
    void hwUnlock() override { hw.unlock(); }
    uint32_t hwNowMs() const override { return hw.nowMs(); }

    // GBN backoff cap: 8 * syncAckTimeoutMs, floored at
    // syncAckTimeoutMs. Caps the exponential doublings so a
    // permanently-stuck base still trips maxRetx within a
    // bounded wall budget — at 500 ms RTO this is 4 s of
    // stretched cadence, well below the 10 s idle watchdog
    // that would otherwise escalate the drop.
    uint32_t gbnBackoffCapMs_unlocked() const {
        uint32_t cap = (uint32_t)cfg.syncAckTimeoutMs * 8u;
        if (cap < (uint32_t)cfg.syncAckTimeoutMs)
            cap = (uint32_t)cfg.syncAckTimeoutMs;
        return cap;
    }
    void hwSetSpd(uint32_t b) override { hw.setSpd(b); }
    void hwStartTimer(int ms) override { hw.startTimer(ms); }
    uint8_t reorderExpectedSeq() const;

    bool masterRole() const override { return isMaster; }
    int currentSpdI() const override { return spdI; }
    void setCurrentSpdI(int i) override { spdI = i; }

    int allowedBaudsCount() const override {
        if (cfg.allowedBaudsCount < 0)
            return 0;
        if (cfg.allowedBaudsCount > AUTOLINK_MAX_BAUDS)
            return AUTOLINK_MAX_BAUDS;
        return cfg.allowedBaudsCount;
    }
    uint32_t allowedBaud(int i) const override {
        return cfg.allowedBaudSafe(i);
    }
    int delayMs() const override { return cfg.delayMs; }

public:
    int arqPendingCount() const { return arq_.pendingCount(); }

    uint16_t bytesRecvdFor(uint8_t cobsSeq) const {
        return arq_.bytesFor(cobsSeq);
    }

    uint16_t bytesRecvdForMessage(uint8_t baseSeq) const {
        return arq_.bytesForMessage(baseSeq);
    }

    uint8_t lastAckSeq() const {
        hw.lock();

        uint8_t s = lastAckSeq_;
        hw.unlock();
        return s;
    }

    uint8_t lastNakSeq() const {
        hw.lock();
        uint8_t s = lastNakSeq_;
        hw.unlock();
        return s;
    }

    uint8_t lastRxSeq() const {
        hw.lock();
        uint8_t s = lastRxSeq_;
        hw.unlock();
        return s;
    }

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

    void kickoff();

    void setMode(AutoLinkConfig::Mode m) { cfg.mode = m; }
    AutoLinkConfig::Mode mode() const { return cfg.mode; }

    size_t maxMsg() const { return cfg.maxMsg; }

    void setTxDelayMs(int ms) { cfg.txDelayMs = ms < 0 ? 0 : ms; }
    int txDelayMs() const { return cfg.txDelayMs; }

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
#ifdef AUTOLINK_HOST_TEST
    // Host-only hooks: SYNC sendMsg blocks on the injected clock,
    // so tests drive it in two non-blocking halves.
    bool test_sendMsgBegin(const uint8_t *b, int len);
    bool test_sendMsgStillWaiting();
    bool test_syncRtoStep();
    int test_syncAttempt() const;
#endif
};

} // namespace autolink
