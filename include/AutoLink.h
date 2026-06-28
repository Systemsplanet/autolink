// Public facade: stream-style API over
// Link. ARQ payload cache lives in
// al/link/arq/ArqCache.{h,cpp} behind IArqCache.
// AutoLink owns an ArqCache by value and
// hands a reference to Link at construction;
// reference semantics make the lifetime
// contract unbreakable at compile time.
// Host-only EspHal / EspBlinkHal stubs live
// in test/common/EspHalStub.h. Public ctor
// body lives in src/AutoLink.cpp so the
// public HAL boundary doesn't leak into the
// host build.
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
#define AUTOLINK_VERSION "6.0.2"

class AutoLinkTestAccessor;

class AutoLink {
private:
#ifdef AUTOLINK_HOST_TEST
    // The host-test ctor takes an IHal by
    // reference; the caller still owns the
    // object. RefViewDeleter makes the
    // unique_ptr non-owning so the test's
    // MockHal isn't double-freed when the
    // facade destructs. The production ctor
    // constructs an EspHal by value and
    // transfers ownership — the deleter
    // fires only on that path's release().
    struct RefViewDeleter {
        void operator()(IHal *) const noexcept {}
    };
    using IHalPtr = std::unique_ptr<IHal, RefViewDeleter>;
#else
    using IHalPtr = std::unique_ptr<IHal>;
#endif

    IHalPtr hal;
    std::unique_ptr<Link> link;
    // ARQ cache sized for Ping's pipeline
    // window. Ping owns the window (it's
    // the flow controller); the cache
    // validates its own pool holds a full
    // window plus retx headroom. Default
    // ctor of ArqCache would use the
    // compile-time fallback; passing
    // AUTOLINK_ARQ_PIPELINE_WINDOW here
    // means widening the pipeline on Ping
    // flows through to the cache at the
    // same call site.
    ArqCache arqCache_{ AUTOLINK_ARQ_PIPELINE_WINDOW };
#ifdef ARDUINO
    EspBlinkHal blinkHal;
    UtilBlink blinker;
#endif

    // Host-test hooks: friends of
    // AutoLinkTestAccessor only; production
    // sketches have no path to call these.
    friend class AutoLinkTestAccessor;
    int arqCacheSizeForTest() const { return arqCache_.size(); }
    Link *linkForTest() { return link.get(); }
    const Link *linkForTest() const { return link.get(); }
    ArqCache *arqCacheForTest() { return &arqCache_; }
    const ArqCache *arqCacheForTest() const { return &arqCache_; }
    void test_arqCache_put(uint8_t seq, const uint8_t *b, int len, uint8_t) {
        arqCache_.testPut(seq, b, len);
    }
    bool test_arqCache_hasRoom() { return arqCache_.hasRoom(); }
    static constexpr int test_arqPoolSize() { return ArqCache::POOL_SIZE; }
    void test_arqCache_freeBySeq(uint8_t s) { arqCache_.freeBySeq(s); }
    bool test_arqCache_retx(uint8_t seq) {
        const uint8_t *buf = nullptr;
        int len = 0;
        bool hit = arqCache_.testRetx(seq, &buf, &len);
        (void)buf;
        (void)len;
        return hit;
    }
    int test_arqCache_findBySeq(uint8_t s) {
        return arqCache_.slotInUse(s) ? (int)s : -1;
    }
    void test_markAckedPending(uint8_t s) {
        if (link)
            link->test_markAckedPending(s);
    }

    // SYNC-mode host test hooks. The
    // Arduino build blocks inside send()
    // while the FreeRTOS link task
    // concurrently delivers the ACK. The
    // host build has no concurrent link
    // task, so the test must drive the
    // wire step-by-step. Splitting send()
    // into begin + still-waiting lets the
    // test pump time between halves.
    bool test_sendMsgBeginForTest(const uint8_t *b, int len) {
        return link && link->test_sendMsgBegin(b, len);
    }
    bool test_sendMsgStillWaitingForTest() {
        return link && link->test_sendMsgStillWaiting();
    }
    int syncAckTimeoutMsForTest() const {
        return link ? link->cfg.syncAckTimeoutMs : 500;
    }

public:
    // Back-compat re-export of the
    // production cache size for tests
    // and dashboard that used to read
    // AutoLink::ARQ_CACHE_SLOTS_PUBLIC.
    static constexpr int ARQ_CACHE_SLOTS_PUBLIC = ArqCache::SLOTS;


