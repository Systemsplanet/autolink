#pragma once
#include "EspHal.h"
#include "ALink.h"
#include "Log.h"
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

class AutoLink : public Stream {
private:
    std::unique_ptr<EspHal> hal;
    std::unique_ptr<ALink> link;
    int ledPin;

public:
    // Construct on the stack as a global — no new/pointer needed. Everything past
    // the role flag is optional; sane defaults cover the common case.
    AutoLink(uart_port_t u_num, int rx_pin, int tx_pin, bool isMasterNode, AutoLinkConfig cfg = AutoLinkConfig())
    {
        // Auto-size the reassembly buffer so a whole message always fits. The user
        // never has to reason about the maxMsg/streamBufferSize relationship.
        size_t need = cfg.maxMsg + MSG_HDR + 64;
        if (cfg.streamBufferSize < need) cfg.streamBufferSize = need;

        ledPin = cfg.ledPin;
        pinMode(ledPin, OUTPUT);
        digitalWrite(ledPin, LOW);
        hal = std::make_unique<EspHal>(u_num, rx_pin, tx_pin, cfg);
        link = std::make_unique<ALink>(*hal, isMasterNode, cfg);
    }

    void begin() {
        hal->begin();
    }

    // Blink the status LED n times (blocking, active-high). Handy for marking a
    // bring-up phase and for a one-blink-per-packet heartbeat. Tune the on/off
    // milliseconds if you want shorter/longer flashes. If delayMs > 0, pause that
    // long after the last flash before returning (e.g. to pace a packet loop).
    void blink(int n, int onMs = 60, int offMs = 60, long delayMs = 0) {
        for (int i = 0; i < n; i++) {
            digitalWrite(ledPin, HIGH); delay(onMs);
            digitalWrite(ledPin, LOW);
            if (i < n - 1) delay(offMs);
        }
        if (delayMs > 0) delay(delayMs);
    }

    // ======================= Simple API (recommended) =======================
    // Boundary-preserving, CRC-checked, self-healing. Just send and recv every
    // loop; both are safe to call when the link is down (send returns 0, recv 0).
    int  send(const uint8_t* b, int len) { return link->sendMsg(b, len) ? len : 0; }
    int  recv(uint8_t* b, int max_len)   { return link->recvMsg(b, max_len); }
    bool ready() const { return link->getState() == State::OK; }

    // Optional: app-stream throughput since the last reset.
    void getStats(uint64_t& tx, uint64_t& rx) const { link->getStats(tx, rx); }
    void resetStats() { link->resetStats(); }

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
