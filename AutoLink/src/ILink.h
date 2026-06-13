// ILink.h — hardware abstraction interface between the protocol core (ALink)
// and the physical layer. EspHal implements it on the ESP32; MockHal
// implements it in the host test suite, which is what makes the protocol
// natively testable without hardware.
#pragma once
#include <stdint.h>

namespace autolink {

class ALink;

// ----------------------------------------------------------------------------
// ILink — the hardware seam between the protocol core (ALink) and the world:
// UART TX/break/baud, the sweep timer, a monotonic clock, a mutex, and the
// app-side byte buffer. EspHal implements it on the ESP32; MockHal implements
// it in the host test suite, which is what makes the protocol natively
// testable.
// ----------------------------------------------------------------------------
class ILink {
public:
    ALink* link = nullptr;

    virtual ~ILink() {}
    void bind(ALink* l) { link = l; }

    virtual void begin() = 0;
    virtual void setSpd(uint32_t s) = 0;
    virtual void sendBreak() = 0;
    // Write bytes to the UART TX ring. Returns the number of bytes actually
    // accepted by the driver. A short return means the TX ring was full and
    // bytes were dropped — the caller MUST treat this as a link error because
    // the partial frame cannot be recovered by the receiver.
    virtual int tx(const uint8_t* b, int n) = 0;
    virtual void flushTx() = 0;
    virtual void startTimer(int ms) = 0;
    virtual void stopTimer() = 0;
    virtual void delayMs(int ms) = 0;
    virtual uint32_t nowMs() = 0;   // monotonic ms; injectable for host tests

    virtual void lock() const = 0;
    virtual void unlock() const = 0;

    virtual void pushAppBuf(uint8_t b) = 0;
    virtual int  pushAppBuf(const uint8_t* b, int n) = 0;  // returns bytes accepted
    virtual int popAppBuf() = 0;
    virtual int popAppBuf(uint8_t* b, int max_len) = 0;
    virtual int peekAppBuf() = 0;
    virtual int appBufAvailable() const = 0;
    virtual void clearAppBuf() = 0;
    // Flush the hardware receive buffer (UART driver ring, DMA buffer, etc.)
    // so bytes already received but not yet pushed to the app buffer are
    // discarded. No-op on host (MockHal). Called from ALink::flushRx() after
    // clearAppBuf() to prevent the UART event task from immediately refilling
    // the stream buffer with stale bytes from the driver ring.
    virtual void flushRxHw() {}
};

} // namespace autolink
