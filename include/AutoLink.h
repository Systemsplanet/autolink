// Public facade: stream-style API over
// Link. ARQ payload cache lives in
// al/link/ArqCache.{h,cpp} behind IArqCache.
// AutoLink owns an ArqCache by value and
// hands a raw pointer to Link. Host-only
// EspHal / EspBlinkHal stubs live in
// test/common/EspHalStub.h. Public ctor
// body lives in src/AutoLink.cpp so the
// public HAL boundary doesn't leak into
// the host build.
#pragma once
#include "al/link/ArqCache.h"
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
#define AUTOLINK_VERSION "5.3.70"

class AutoLinkTestAccessor;

class AutoLink {
private:
#ifdef AUTOLINK_HOST_TEST
    struct NoOpDeleter {
        void operator()(IHal *) const noexcept {}
    };
    using IHalPtr = std::unique_ptr<IHal, NoOpDeleter>;
#else
    using IHalPtr = std::unique_ptr<IHal>;
#endif

    IHalPtr hal;
    std::unique_ptr<Link> link;
    ArqCache arqCache_;
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

    AutoLink(IHal *hal_in, bool isMasterNode,
             AutoLinkConfig cfg = AutoLinkConfig()) {
        size_t need = 2 * 16 * (cfg.maxMsg + 6);
        if (cfg.streamBufferSize < need)
            cfg.streamBufferSize = need;
        size_t need_tx = 16 * ((cfg.maxMsg + 6) * 5 / 4 + 64);
        if (cfg.txBufferSize < need_tx)
            cfg.txBufferSize = need_tx;
        hal = IHalPtr(hal_in);
        link = std::make_unique<Link>(*hal, isMasterNode, cfg);
        link->setArqCache(&arqCache_);
    }
#endif

    void begin() {
#ifdef ARDUINO
        hal->begin();
#else
        link->begin();
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

    void setMode(AutoLinkConfig::Mode m) {
        if (link)
            link->setMode(m);
    }
    AutoLinkConfig::Mode mode() const {
        return link ? link->mode() : AutoLinkConfig::Mode::SYNC;
    }

    void setTxDelayMs(int ms) {
        if (link)
            link->setTxDelayMs(ms);
    }
    int txDelayMs() const { return link ? link->txDelayMs() : 0; }

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