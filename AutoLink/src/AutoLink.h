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
#include <functional>   // v5.1.37: no-op deleter for borrowed ILink*

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
#define AUTOLINK_VERSION "5.1.38"

class AutoLink : public Stream {
private:
#ifdef AUTOLINK_HOST_TEST
    // v5.1.37: the host-only ILink* injection constructor needs
    // a unique_ptr<ILink, NoOpDeleter> because the ILink is
    // BORROWED (not owned). Use a custom deleter type alias so
    // the member declaration works for both the production
    // (default_delete<ILink>) and host (NoOpDeleter) paths.
    struct NoOpDeleter {
        void operator()(ILink*) const noexcept { /* borrowed, no-op */ }
    };
    using ILinkPtr = std::unique_ptr<ILink, NoOpDeleter>;
#else
    using ILinkPtr = std::unique_ptr<ILink>;
#endif

    // v5.1.37: holds the ILink. In production this owns an EspHal
    // (default constructor). The host-only ILink* injection
    // constructor below uses a no-op deleter because the test's
    // MockHal is owned by WireSim (heap-allocated) and the
    // AutoLink's ILink is a borrowed observer, not an owner.
    // WireSim's unique_ptr<MockHal> does the actual delete.
    ILinkPtr hal;
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
    // v5.1.37: ARQ_CACHE_SLOTS = 48 (1.5x WINDOW). The cache needs
    // headroom over the link's in-flight window because: (a) on
    // link drop + re-sweep, the new session restarts from cobsSeq=0
    // and the old session's slots can persist briefly before the
    // reset hook fires; (b) the cache takes a beat to receive the
    // first round of ACKs from the new session; (c) one or two
    // mis-keyed / leaked slots are tolerable but two or three with
    // zero margin ratchets the cache to full and the gate latches.
    // 32 -> 48 = +50% headroom, ~16 KB extra heap at maxMsg=1024.
    // WINDOW stays 32 (UtilPing's app-side pacing).
    static constexpr int ARQ_CACHE_SLOTS = 48;
    Pending pending_[ARQ_CACHE_SLOTS];
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
    // v5.1.37: free every pending payload, zero pendingCount_, and
    // memset seqToPending_ to -1. Called from the link-reset hook
    // (ALink::reset_unlocked -> linkResetCallback_). Without this,
    // a link drop orphaned the cache: old session's cobsSeqs were
    // never reclaimed by arqCache_freeBySeq (the new session
    // reuses low seqs first, doesn't sweep back through the old
    // high range), pendingCount_ never returned to 0, and the
    // v5.1.36 cache-full gate latched on the very next sendMsg.
    // This is the facade half of the v5.1.37 "drop clears both
    // layers" fix. Protocol half lives in ALink::reset_unlocked.
    void    arqCache_clearAll();
    // v5.1.37: C-linkage trampoline fired by ALink::reset_unlocked
    // after it has cleared its own ARQ state. Calls
    // arqCache_clearAll() on the facade. ctx is the AutoLink*.
    // Public so the host test can drive the link-reset path
    // directly (without UART) and pin the cache-clear behavior.
    static void linkResetHookTrampoline(void* ctx);

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
    // v5.1.37: expose ARQ_CACHE_SLOTS as a public alias so the
    // test can size its fixture without poking at the private
    // constant directly.
    static constexpr int ARQ_CACHE_SLOTS_PUBLIC = ARQ_CACHE_SLOTS;
    // Public so the test file can call them directly; the rest of
    // the AutoLink class is unchanged.
    int  arqCacheSizeForTest() const {
        int n = 0;
        for (int i = 0; i < ARQ_CACHE_SLOTS; i++) if (pending_[i].in_use) n++;
        return n;
    }
    // v5.1.19: test-only accessor to drive ALink::onTimer() so the
    // host test can pin the deferred-retransmit fix without a real
    // FreeRTOS timer. The accessor returns the ALink*; the test
    // then calls onTimer() to fire the OK tick, then advances the
    // MockHal clock and calls onTimer() again to trigger a retx.
    ALink* linkForTest() { return link.get(); }
    // v5.1.37: const overload so WireSim can call from const
    // contexts (e.g. bytesTransferredAtoB is const). getStats is
    // already const on ALink.
    const ALink* linkForTest() const { return link.get(); }
    // Thin wrappers around the private cache methods so the host
    // test can exercise them. Public on purpose for testing only;
    // production callers go through send() / sendMsg() / the hook
    // trampolines.
    void test_arqCache_put(uint8_t baseSeq, const uint8_t* b, int len, uint8_t chunkCount)
        { arqCache_put(baseSeq, b, len, chunkCount); }
    bool test_arqCache_retx(uint8_t baseSeq) { return arqCache_retx(baseSeq); }
    void test_arqCache_takeRetxBuffer(uint8_t baseSeq, uint8_t** bufOut, int* lenOut)
        { arqCache_takeRetxBuffer(baseSeq, bufOut, lenOut); }
    // v5.1.37: test hook for retx_resend. Public so the test can
    // drive the retx path without going through sendMsg, which
    // would require UART in production.
    void test_retx_resend(const uint8_t* buf, int len) { retx_resend(buf, len); }
    // v5.1.37: test hook for the link-reset trampoline. The
    // production path is ALink::reset_unlocked -> linkResetCallback_
    // -> linkResetHookTrampoline -> arqCache_clearAll. Tests can
    // call this directly to drive the cache-clear without needing
    // a full drop-then-renegotiate cycle (which requires UART).
    static void test_linkResetHookTrampoline(void* ctx)
        { linkResetHookTrampoline(ctx); }

