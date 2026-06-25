

#pragma once
#include "al/hal/IHal.h"
#include "al/link/LinkFrameRx.h"
#include "al/link/LinkBaudSweep.h"
#include <stdint.h>
#include <stddef.h>

namespace autolink
{
enum class State { OK, SWP, LCK };

const char *StateToStr(State s);

constexpr uint8_t PING_CMD = 0x22;
constexpr uint8_t PONG_CMD = 0x33;
constexpr uint8_t REQ_CMD = 0x11;
constexpr uint8_t LOCK_CMD = 0x44;
constexpr int LOCK_CMD_BASE = 0x44;

enum class SweepPhase : uint8_t {
    NONE = 0,
    PHASE1,
    PHASE2,
    PHASE3
};

struct SweepDwells {
    int phase1;
    int phase2[16];
    int phase2Slave[16];
    int phase3;
    int phase2Total;
};

constexpr int CTRL_FRAME_SIZE = 5;
constexpr int CTRL_FRAME_SEQ_IDX = 2;
constexpr int CTRL_FRAME_PAYLOAD_IDX = 3;
constexpr int CTRL_FRAME_CRC_IDX = 4;

// COBS seq space: 256 wraps in math, but 0xFE/0xFF are
// reserved as wire discriminators (NAK/ACK), so the
// data seq wraps at 0xFD.
constexpr int COBS_SEQ_WRAP = 256;

constexpr int MAX_CHUNK = 250;

constexpr int MSG_HDR = 6;

#ifndef AUTOLINK_MAX_BAUDS
#    define AUTOLINK_MAX_BAUDS 16
#endif

struct AutoLinkConfig {
    uint32_t allowedBauds[AUTOLINK_MAX_BAUDS] = {
        115200, 57600, 38400, 19200, 9600
    };
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


    int arqReorderHoldMs = 1500;


    bool _test_forwardResync = false;
};

struct Stats {
    uint64_t tx;
    uint64_t rx;
    uint64_t discCount;
    uint64_t frameErrs;
};

struct Diag {
    uint8_t txSeq;
    bool rxSeqSet;
    uint8_t rxSeq;
    uint64_t gaps;
    uint64_t stale;
    uint64_t lostMsgs;
    uint64_t baudRetries;
    uint8_t preferredBaud;
};

class Link : private UtilFrameRx::Listener
{
    friend class AutoLink;
    IHal &hw;
    bool isMaster;
    AutoLinkConfig cfg;

    State state;
    int errs;
    int spdI;
    int pingSample;
    int emptySweeps;
    UtilBaudSweep baudSweep;

    SweepPhase sweepPhase_ = SweepPhase::NONE;
    int phase3Baud_ = -1;
    int phase3Acks_ = 0;


    bool wasEverOk_ = false;
    SweepDwells dwells_;

    int heartbeatPingsMissed_ = 0;
    uint32_t lastHeartbeatMs_ = 0;


    static constexpr uint8_t NO_PREFERRED_BAUD = 0xFF;
    uint8_t preferredBaud_ = NO_PREFERRED_BAUD;
    int baudRetries_ = 0;
    uint32_t errWindowStartMs_ = 0;
    int errWindowCount_ = 0;

    uint8_t rxBuf[CTRL_FRAME_SIZE];
    int rxIdx;
    int swpRxBytes = 0;

    UtilFrameRx frameRx;

    int rxMsgLen = -1;
    uint16_t rxMsgCrc;
    int lckRetries;

    uint32_t lastRxMs;
    uint32_t lastTxMs;

    uint64_t txBytes;
    uint64_t rxBytes;
    uint64_t discCount;
    uint64_t frameErrs;

    uint8_t txSeq = 0;
    bool rxSeqSet = false;
    uint8_t rxSeq = 0;
    uint64_t gaps = 0;
    uint64_t stale = 0;
    uint64_t lostMsgs = 0;

    bool ackedPending_[256] = {};
    uint8_t retxCount_[256] = {};
    uint32_t sentAtMs_[256] = {};
    uint8_t baseSeq_[256] = {};
    bool retxNeeded_ = false;


    bool hasPendingRetx_ = false;
    uint8_t pendingRetxBase_ = 0;


    struct ReorderSlot {
        uint8_t *buf = nullptr;
        uint16_t len = 0;
        uint32_t heldAtMs = 0;
        bool in_use = false;
    };
    ReorderSlot reorder_[256] = {};

    bool linkPaused_ = false;

    // Hard cap on per-chunk retransmit attempts; past
    // this the message is counted as lost and the link
    // stays up.
    static constexpr uint8_t MAX_RETX = 5;


    // ARQ retransmit timeout for an unacked chunk.
    static constexpr uint32_t ACK_RTO_MS = 500;

    using ArqAckCallback = bool (*)(uint8_t ackedSeq,
                                    void *ctx);
    using ArqRetxCallback = bool (*)(uint8_t retxSeq,
                                     void *ctx);
    using ArqCacheHasRoomCallback =
        bool (*)(void *ctx);
    using ArqCacheInsertCallback = void (*)(
        uint8_t baseSeq, const uint8_t *payload,
        int payloadLen, uint8_t chunkCount, void *ctx);
    using ArqCacheClearAllCallback =
        void (*)(void *ctx);
    using LinkResetCallback = void (*)(void *ctx);

