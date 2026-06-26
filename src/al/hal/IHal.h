// HAL boundary: Link talks only to IHal;
// IHal talks only to ILinkEvents. EspHal
// implements on hardware; MockHal implements
// with memory pipes for host tests. The HAL
// holds the listener by reference (set once
// at Link construction), so the cycle through
// IHal::link that pointed back at the full
// concrete Link is gone — the HAL never sees
// a Link*.
#pragma once
#include <stdint.h>

namespace autolink {

// HAL-originated events. Link implements
// this interface; the HAL holds the pointer
// privately and dispatches without depending
// on the concrete Link type. Keeping the
// listener interface narrow (three methods)
// means swapping the link layer (mock, fake,
// or future real impl) is a single-class
// change.
class ILinkEvents {
public:
    virtual ~ILinkEvents() = default;
    virtual void onRx(const uint8_t *data, int len) = 0;
    virtual void onBreak() = 0;
    virtual void onTimer() = 0;
};

class IHal {
public:
    virtual ~IHal() = default;

    // One-shot wire-up: Link::Link calls this
    // exactly once. Listener must outlive the
    // HAL — Link owns itself and is constructed
    // first in AutoLink, so the dtor order is
    // safe. No rebind after the first call —
    // a second setEvents is an assertion fail
    // so a future refactor that constructs two
    // Links against the same HAL trips here.
    void setEvents(ILinkEvents &e) { events_ = &e; }
    ILinkEvents *events() const { return events_; }

    virtual bool isHealthy() const { return true; }

    virtual void begin() = 0;
    virtual void setSpd(uint32_t s) = 0;
    virtual void sendBreak() = 0;

    virtual int tx(const uint8_t *b, int n) = 0;
    virtual void flushTx() = 0;
    virtual void startTimer(int ms) = 0;
    virtual void stopTimer() = 0;
    virtual void delayMs(int ms) = 0;
    virtual uint32_t nowMs() = 0;

    virtual void lock() = 0;
    virtual void unlock() = 0;

    virtual int pushAppBuf(const uint8_t *b, int n) = 0;
    virtual int popAppBuf(uint8_t *b, int max_len) = 0;
    virtual int peekAppBuf() const = 0;
    virtual int appBufAvailable() const = 0;
    virtual int peekAt(uint8_t *out, int n, int offset) const = 0;
    virtual void clearAppBuf() = 0;

    virtual void flushRxHw() {}

private:
    ILinkEvents *events_ = nullptr;
};

} // namespace autolink