    AutoLink(const AutoLink&) = delete;
    AutoLink& operator=(const AutoLink&) = delete;

    AutoLink(uart_port_t u_num, int rx_pin, int tx_pin, bool isMasterNode, AutoLinkConfig cfg = AutoLinkConfig())
#ifdef ARDUINO
        : blinkHal(cfg.ledPin), blinker(blinkHal)
#endif
    {
#ifdef ARDUINO
        blinkHal.bind(&blinker);
#endif
#ifdef ARDUINO
        memset(seqToPending_, -1, sizeof(seqToPending_));

        size_t need = 2 * 16 * (cfg.maxMsg + MSG_HDR);
        if (cfg.streamBufferSize < need) cfg.streamBufferSize = need;
        size_t need_tx = 16 * ((cfg.maxMsg + MSG_HDR) * 5 / 4 + 64);
        if (cfg.txBufferSize < need_tx) cfg.txBufferSize = need_tx;

        hal = ILinkPtr(std::make_unique<EspHal>(u_num, rx_pin, tx_pin, cfg).release());
        link = std::make_unique<ALink>(*hal, isMasterNode, cfg);
        // v5 ARQ: register cache hooks with the protocol layer.
        link->setArqHooks(&arqAckHookTrampoline, &arqRetxHookTrampoline, this);
        // v5.1.37: register the link-reset hook so ALink::reset_unlocked
        // can also clear the facade's payload cache. Without this, a
        // link drop orphans the cache and the v5.1.36 cache-full gate
        // latches.
        link->setLinkResetHook(&linkResetHookTrampoline, this);
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
        // null-deref. The stub EspHal is a no-op; the NoOpDeleter
        // we wrap it in is intentional — the stub is a stack of
        // virtual no-ops that never needs to be freed. (Disclosed
        // v5.1.14.) The production ARDUINO path uses the default
        // deleter because the real EspHal has FreeRTOS resources
        // that need to be released in ~EspHal().
        hal = ILinkPtr(std::make_unique<EspHal>(u_num, rx_pin, tx_pin, cfg).release());
        link = std::make_unique<ALink>(*hal, isMasterNode, cfg);
        link->setArqHooks(&arqAckHookTrampoline, &arqRetxHookTrampoline, this);
        link->setLinkResetHook(&linkResetHookTrampoline, this);
#endif
    }

#ifdef ARDUINO
    ~AutoLink() {
        for (int i = 0; i < ARQ_CACHE_SLOTS; i++) free(pending_[i].buf);
    }
#else
    ~AutoLink() = default;
#endif

#ifdef AUTOLINK_HOST_TEST
    // v5.1.37 (closed-loop test): host-only constructor that takes
    // an injected ILink*. The AutoLink facade borrows the ILink
    // (does NOT take ownership) and builds the ALink protocol
    // layer on top of it, exactly like the production constructor
    // does with EspHal. This lets the host test put a real ILink
    // (MockHal) under the facade and drive the closed loop with
    // two AutoLink instances pipe'd together. Without this, the
    // only way to get an AutoLink running on host is with the
    // no-op EspHal stub, which never moves bytes, so the closed
    // loop can't be exercised.
    //
    // The ILink is BORROWED, not owned. WireSim heap-allocates
    // the MockHals and stores them in unique_ptr<MockHal> so the
    // MockHal dtors run in the right order (MockHals are destroyed
    // AFTER the AutoLinks, since AutoLinks are declared later in
    // WireSim and destruction order is reverse of declaration).
    // We use a no-op deleter on the AutoLink's unique_ptr<ILink>
    // so its dtor doesn't double-free.
    //
    // Production callers use the (u_num, rx_pin, tx_pin, ...) form.
    // This constructor is gated to AUTOLINK_HOST_TEST so it can't
    // be misused on real hardware (the test's MockHal is not a
    // real ILink).
    AutoLink(ILink* hal_in, bool isMasterNode, AutoLinkConfig cfg = AutoLinkConfig()) {
        memset(seqToPending_, -1, sizeof(seqToPending_));
        // Mirror the ARDUINO-side auto-sizing so getStreamBufferSize
        // returns the same value on host. (Disclosed v5.1.14.)
        size_t need = 2 * 16 * (cfg.maxMsg + 6);
        if (cfg.streamBufferSize < need) cfg.streamBufferSize = need;
        size_t need_tx = 16 * ((cfg.maxMsg + 6) * 5 / 4 + 64);
        if (cfg.txBufferSize < need_tx) cfg.txBufferSize = need_tx;
        // Borrow the ILink* with a no-op deleter; the caller
        // (WireSim) owns it and will free it via its own
        // unique_ptr<MockHal>.
        hal = ILinkPtr(hal_in);
        link = std::make_unique<ALink>(*hal, isMasterNode, cfg);
        // v5 ARQ: register cache hooks with the protocol layer.
        link->setArqHooks(&arqAckHookTrampoline, &arqRetxHookTrampoline, this);
        // v5.1.37: register the link-reset hook so ALink::reset_unlocked
        // can also clear the facade's payload cache. Without this, a
        // link drop orphans the cache and the v5.1.36 cache-full gate
        // latches.
        link->setLinkResetHook(&linkResetHookTrampoline, this);
    }
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

