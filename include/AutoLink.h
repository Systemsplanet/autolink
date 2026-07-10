
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
#define AUTOLINK_VERSION "6.1.15"

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

    ArqCache arqCache_{ AUTOLINK_ARQ_PIPELINE_WINDOW };
#ifdef ARDUINO
    EspBlinkHal blinkHal;
    UtilBlink blinker;
#endif

    friend class AutoLinkTestAccessor;
    int arqCacheSizeForTest() const { return arqCache_.size(); }
    Link *linkForTest() { return link.get(); }
    const Link *linkForTest() const { return link.get(); }
    ArqCache *arqCacheForTest() { return &arqCache_; }
    const ArqCache *arqCacheForTest() const { return &arqCache_; }
#ifdef AUTOLINK_HOST_TEST
    // Host-only shims reached via AutoLinkTestAccessor.
    void test_arqCache_put(uint8_t seq, const uint8_t *b, int len, uint8_t) {
        arqCache_.testPut(seq, b, len);
    }
    bool test_arqCache_hasRoom() { return arqCache_.hasRoom(); }
    static constexpr int test_arqPoolSize() { return ArqCache::POOL_SIZE; }
    void test_arqCache_freeBySeq(uint8_t s) { arqCache_.freeBySeq(s); }
    bool test_arqCache_retx(uint8_t seq) {
        const uint8_t *buf = nullptr;
        int len = 0;
        return arqCache_.testRetx(seq, &buf, &len);
    }
    int test_arqCache_findBySeq(uint8_t s) {
        return arqCache_.slotInUse(s) ? (int)s : -1;
    }
#endif

public:
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

    AutoLink(IHal &hal_in, bool isMasterNode,
             AutoLinkConfig cfg = AutoLinkConfig()) {
        cfg.clampToMaxBauds();
        hal = IHalPtr(&hal_in);
        link = std::make_unique<Link>(*hal, arqCache_, isMasterNode, cfg);
    }
#endif

    void begin() {
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

    void kickoff() { link->kickoff(); }

    void setMode(AutoLinkConfig::Mode m) {
        if (hal)
            hal->setMode(m);
        if (link)
            link->setMode(m);
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
