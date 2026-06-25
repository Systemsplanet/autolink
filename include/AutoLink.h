// Public Arduino/ESP-IDF facade: bridges the protocol
// Link to the user-facing Stream API and owns the
// payload ARQ cache.
#pragma once
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
namespace autolink
{
struct EspHal : public IHal {
    ~EspHal() override = default;
    EspHal() = default;
    EspHal(int, int, int, autolink::AutoLinkConfig) {}
    void begin() override {}
    void setSpd(uint32_t) override {}
    void sendBreak() override {}
    int tx(const uint8_t *, int) override { return 0; }
    void flushTx() override {}
    void startTimer(int) override {}
    void stopTimer() override {}
    void delayMs(int) override {}
    uint32_t nowMs() override { return 0; }
    void lock() const override {}
    void unlock() const override {}
    int pushAppBuf(const uint8_t *, int) override
    {
        return 0;
    }
    int popAppBuf(uint8_t *, int) override
    {
        return 0;
    }
    int peekAppBuf() const override { return -1; }
    int peekAt(uint8_t *, int, int) const override
    {
        return 0;
    }
    int appBufAvailable() const override { return 0; }
    void clearAppBuf() override {}
    void flushRxHw() override {}
};
struct EspBlinkHal {
    ~EspBlinkHal() = default;
    EspBlinkHal() = default;
    EspBlinkHal(int) {}
    void bind(void *) {}
};
} // namespace autolink
#else
#    include "al/hal/EspHal.h"
#endif

#ifdef ARDUINO
#    include <Stream.h>
#else
class Stream
{
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *buffer,
                         size_t size) = 0;
    virtual void flush() = 0;
};
#endif

namespace autolink
{
#define AUTOLINK_VERSION "5.3.28"

class AutoLink : public Stream
{
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
#ifdef ARDUINO
    EspBlinkHal blinkHal;
    UtilBlink blinker;
#endif


    struct Pending {
        uint16_t len = 0;
        uint8_t poolIdx = 0xFF;
        bool in_use = false;
    };


    static constexpr int ARQ_CACHE_SLOTS = 256;
    static constexpr int ARQ_CACHE_POOL_SIZE = 16;
    static constexpr int ARQ_POOL_BUF_MAX = 256;
    Pending pending_[ARQ_CACHE_SLOTS];
    int pendingCount_ = 0;
    uint8_t arqPool_[ARQ_CACHE_POOL_SIZE]
                    [ARQ_POOL_BUF_MAX];
    bool arqPoolUsed_[ARQ_CACHE_POOL_SIZE] = {};


    bool arqCache_hasRoom();
    void arqCache_insert_unlocked(
        uint8_t seq, const uint8_t *payload,
        int payloadLen, uint8_t chunkCount);


    int arqCache_findBySeq(uint8_t seq);
    void arqCache_freeBySeq(uint8_t seq);
    bool arqCache_retx(uint8_t seq);
    void arqCache_clearAll();
    void assertCacheInvariants() const;


    static void linkResetHookTrampoline(void *ctx);

    static bool arqAckHookTrampoline(uint8_t ackedSeq,
                                     void *ctx);
    static bool arqRetxHookTrampoline(uint8_t retxSeq,
                                      void *ctx);
    static bool arqCacheHasRoomTrampoline(void *ctx);
    static void arqCacheInsertTrampoline(
        uint8_t baseSeq, const uint8_t *payload,
        int payloadLen, uint8_t chunkCount, void *ctx);
    static void arqCacheClearAllTrampoline(void *ctx);

public:
    static constexpr int ARQ_CACHE_SLOTS_PUBLIC =
        ARQ_CACHE_SLOTS;
    int arqCacheSizeForTest() const
    {
        int n = 0;
        for (int i = 0; i < ARQ_CACHE_SLOTS; i++)
            if (pending_[i].in_use)
                n++;
        return n;
    }
    Link *linkForTest() { return link.get(); }
    const Link *linkForTest() const
    {
        return link.get();
    }
    void test_arqCache_put(uint8_t seq,
                           const uint8_t *b, int len,
                           uint8_t)
    {
        arqCache_insert_unlocked(seq, b, len, 1);
    }
    bool test_arqCache_hasRoom()
    {
        return arqCache_hasRoom();
    }
    void test_arqCache_freeBySeq(uint8_t s)
    {
        arqCache_freeBySeq(s);
    }
    bool test_arqCache_retx(uint8_t seq)
    {
        return arqCache_retx(seq);
    }
    int test_arqCache_findBySeq(uint8_t s)
    {
        return arqCache_findBySeq(s);
    }
    void test_markAckedPending(uint8_t s)
    {
        if (link)
            link->test_markAckedPending(s);
    }
    static void test_linkResetHookTrampoline(void *ctx)
    {
        linkResetHookTrampoline(ctx);
    }

    AutoLink(const AutoLink &) = delete;
    AutoLink &operator=(const AutoLink &) = delete;

