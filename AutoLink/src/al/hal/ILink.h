// ILink — hardware seam between protocol core (ALink) and the physical layer.
// EspHal implements it on ESP32; MockHal implements it in the host test suite.
#pragma once
#include <stdint.h>

namespace autolink {

class ALink;

class ILink {
public:
    ALink* link = nullptr;

    virtual ~ILink() {}
    void bind(ALink* l) { link = l; }

    // v5.1.38: health check (WiFi connection alive, etc.). Default
    // returns true so non-network HALs (MockHal, host stub) need
    // no override. EspHal overrides to return its `healthy` flag,
    // which is set by the WiFi connection callbacks.
    virtual bool isHealthy() const { return true; }

    virtual void begin() = 0;
    virtual void setSpd(uint32_t s) = 0;
    virtual void sendBreak() = 0;
    // Returns bytes accepted by the driver. Short return = TX ring full,
    // caller MUST treat as link error (partial frame is unrecoverable).
    virtual int tx(const uint8_t* b, int n) = 0;
    virtual void flushTx() = 0;
    virtual void startTimer(int ms) = 0;
    virtual void stopTimer() = 0;
    virtual void delayMs(int ms) = 0;
    virtual uint32_t nowMs() = 0;   // monotonic ms; injectable for host tests

    virtual void lock() const = 0;
    virtual void unlock() const = 0;

    virtual int  pushAppBuf(const uint8_t* b, int n) = 0;  // bytes accepted
    virtual int popAppBuf(uint8_t* b, int max_len) = 0;
    virtual int peekAppBuf() const = 0;
    virtual int appBufAvailable() const = 0;
    // Peek N bytes at offset without consuming. Used by the message-layer
    // resync scan to look ahead in the buffer.
    virtual int peekAt(uint8_t* out, int n, int offset) const = 0;
    virtual void clearAppBuf() = 0;
    // Drop bytes in the driver ring but not yet pushed to the app buffer.
    // Prevents the UART event task from refilling the stream buffer with
    // stale bytes after a flushRx().
    virtual void flushRxHw() {}
};

} // namespace autolink