    ArqAckCallback arqAckCallback_ = nullptr;
    ArqRetxCallback arqRetxCallback_ = nullptr;
    ArqCacheHasRoomCallback arqCacheHasRoomCallback_ =
        nullptr;
    ArqCacheInsertCallback arqCacheInsertCallback_ =
        nullptr;
    ArqCacheClearAllCallback
        arqCacheClearAllCallback_ = nullptr;
    LinkResetCallback linkResetCallback_ = nullptr;
    void *arqCtx_ = nullptr;

    bool onPayload(uint8_t cobsSeq, const uint8_t *b,
                   int n) override;
    bool onAck(uint8_t ackedCobsSeq);
    bool onNak(uint8_t missingCobsSeq);
    bool onFrameError() override;

    void sendPongAck_unlocked();
    void enterPhase1_unlocked();
    void enterPhase2_unlocked();
    void enterPhase3_unlocked(int chosenBaud);
    void enterResweep_unlocked();
    void computeDwells_unlocked();


    int phase1ArmMs();

    void sendFrame(uint8_t payload);
    void sendFrame_unlocked(uint8_t payload);
    void sendCobsFrame(const uint8_t *b, int n);
    void sendCobsFrame_unlocked(const uint8_t *b,
                                int n);
    int sendMsg_unlocked(const uint8_t *b, int len);

    uint8_t sendCobsFrameAcked_unlocked(
        const uint8_t *b, int n, uint8_t baseSeq);
    void resendCobsFrame_unlocked(uint8_t seq,
                                  const uint8_t *b,
                                  int n);
    static constexpr uint8_t NO_BASE = 0xFF;
    void sendAckFrame_unlocked(uint8_t ackedCobsSeq);
    void sendNakFrame_unlocked(uint8_t missingCobsSeq);

    void changeState_unlocked(State newState);

    int bestSpd_unlocked() const;
    int readStream(uint8_t *b, int n);
    void resetSeq_unlocked();

    void lockOk_unlocked(int idx, const char *tag);

    bool ctrlFrameReady_unlocked(uint8_t cobsSeq,
                                 uint8_t payload,
                                 State curState);
    bool handleSwp_unlocked(uint8_t cobsSeq,
                            uint8_t payload);
    bool handleLck_unlocked(uint8_t cobsSeq,
                            uint8_t payload);

    void onTimerOk_unlocked();
    void onTimerSwp_unlocked();
    void onTimerLck_unlocked();


    void reset_unlocked(bool count);

    int okTickMs() const;

    bool retxNeeded() const { return retxNeeded_; }
    void setRetxPending(bool v = true)
    {
        retxNeeded_ = v;
    }
    int popRetransmitSlot();

    int findMsgHeaderResync_unlocked(int max_scan);

    void reorderClear_unlocked();
    void reorderDropExpired_unlocked(uint32_t nowMs);


    int
    reorderFlushContiguous_unlocked(uint32_t nowMs);

public:
    uint8_t peekTxSeq() const { return txSeq; }

    void setArqHooks(ArqAckCallback ack,
                     ArqRetxCallback retx, void *ctx)
    {
        arqAckCallback_ = ack;
        arqRetxCallback_ = retx;
        arqCtx_ = ctx;
    }
    void setLinkResetHook(LinkResetCallback cb,
                          void *ctx)
    {
        linkResetCallback_ = cb;
        arqCtx_ = ctx;
    }
    void setArqCacheHooks(
        ArqAckCallback ack, ArqRetxCallback retx,
        ArqCacheHasRoomCallback hasRoom,
        ArqCacheInsertCallback insert,
        ArqCacheClearAllCallback clearAll, void *ctx)
    {
        arqAckCallback_ = ack;
        arqRetxCallback_ = retx;
        arqCacheHasRoomCallback_ = hasRoom;
        arqCacheInsertCallback_ = insert;
        arqCacheClearAllCallback_ = clearAll;
        arqCtx_ = ctx;
    }

    Link(IHal &hw, bool isMasterNode,
         const AutoLinkConfig &config =
             AutoLinkConfig());
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

    bool sendMsg(const uint8_t *b, int len,
                 uint8_t *outBaseSeq = nullptr);
    void dropLink();

    void setLinkPaused(bool p) { linkPaused_ = p; }
    int recvMsg(uint8_t *b, int max_len);

    int pendingAcks() const;
    bool isAcked(uint8_t cobsSeq) const;

    void getStats(Stats &s) const;
    void resetStats();
    void resetErrors();
    void resetDiag();

    const AutoLinkConfig &getConfig() const
    {
        return cfg;
    }

    State getState() const;
    int getErrCount() const;
    int getCurrentSpdIndex() const;
    uint32_t getCurrentBaud() const;

    void getDiag(Diag &d) const;

    void onRx(const uint8_t *data, int len);
    void onBreak();
    void onTimer();

    void test_markAckedPending(uint8_t s)
    {
        ackedPending_[s] = true;
    }
};

} // namespace autolink