
#pragma once
#include "al/link/arq/ArqCache.h"
#include "al/link/Link.h"
#include "al/util/Log.h"
#include "al/util/UtilBlink.h"
#include <memory>
#include <string.h>
#include <stdlib.h>
#include <functional>

#ifdef AUTOLINK_HOST_TEST

typedef int uart_port_t;
#    include "al/hal/IHal.h"
namespace autolink {
struct EspHal;
}
#else
#    include "al/hal/EspHal.h"
#endif

namespace autolink {
#define AUTOLINK_VERSION "6.1.64"

class AutoLinkTestAccessor;

class AutoLink {
private:
#ifdef AUTOLINK_HOST_TEST

    struct RefViewDeleter {
        void operator()(IHal *) const noexcept {}
    };
    using IHalPtr = std::unique_ptr<IHal, RefViewDeleter>;
#else
    using IHalPtr = std::unique_ptr<IHal>;
#endif

    IHalPtr hal;
    std::unique_ptr<Link> link;

    // Snapshot of the role / config captured at construction
    // time. begin() logs both for the field-log pair
    // (begin: ... / begin: link layer ready) and the dtor
    // logs them again on graceful teardown. The
    // constructor's local `cfg` is consumed by the Link ctor
    // and not stored on AutoLink directly, so we keep a copy
    // here for diagnostics.
    bool isMasterNode_ = false;
    AutoLinkConfig cfg_{};

    ArqCache arqCache_{ AUTOLINK_ARQ_PIPELINE_WINDOW };
#ifdef ARDUINO
    EspBlinkHal blinkHal;
    UtilBlink blinker;
#endif

    friend class AutoLinkTestAccessor;

public:
    static constexpr int ARQ_CACHE_SLOTS_PUBLIC = ArqCache::SLOTS;

    AutoLink(const AutoLink &) = delete;
    AutoLink &operator=(const AutoLink &) = delete;

    AutoLink(uart_port_t u_num, int rx_pin, int tx_pin, bool isMasterNode,
             AutoLinkConfig cfg = AutoLinkConfig());

#ifdef ARDUINO
    ~AutoLink() {
        // Log facade destruction so the field log can pair it
        // with the matching "Init as <role>" line — without
        // this, a device that crashes between Init and dtor
        // leaves the role/state ambiguous on next boot.
        Log::log().info("AutoLink", "dtor (v%s)", AUTOLINK_VERSION);
    }
#else
    ~AutoLink() = default;
#endif

#ifdef AUTOLINK_HOST_TEST

    AutoLink(IHal &hal_in, bool isMasterNode,
             AutoLinkConfig cfg = AutoLinkConfig())
        : isMasterNode_(isMasterNode), cfg_(cfg) {
        cfg.clampToMaxBauds();
        hal = IHalPtr(&hal_in);
        link = std::make_unique<Link>(*hal, arqCache_, isMasterNode, cfg);
    }
#endif

    void begin() {
        Log::log().info(
            "AutoLink", "begin: starting v%s mode=%s maxMsg=%u isMaster=%s",
            AUTOLINK_VERSION,
            cfg_.mode == AutoLinkConfig::Mode::ASYNC ? "ASYNC" : "SYNC",
            (unsigned)cfg_.maxMsg, isMasterNode_ ? "true" : "false");
        // hal->begin() must run first: it installs the UART driver
        // and creates the sweep timer (xTimerCreate). link->begin()
        // may kick off immediately (unpaused slave) and drive the
        // HAL — hw.startTimer()/hw.setSpd() before the timer/UART
        // exist silently no-op, freezing the sweep forever.
        // Sync the HAL's mode with the facade's before
        // hal->begin() so the txBufferFloor / rxBufferFloor
        // pick up the right branch. The two were decoupled
        // (facade and HAL each held a copy of the original
        // AutoLinkConfig) and the field log showed the
        // divergence: facade reported mode=SYNC, HAL
        // reported mode=ASYNC with the 381 B ASYNC tx floor
        // under a 32x254 B SYNC window. uart_write_bytes
        // silently blocks the loop the moment more than 1.5
        // chunks are queued. Forwarding here makes the
        // facade the single source of truth, and the
        // post-begin disagreement log below catches any
        // custom HAL that doesn't honour setMode(). Pinned
        // by ModeSyncBeforeBeginTest.
        if (hal)
            hal->setMode(cfg_.mode);
#ifdef ARDUINO
        hal->begin();
#endif
        link->begin();
        if (hal && hal->getMode() != cfg_.mode) {
            Log::log().error(
                "AutoLink",
                "mode mismatch at begin: facade=%s HAL=%s — "
                "buffer floor sized for HAL's mode, not the link's. "
                "Likely a custom HAL that didn't honour setMode().",
                cfg_.mode == AutoLinkConfig::Mode::ASYNC ? "ASYNC" : "SYNC",
                hal->getMode() == AutoLinkConfig::Mode::ASYNC ? "ASYNC"
                                                              : "SYNC");
        }
        Log::log().info("AutoLink", "begin: link layer ready");
    }

