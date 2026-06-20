// AutoLink.h — public facade. The only header most sketches need.
// Wires the protocol core (ALink) to the ESP32 hardware (EspHal),
// auto-sizes buffers from cfg.maxMsg, exposes send()/recv()/ready(),
// the Arduino Stream byte API, and drives a status LED via UtilBlink.
//
// v5: send()/sendMsg() are now ARQ-reliable. Each cobsSeq-bearing
// frame arms a retransmit timer; missing ACKs trigger up to MAX_RETX
// retransmits. The on-wire guarantee is "every accepted recvMsg()
// corresponds to exactly one sendMsg() that was acknowledged by the
// peer" — no message is silently lost on a healthy link.
#pragma once
#include "al/protocol/ALink.h"
#include "al/util/Log.h"
#include "al/util/UtilBlink.h"
#include <memory>
#include <string.h>     // memset/memcpy
#include <stdlib.h>     // malloc/free

#ifdef AUTOLINK_HOST_TEST
// Host stub for EspHal: a minimal ILink implementation so
// std::make_unique<ALink>(*hal, ...) works on host without real
// hardware. The host tests that need richer behaviour (the loopback
// pipe, app buffer, etc.) can substitute their own ILink before
// constructing AutoLink — but the default constructor path uses
// this no-op stub so that getStreamBufferSize, ready(), available()
// etc. don't null-deref on host. (v5.1.14: pre-v5.1.14 the host
// branch left hal/link==nullptr and the first accessor call
// segfaulted. The pre-existing stub was insufficient because it
// didn't satisfy ILink, so ALink couldn't be instantiated.)
typedef int uart_port_t;
#include "al/hal/ILink.h"
namespace autolink {
struct EspHal : public ILink {
    ~EspHal() override = default;
    EspHal() = default;
    EspHal(int, int, int, autolink::AutoLinkConfig) {}
    void begin() override {}
    void setSpd(uint32_t) override {}
    void sendBreak() override {}
    int tx(const uint8_t*, int) override { return 0; }
    void flushTx() override {}
    void startTimer(int) override {}
    void stopTimer() override {}
    void delayMs(int) override {}
    uint32_t nowMs() override { return 0; }
    void lock() const override {}
    void unlock() const override {}
    int pushAppBuf(const uint8_t*, int) override { return 0; }
    int popAppBuf(uint8_t*, int) override { return 0; }
    int peekAppBuf() const override { return -1; }
    int peekAt(uint8_t*, int, int) const override { return 0; }
    int appBufAvailable() const override { return 0; }
    void clearAppBuf() override {}
    void flushRxHw() override {}
};
struct EspBlinkHal {
    ~EspBlinkHal() = default;
    EspBlinkHal() = default;
    EspBlinkHal(int) {}
    void bind(void*) {}
};
} // namespace autolink

#else
#include "al/hal/EspHal.h"
#endif

#ifdef ARDUINO
#include <Stream.h>
#else
class Stream {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *buffer, size_t size) = 0;
    virtual void flush() = 0;
};
#endif

namespace autolink {

// Keep in sync with library.properties.
#define AUTOLINK_VERSION "5.1.30"

class AutoLink : public Stream {
private:
    std::unique_ptr<EspHal> hal;
    std::unique_ptr<ALink> link;
#ifdef ARDUINO
    EspBlinkHal blinkHal;
    UtilBlink   blinker;
#endif