    // v5.1.31: forward facade-driven link pause to the protocol
    // layer. While paused, the protocol suppresses idle-watchdog
    // drops and keepalive emissions. Used by UtilPing's Pause/Start
    // button so the link stays up while the operator inspects the
    // dashboard before clicking Start. Default false (sends normally).
    void setLinkPaused(bool p) { link->setLinkPaused(p); }

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
        // v5.1.35/5.1.36/5.1.37: stall before sending wire bytes if
        // the ARQ cache is full. The cache is sized to give margin
        // over the link's WINDOW (see ARQ_CACHE_SLOTS), but a burst
        // that fills the cache still must wait for ACKs to free
        // slots before pumping more bytes. We refuse to send here,
        // which propagates as sendMsg=false to the caller
        // (Ping::loop), which stops pumping new bytes. The next loop
        // tick will retry once ACKs have freed slots.
        if (pendingCount_ >= ARQ_CACHE_SLOTS) {
            Log::log().warning("AutoLink",
                "ARQ cache full (%d pending); sendMsg stalling until ACKs free a slot. "
                "This is back-pressure, not an error.",
                pendingCount_);
            return false;
        }
        // v5.1.37: sendMsgEx returns the base cobsSeq the protocol
        // stamped on the wire, atomically under the link lock. The
        // old peekTxSeq()+sendMsg() pair had a race: a keepalive
        // (sendCobsFrame_unlocked(nullptr,0) on the timer task) or
        // an onRx-driven frame could advance txSeq between the peek
        // and the actual send, so the cache ended up keyed under a
        // seq the wire never carried. The slot would never get
        // ACKed, chunks_left never reached 0, slot leaked,
        // pendingCount_ crept to ARQ_CACHE_SLOTS, gate latched.
        uint8_t seq = 0;
        bool ok = link->sendMsgEx(b, len, &seq);
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