    AutoLink(uart_port_t u_num, int rx_pin, int tx_pin,
             bool isMasterNode,
             AutoLinkConfig cfg = AutoLinkConfig())
#ifdef ARDUINO
        : blinkHal(cfg.ledPin), blinker(blinkHal)
#endif
    {
#ifdef ARDUINO
        blinkHal.bind(&blinker);
#endif
#ifdef ARDUINO
        constexpr int kHdr = MSG_HDR;
#else
        constexpr int kHdr = 6;
#endif
        size_t need = 2 * 16 * (cfg.maxMsg + kHdr);
        if (cfg.streamBufferSize < need)
            cfg.streamBufferSize = need;
        size_t need_tx =
            16 * ((cfg.maxMsg + kHdr) * 5 / 4 + 64);
        if (cfg.txBufferSize < need_tx)
            cfg.txBufferSize = need_tx;
        // Auto-size: caller set 0 → pick from maxMsg.
        // Manual override honored.
        hal = IHalPtr(std::make_unique<EspHal>(
                          u_num, rx_pin, tx_pin, cfg)
                          .release());
        link = std::make_unique<Link>(
            *hal, isMasterNode, cfg);


        link->setArqCacheHooks(
            &arqAckHookTrampoline,
            &arqRetxHookTrampoline,
            &arqCacheHasRoomTrampoline,
            &arqCacheInsertTrampoline,
            &arqCacheClearAllTrampoline, this);
        link->setLinkResetHook(
            &linkResetHookTrampoline, this);
    }

#ifdef ARDUINO
    ~AutoLink()
    {
        (void)pending_;
        (void)arqPool_;
        (void)arqPoolUsed_;
    }
#else
    ~AutoLink() = default;
#endif

#ifdef AUTOLINK_HOST_TEST


    AutoLink(IHal *hal_in, bool isMasterNode,
             AutoLinkConfig cfg = AutoLinkConfig())
    {
        size_t need = 2 * 16 * (cfg.maxMsg + 6);
        if (cfg.streamBufferSize < need)
            cfg.streamBufferSize = need;
        size_t need_tx =
            16 * ((cfg.maxMsg + 6) * 5 / 4 + 64);
        if (cfg.txBufferSize < need_tx)
            cfg.txBufferSize = need_tx;
        hal = IHalPtr(hal_in);
        link = std::make_unique<Link>(
            *hal, isMasterNode, cfg);
        link->setArqCacheHooks(
            &arqAckHookTrampoline,
            &arqRetxHookTrampoline,
            &arqCacheHasRoomTrampoline,
            &arqCacheInsertTrampoline,
            &arqCacheClearAllTrampoline, this);


        link->setLinkResetHook(
            &linkResetHookTrampoline, this);
    }
#endif

    void begin()
    {
#ifdef ARDUINO
        hal->begin();
#else
        link->begin();
#endif
    }

    void blinkWait(int n, int onMs = 60,
                   int offMs = 60, long delayMs = 0)
    {
        if (n <= 0)
            return;
#ifdef ARDUINO
        if (delayMs > 0)
            blinker.flashBlocking(n, onMs, offMs,
                                  delayMs);
        else
            blinker.start(n, onMs, offMs);
#else
        (void)onMs;
        (void)offMs;
        (void)delayMs;
#endif
    }

    int send(const uint8_t *b, int len)
    {
        if (len <= 0)
            return 0;
        return sendMsg(b, len) ? len : 0;
    }
    int recv(uint8_t *b, int max_len)
    {
        return link->recvMsg(b, max_len);
    }
    bool ready() const
    {
        return link->getState() == State::OK;
    }
    void dropLink() { link->dropLink(); }
    void flushRx() { link->flushRx(); }


    void setLinkPaused(bool p)
    {
        link->setLinkPaused(p);
    }

    void getStats(Stats &s) const
    {
        link->getStats(s);
    }
    void resetStats() { link->resetStats(); }
    void resetErrors() { link->resetErrors(); }
    void resetDiag() { link->resetDiag(); }

    bool isHealthy() const
    {
#ifdef ARDUINO
        return hal->isHealthy();
#else
        return false;
#endif
    }

    int available() override
    {
        return link->available();
    }
    int read() override { return link->read(); }
    int peek() override { return link->peek(); }
    size_t write(uint8_t b) override
    {
        return link->write(&b, 1);
    }
    size_t write(const uint8_t *buffer,
                 size_t size) override
    {
        return link->write(buffer, (int)size);
    }
    void flush() override { return link->flush(); }
    int read(uint8_t *b, int max_len)
    {
        return link->read(b, max_len);
    }

    bool sendMsg(const uint8_t *b, int len)
    {
        if (len <= 0)
            return link->sendMsg(b, len);
        return link->sendMsg(b, len, nullptr);
    }
    int recvMsg(uint8_t *b, int max_len)
    {
        return link->recvMsg(b, max_len);
    }

    void err() { link->err(); }
    void clearErr() { link->clearErr(); }
    int getErrCount() const
    {
        return link->getErrCount();
    }
    State getState() const { return link->getState(); }
    uint32_t getCurrentBaud() const
    {
        return link->getCurrentBaud();
    }
    void getDiag(Diag &d) const { link->getDiag(d); }

    size_t getStreamBufferSize() const
    {
        return link->getConfig().streamBufferSize;
    }
};

} // namespace autolink