    // v5 ARQ: payload cache. send() copies the payload here keyed by
    // the cobsSeq ALink assigned to the message's HEADER chunk, so
    // the cache hooks can resend the exact same bytes on retransmit.
    // 32 slots * (cfg.maxMsg + ~16 B overhead) ≈ 32 KB at maxMsg=1024
    // — modest for the reliability guarantee.
    //
    // A multi-chunk message shares one cache slot (one send() call =
    // one slot, regardless of how many wire chunks it became). The
    // protocol tracks per-chunk ACK state; the slot is freed only
    // when ALL chunks of the message have been ACKed (chunks_left
    // reaches 0). On any chunk's retransmit, the hook re-sends the
    // WHOLE message from offset 0 under a new cobsSeq — the peer's
    // cobsSeq gap logic then re-syncs.
    struct Pending {
        uint8_t  seq = 0;         // base cobsSeq this message was sent under
        uint8_t* buf = nullptr;   // heap-allocated copy of the payload
        int      len = 0;
        uint8_t  chunks_left = 0; // chunks still waiting for ACK (header + payload)
        bool     in_use = false;
    };
    Pending pending_[32];
    int     pendingCount_ = 0;
    int8_t  seqToPending_[256];  // base cobsSeq -> pending[] index, -1 if none
    void    arqCache_put(uint8_t baseSeq, const uint8_t* b, int len, uint8_t chunkCount);
    int8_t  arqCache_findBySeq(uint8_t baseSeq);
    void    arqCache_freeBySeq(uint8_t baseSeq);
    bool    arqCache_retx(uint8_t baseSeq);
    // Take the payload out of the cache for retransmission and free
    // the slot. (v5.1.14: split out of arqCache_retx so the host
    // test can pin the fix without driving sendMsg.)
    void    arqCache_takeRetxBuffer(uint8_t baseSeq, uint8_t** bufOut, int* lenOut);
    // Re-send a previously-cached payload.
    void    retx_resend(const uint8_t* buf, int len);

    // v5 ARQ trampolines. The protocol layer takes plain C function
    // pointers; we route through static methods that know about
    // AutoLink. Defined in AutoLink.cpp.
    static bool arqAckHookTrampoline(uint8_t ackedSeq, void* ctx);
    static bool arqRetxHookTrampoline(uint8_t retxSeq, void* ctx);

public:
    // Cache accessors for the host test (v5.1.14). These let the
    // test exercise the ARQ cache directly without needing the
    // hardware timer to drive retransmits. The test pins the
    // v5.1.14 fix that arqCache_retx must free the old slot
    // before retransmitting — otherwise every retx leaked a slot.
    // Public so the test file can call them directly; the rest of
    // the AutoLink class is unchanged.
    int  arqCacheSizeForTest() const {
        int n = 0;
        for (int i = 0; i < 32; i++) if (pending_[i].in_use) n++;
        return n;
    }
    // v5.1.19: test-only accessor to drive ALink::onTimer() so the
    // host test can pin the deferred-retransmit fix without a real
    // FreeRTOS timer. The accessor returns the ALink*; the test
    // then calls onTimer() to fire the OK tick, then advances the
    // MockHal clock and calls onTimer() again to trigger a retx.
    ALink* linkForTest() { return link.get(); }
    // Thin wrappers around the private cache methods so the host
    // test can exercise them. Public on purpose for testing only;
    // production callers go through send() / sendMsg() / the hook
    // trampolines.
    void test_arqCache_put(uint8_t baseSeq, const uint8_t* b, int len, uint8_t chunkCount)
        { arqCache_put(baseSeq, b, len, chunkCount); }
    bool test_arqCache_retx(uint8_t baseSeq) { return arqCache_retx(baseSeq); }
    void test_arqCache_takeRetxBuffer(uint8_t baseSeq, uint8_t** bufOut, int* lenOut)
        { arqCache_takeRetxBuffer(baseSeq, bufOut, lenOut); }

    AutoLink(const AutoLink&) = delete;
    AutoLink& operator=(const AutoLink&) = delete;