    void blinkWait(int n, int onMs = 60, int offMs = 60, long delayMs = 0) {
        if (n <= 0)
            return;
#ifdef ARDUINO
        if (delayMs > 0)
            blinker.flashBlocking(n, onMs, offMs, delayMs);
        else
            blinker.start(n, onMs, offMs);
#else
        (void)onMs;
        (void)offMs;
        (void)delayMs;
#endif
    }

    int send(const uint8_t *b, int len) {
        if (len <= 0)
            return 0;
        return sendMsg(b, len) ? len : 0;
    }
    int recv(uint8_t *b, int max_len) { return link->recvMsg(b, max_len); }
    bool ready() const { return link->getState() == State::OK; }
    void dropLink() {
        Log::log().info("AutoLink", "dropLink requested by app");
        link->dropLink();
    }
    void flushRx() { link->flushRx(); }

    void setLinkPaused(bool p) {
        Log::log().info("AutoLink", "setLinkPaused %s -> %s",
                        link && link->getState() == State::OK
                            ? (p ? "OK->paused" : "OK->unpaused")
                            : (p ? "SWP->paused" : "SWP->unpaused"),
                        p ? "true" : "false");
        link->setLinkPaused(p);
    }

    void kickoff() {
        Log::log().info("AutoLink", "kickoff requested by app");
        link->kickoff();
    }

    void setMode(AutoLinkConfig::Mode m) {
        AutoLinkConfig::Mode prev =
            link ? link->mode() : AutoLinkConfig::Mode::SYNC;
        if (hal)
            hal->setMode(m);
        if (link)
            link->setMode(m);
        Log::log().info("AutoLink", "setMode %s -> %s",
                        prev == AutoLinkConfig::Mode::SYNC ? "SYNC" : "ASYNC",
                        m == AutoLinkConfig::Mode::SYNC ? "SYNC" : "ASYNC");
    }
    AutoLinkConfig::Mode mode() const {
        return link ? link->mode() : AutoLinkConfig::Mode::SYNC;
    }

    size_t maxMsg() const {
        return link ? link->maxMsg() : AUTOLINK_DEFAULT_MAX_MSG;
    }

    void setTxDelayMs(int ms) {
        if (link)
            link->setTxDelayMs(ms);
    }
    int txDelayMs() const { return link ? link->txDelayMs() : 0; }

    uint8_t lastAckSeq() const {
        return link ? link->lastAckSeq() : (uint8_t)0xFF;
    }
    uint8_t lastNakSeq() const {
        return link ? link->lastNakSeq() : (uint8_t)0xFF;
    }
    uint8_t lastRxSeq() const {
        return link ? link->lastRxSeq() : (uint8_t)0xFF;
    }
    uint16_t bytesRecvdFor(uint8_t seq) const {
        return link ? link->bytesRecvdFor(seq) : (uint16_t)0;
    }

    int arqPendingCount() const { return link ? link->arqPendingCount() : 0; }

    uint16_t bytesRecvdForMessage(uint8_t baseSeq) const {
        return link ? link->bytesRecvdForMessage(baseSeq) : (uint16_t)0;
    }
    bool isAcked(uint8_t seq) const {
        return link ? link->isAcked(seq) : false;
    }

    void getStats(Stats &s) const { link->getStats(s); }
    void resetStats() { link->resetStats(); }
    void resetErrors() { link->resetErrors(); }
    void resetDiag() { link->resetDiag(); }

    bool isHealthy() const {
#ifdef ARDUINO
        return hal->isHealthy();
#else
        return false;
#endif
    }

    int available() { return link->available(); }
    int read() { return link->read(); }
    int peek() { return link->peek(); }
    size_t write(uint8_t b) { return link->write(&b, 1); }
    size_t write(const uint8_t *buffer, size_t size) {
        return link->write(buffer, (int)size);
    }
    void flush() { return link->flush(); }
    int read(uint8_t *b, int max_len) { return link->read(b, max_len); }

    bool sendMsg(const uint8_t *b, int len) {
        if (len <= 0)
            return link->sendMsg(b, len);
        return link->sendMsg(b, len, nullptr);
    }

    bool sendMsg(const uint8_t *b, int len, uint8_t *outBaseSeq) {
        if (len <= 0) {
            if (outBaseSeq)
                *outBaseSeq = 0;
            return link->sendMsg(b, len);
        }
        return link->sendMsg(b, len, outBaseSeq);
    }
    // The reason the most recent sendMsg returned false.
    // Stamped under the link lock inside sendMsg so the
    // app can read it after a false return without
    // racing the link thread. Pinned by
    // SendMsgReasonEnumTest.
    using SendMsgReason = ::autolink::SendMsgReason;
    SendMsgReason lastSendMsgReason() const {
        return link ? link->lastSendMsgReason() : ::autolink::SendMsgReason::Ok;
    }
    int recvMsg(uint8_t *b, int max_len) { return link->recvMsg(b, max_len); }

    void err() { link->err(); }
    void clearErr() { link->clearErr(); }
    int getErrCount() const { return link->getErrCount(); }
    State getState() const { return link->getState(); }
    uint32_t getCurrentBaud() const { return link->getCurrentBaud(); }
    void getDiag(Diag &d) const { link->getDiag(d); }

    size_t getStreamBufferSize() const {
        return link->getConfig().streamBufferSize;
    }
};

} // namespace autolink
