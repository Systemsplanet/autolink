
#pragma once
#include "al/link/arq/ArqCache.h"
#include "al/link/Link.h"
#include "al/util/log/Log.h"
#include "al/util/blink/UtilBlink.h"
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
#define AUTOLINK_VERSION "6.1.98"

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

    // H2: returns bool — false means
    // the link is in a fatal misconfig
    // (the ring the HAL would install
    // is too small for the COBS worst
    // case; the H1 fix in Link::begin
    // returns false on that path).
    // true means the link is alive
    // and (if not linkPaused) about
    // to start the wire handshake.
    bool begin() {
        Log::log().info(
            "AutoLink", "begin: starting v%s mode=%s maxMsg=%u isMaster=%s",
            AUTOLINK_VERSION,
            mode() == AutoLinkConfig::Mode::ASYNC ? "ASYNC" : "SYNC",
            (unsigned)cfg_.maxMsg, isMasterNode_ ? "true" : "false");
        // Link::begin() begins the HAL itself (hw.begin(cfg)) before it
        // can kick off and drive it — hw.startTimer()/hw.setSpd() against
        // an uninstalled UART or timer silently no-op and freeze the
        // sweep forever. The HAL is initialised from the link's config in
        // begin() and reads the mode from there; an explicit facade-side
        // hal->setMode(cfg_.mode) here would *revert* a mode the app
        // deliberately installed via AutoLink::setMode() before begin(),
        // because cfg_.mode is a construction-time snapshot and does not
        // track setMode(). Pinned by ModeSyncBeforeBeginTest.
        bool ok = link->begin();
        if (hal && hal->getMode() != link->mode()) {
            Log::log().error(
                "AutoLink",
                "mode mismatch at begin: link=%s HAL=%s — "
                "buffer floor sized for HAL's mode, not the link's. "
                "Likely a custom HAL that didn't honour setMode().",
                link->mode() == AutoLinkConfig::Mode::ASYNC ? "ASYNC" : "SYNC",
                hal->getMode() == AutoLinkConfig::Mode::ASYNC ? "ASYNC"
                                                              : "SYNC");
        }
        Log::log().info("AutoLink", "begin: link layer ready");
        return ok;
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
        cfg_.mode = m;
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

    // Cap maxMsg *before* begin() so the buffer floors are sized for
    // the smaller ask. The web monitor's default asks for 5120-byte
    // messages; on a 41 KB post-alloc free heap that consumes ~26 KB
    // across streamBuf + rxBuf + txBuf and leaves only heapReserveBytes
    // for LWIP / httpd, which is too little. Capping to 2048 (the
    // message size the dashboard's /stats JSON comfortably fits in)
    // takes streamBuf 10252 -> 4108 and txBuf 5588 -> 2540, freeing
    // ~9 KB without changing rxBuf (which depends on the ARQ
    // pipeline window, not maxMsg). Pinned by
    // EspHalHeapAccountingTest's field-numbers case.
    void setMaxMsg(size_t m) {
        cfg_.maxMsg = m;
        if (link)
            link->setMaxMsg(m);
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
    // Effective GBN window after Link::begin()'s installed-ring clamp —
    // may be smaller than the compile-time AUTOLINK_ARQ_PIPELINE_WINDOW.
    // Callers sizing sends against a fixed WINDOW constant will
    // over-admit against a clamped ring; use this instead. Pinned by
    // ArqWindowAccessorTest.
    int arqWindow() const { return link ? link->arqWindow() : 0; }

    // F8: the facade is now 2-arg. The
    // caller's sendMsg returns the lap
    // in SendResult.baseLap; passing the
    // same (baseSeq, baseLap) pair the
    // header went out under is the only
    // way to walk the ARQ's per-message
    // byte count without aliasing a
    // re-stamped seq across a 254-lap
    // wrap. The single-arg form is gone
    // from the production surface; the
    // 2-arg form is the only API app
    // code can call.
    uint16_t bytesRecvdForMessage(uint8_t baseSeq, uint8_t baseLap) const {
        return link ? link->bytesRecvdForMessage(baseSeq, baseLap)
                    : (uint16_t)0;
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

    void err() { link->err(FrameErrCause::CrcFail); }
    void clearErr() { link->clearErr(); }
    int getErrCount() const { return link->getErrCount(); }
    State getState() const { return link->getState(); }
    uint32_t getCurrentBaud() const { return link->getCurrentBaud(); }
    void getDiag(Diag &d) const { link->getDiag(d); }
    // AL87-06: number of chunks currently awaiting ACK. Threaded
    // through to the periodic stats heartbeat so a field capture
    // can distinguish "device wedged" from "device fine, log line
    // lost" without cross-referencing the /logs `dropped` counter.
    int pendingAcks() const { return link->pendingAcks(); }

    size_t getStreamBufferSize() const {
        return link->getConfig().streamBufferSize;
    }
};

} // namespace autolink