    AutoLink(uart_port_t u_num, int rx_pin, int tx_pin, bool isMasterNode, AutoLinkConfig cfg = AutoLinkConfig())
#ifdef ARDUINO
        : blinkHal(cfg.ledPin), blinker(blinkHal)
#endif
    {
#ifdef ARDUINO
        blinkHal.bind(&blinker);
        memset(seqToPending_, -1, sizeof(seqToPending_));

        size_t need = 2 * 16 * (cfg.maxMsg + MSG_HDR);
        if (cfg.streamBufferSize < need) cfg.streamBufferSize = need;
        size_t need_tx = 16 * ((cfg.maxMsg + MSG_HDR) * 5 / 4 + 64);
        if (cfg.txBufferSize < need_tx) cfg.txBufferSize = need_tx;

        hal = std::make_unique<EspHal>(u_num, rx_pin, tx_pin, cfg);
        link = std::make_unique<ALink>(*hal, isMasterNode, cfg);
        // v5 ARQ: register cache hooks with the protocol layer.
        link->setArqHooks(&arqAckHookTrampoline, &arqRetxHookTrampoline, this);
#else
        memset(seqToPending_, -1, sizeof(seqToPending_));
        // Mirror the ARDUINO-side auto-sizing so getStreamBufferSize
        // returns the same value on host. (Disclosed v5.1.14.)
        size_t need = 2 * 16 * (cfg.maxMsg + 6);
        if (cfg.streamBufferSize < need) cfg.streamBufferSize = need;
        size_t need_tx = 16 * ((cfg.maxMsg + 6) * 5 / 4 + 64);
        if (cfg.txBufferSize < need_tx) cfg.txBufferSize = need_tx;
        // Instantiate the stub HAL+ALink pair so accessors that
        // dereference link (getState, available, getStats, ...) don't
        // null-deref. (Disclosed v5.1.14.)
        hal = std::make_unique<EspHal>(u_num, rx_pin, tx_pin, cfg);
        link = std::make_unique<ALink>(*hal, isMasterNode, cfg);
        link->setArqHooks(&arqAckHookTrampoline, &arqRetxHookTrampoline, this);
#endif
    }

#ifdef ARDUINO
    ~AutoLink() {
        for (int i = 0; i < 32; i++) free(pending_[i].buf);
    }
#else
    ~AutoLink() = default;
#endif

    void begin() {
#ifdef ARDUINO
        hal->begin();
#endif
    }

    void blinkWait(int n, int onMs = 60, int offMs = 60, long delayMs = 0) {
        if (n <= 0) return;
#ifdef ARDUINO
        if (delayMs > 0) blinker.flashBlocking(n, onMs, offMs, delayMs);
        else            blinker.start(n, onMs, offMs);
#else
        (void)onMs; (void)offMs; (void)delayMs;  // no-op on host
#endif
    }

    // ===== v5 ARQ send (was best-effort in v4) =====
    // Returns the number of bytes accepted for transmission. ARQ
    // makes this "exactly len on a healthy link" — the bytes are
    // cached and retransmitted until the peer ACKs them. If the
    // link drops after we've cached the bytes, those bytes are
    // lost (no persistent queue across re-sweeps).
    int  send(const uint8_t* b, int len) {
        if (len <= 0) return 0;
        return sendMsg(b, len) ? len : 0;
    }
    int  recv(uint8_t* b, int max_len) {
        return link->recvMsg(b, max_len);
    }
    bool ready() const { return link->getState() == State::OK; }
    void dropLink() { link->dropLink(); }
    void flushRx()  { link->flushRx(); }

    void getStats(Stats& s) const { link->getStats(s); }
    void resetStats()  { link->resetStats(); }
    void resetErrors() { link->resetErrors(); }
    void resetDiag()   { link->resetDiag(); }

    bool isHealthy() const {
#ifdef ARDUINO
        return hal->isHealthy();
#else
        return false;
#endif
    }

    // Raw Stream byte API (no message boundaries).
    int available() override { return link->available(); }
    int read() override { return link->read(); }
    int peek() override { return link->peek(); }
    size_t write(uint8_t b) override { return link->write(&b, 1); }
    size_t write(const uint8_t *buffer, size_t size) override { return link->write(buffer, (int)size); }
    void flush() override { link->flush(); }
    int read(uint8_t* b, int max_len) { return link->read(b, max_len); }

    bool sendMsg(const uint8_t* b, int len) {
        if (len <= 0) return link->sendMsg(b, len);
        uint8_t seq = link->peekTxSeq();
        bool ok = link->sendMsg(b, len);
        if (ok) {
            int payloadChunks = (len + MAX_CHUNK - 1) / MAX_CHUNK;
            arqCache_put(seq, b, len, (uint8_t)(1 + payloadChunks));
        }
        return ok;
    }
    int  recvMsg(uint8_t* b, int max_len) {
        return link->recvMsg(b, max_len);
    }

    void err() { link->err(); }
    void clearErr() { link->clearErr(); }
    int  getErrCount() const { return link->getErrCount(); }
    State getState() const { return link->getState(); }
    uint32_t getCurrentBaud() const { return link->getCurrentBaud(); }
    void getDiag(Diag& d) const { link->getDiag(d); }

    size_t getStreamBufferSize() const { return link->getConfig().streamBufferSize; }
};

} // namespace autolink
