#pragma once
#include "EspHal.h"
#include "ALink.h"
#include "Log.h"
#include "UtilBlink.h"
#include <memory>

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

// ----------------------------------------------------------------------------
// AutoLink — the one-object public facade: construct as a global, begin(),
// then send()/recv(). Wires the protocol core (ALink) to the ESP32 hardware
// (EspHal), auto-sizes buffers from maxMsg, exposes the Arduino Stream byte
// API, and drives a status LED through UtilBlink.
// ----------------------------------------------------------------------------
class AutoLink : public Stream {
private:
    EspBlinkHal blinkHal;
    UtilBlink   blinker;
    std::unique_ptr<EspHal> hal;
    std::unique_ptr<ALink> link;

public:
    // The blink timer callback captures `this`; copies/moves would dangle.
    AutoLink(const AutoLink&) = delete;
    AutoLink& operator=(const AutoLink&) = delete;

    // Construct on the stack as a global — no new/pointer needed. Everything past
    // the role flag is optional; sane defaults cover the common case.
    AutoLink(uart_port_t u_num, int rx_pin, int tx_pin, bool isMasterNode, AutoLinkConfig cfg = AutoLinkConfig())
        : blinkHal(cfg.ledPin), blinker(blinkHal)
    {
        blinkHal.bind(&blinker);

        // Auto-size the reassembly buffer to two full messages so RX keeps
        // flowing while the app is briefly busy. The user never has to reason
        // about the maxMsg/streamBufferSize relationship.
        size_t need = 2 * (cfg.maxMsg + MSG_HDR);
        if (cfg.streamBufferSize < need) cfg.streamBufferSize = need;

        hal = std::make_unique<EspHal>(u_num, rx_pin, tx_pin, cfg);
        link = std::make_unique<ALink>(*hal, isMasterNode, cfg);
    }

    void begin() {
        hal->begin();
    }

    // Flash the status LED n times.
    //   delayMs == 0 (default): asynchronous. Returns immediately; the
    //     pattern runs on an esp_timer, so blinkWait(1) per packet costs
    //     nothing. A new call replaces any pattern still running.
    //   delayMs > 0: blocking. Flashes, then pauses delayMs -- holds the CPU
    //     for n * (onMs + offMs) + delayMs ms. Use it to pace a loop.
    void blinkWait(int n, int onMs = 60, int offMs = 60, long delayMs = 0) {
        if (n <= 0) return;
        if (delayMs > 0) blinker.flashBlocking(n, onMs, offMs, delayMs);
        else            blinker.start(n, onMs, offMs);
    }

    // ======================= Simple API (recommended) =======================
    // Boundary-preserving, CRC-checked, self-healing. Just send and recv every
    // loop; both are safe to call when the link is down (send returns 0, recv 0).
    int  send(const uint8_t* b, int len) { return link->sendMsg(b, len) ? len : 0; }
    int  recv(uint8_t* b, int max_len)   { return link->recvMsg(b, max_len); }
    bool ready() const { return link->getState() == State::OK; }

    // Optional: app-stream throughput + lifetime disconnect count.
    //
    // The 2-arg form is unchanged; the 3-arg form adds the lifetime
    // disconnect count -- one per link drop, regardless of cause (bad
    // frame flood, idle watchdog, peer BREAK, LCK timeout). Survives
    // resetStats() and link drops; only zeroed by resetErrors(). This
    // is what you want for longevity testing ("how many bounces did
    // this link survive?"); per-byte error noise is intentionally not
    // counted.
    void getStats(uint64_t& tx, uint64_t& rx) const {
        uint64_t errs;
        link->getStats(tx, rx, errs);
    }
    void getStats(uint64_t& tx, uint64_t& rx, uint64_t& errors) const {
        link->getStats(tx, rx, errors);
    }
    // Zero the tx/rx throughput counters. Does NOT zero the disconnect
    // counter -- use resetErrors() for that, or per-second B/s sampling
    // would wipe the very history that lets you see "errors went up
    // since last sample".
    void resetStats() { link->resetStats(); }
    // Zero the lifetime disconnect counter (e.g. on operator ack).
    void resetErrors() { link->resetErrors(); }

    // ======================= Advanced =======================
    bool isHealthy() const { return hal->isHealthy(); }

    // Raw Stream byte API (unframed/framed bytes, no message boundaries).
    int available() override { return link->available(); }
    int read() override { return link->read(); }
    int peek() override { return link->peek(); }
    size_t write(uint8_t b) override { return link->write(&b, 1); }
    size_t write(const uint8_t *buffer, size_t size) override { return link->write(buffer, (int)size); }
    void flush() override { link->flush(); }
    int read(uint8_t* b, int max_len) { return link->read(b, max_len); }

    // Explicit message verbs (send()/recv() above are the same thing).
    bool sendMsg(const uint8_t* b, int len) { return link->sendMsg(b, len); }
    int  recvMsg(uint8_t* b, int max_len)   { return link->recvMsg(b, max_len); }

    // Manual error control / raw state for custom validation.
    void err() { link->err(); }
    void clearErr() { link->clearErr(); }
    int  getErrCount() const { return link->getErrCount(); }
    State getState() const { return link->getState(); }
};

} // namespace autolink