    AutoLink(const AutoLink &) = delete;
    AutoLink &operator=(const AutoLink &) = delete;

    AutoLink(uart_port_t u_num, int rx_pin, int tx_pin, bool isMasterNode,
             AutoLinkConfig cfg = AutoLinkConfig());

#ifdef ARDUINO
    ~AutoLink() = default;
#else
    ~AutoLink() = default;
#endif

#ifdef AUTOLINK_HOST_TEST
    // Host-test seam: inject a non-EspHal IHal
    // (MockHal or test stub) by reference. The
    // production ctor above is the only path
    // sketches can reach; this exists so the
    // facade is testable against an IHal
    // implementation the device build can't
    // construct (no FreeRTOS). Reference
    // arguments reject null at compile time
    // and make ownership unambiguous — the
    // caller still owns the IHal, and
    // RefViewDeleter above prevents the
    // facade dtor from double-freeing it.
    AutoLink(IHal &hal_in, bool isMasterNode,
             AutoLinkConfig cfg = AutoLinkConfig()) {
        cfg.clampToMaxBauds();
        hal = IHalPtr(&hal_in);
        link = std::make_unique<Link>(*hal, arqCache_, isMasterNode, cfg);
    }
#endif

    void begin() {
        // Order matters: bring up the link
        // layer first (state init, dwell
        // compute, ARQ cache reset) and then
        // the HAL (UART install, event task,
        // timer). The pre-refactor flow had
        // hal->begin() call link->begin()
        // through the HAL's back-pointer;
        // the ILinkEvents split removed that
        // path so the facade owns the order.
        link->begin();
#ifdef ARDUINO
        hal->begin();
#endif
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
    void dropLink() { link->dropLink(); }
    void flushRx() { link->flushRx(); }

    void setLinkPaused(bool p) { link->setLinkPaused(p); }

    // Fire the wire-side SWP handshake start. Used by
    // Ping to defer the master break until the user
    // pushes the dashboard's Start button. No-op when
    // the link is already running or when paused.
    void kickoff() { link->kickoff(); }

    void setMode(AutoLinkConfig::Mode m) {
        if (link)
            link->setMode(m);
    }
    AutoLinkConfig::Mode mode() const {
        return link ? link->mode() : AutoLinkConfig::Mode::SYNC;
    }

    // Configured max message size. Forwarded to
    // Link; falls back to the AutoLinkConfig default
    // before the facade is constructed so callers
    // like Ping can read it from any state.
    size_t maxMsg() const { return link ? link->maxMsg() : (size_t)1024; }

    void setTxDelayMs(int ms) {
        if (link)
            link->setTxDelayMs(ms);
    }
    int txDelayMs() const { return link ? link->txDelayMs() : 0; }

    // this release: Ping's gap-stop / gap-resume detector
    // and bytes-recvd log line read these directly.
    // The facade forwards to the link layer; the
    // link layer takes its lock internally.
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
    // this release: Ping needs the FIRST chunk's cobsSeq after
    // send() so it can match the peer's wire ACK to the
    // pending slot. The facade forwards to Link's
    // sendMsg(b, len, outBaseSeq). nullptr outBaseSeq is
    // fine when the caller doesn't care.
    bool sendMsg(const uint8_t *b, int len, uint8_t *outBaseSeq) {
        if (len <= 0) {
            if (outBaseSeq)
                *outBaseSeq = 0;
            return link->sendMsg(b, len);
        }
        return link->sendMsg(b, len, outBaseSeq);
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