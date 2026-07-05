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
#ifdef AUTOLINK_HOST_TEST
#    include <cassert>
#endif
#include "al/AutoLinkConfig.h"
#include "al/util/Log.h"

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
    // host tests assert on a second setEvents
    // so a future refactor that constructs two
    // Links against the same HAL trips here
    // before it ships; on-device we log and
    // ignore, since double-bind is recoverable
    // (the new owner still wins) and a panic
    // would brick a live link for a logical
    // misuse. The guard makes the contract
    // loud at compile/test time and forgiving
    // in production.
    void setEvents(ILinkEvents &e) {
#ifdef AUTOLINK_HOST_TEST
        assert(events_ == nullptr && "setEvents called twice on the same HAL");
#endif
        if (events_ != nullptr) {
            Log::log().error("IHal",
                             "setEvents called twice — "
                             "rebinding listener");
        }
        events_ = &e;
    }
    ILinkEvents *events() const { return events_; }

    virtual bool isHealthy() const { return true; }

    virtual void begin() = 0;
    virtual void setSpd(uint32_t s) = 0;
    virtual void sendBreak() = 0;

    // Mode reflects the link's cfg.mode so the HAL
    // can size UART / stream buffers at begin() and
    // keep its log line honest across a NVS-restored
    // ASYNC. The default is a no-op; EspHal is the
    // only impl that needs mode awareness. The link
    // layer's setMode still mutates cfg.mode; this
    // gives the HAL a copy of the same field, so a
    // NVS+reboot restore path that flips cfg.mode
    // before begin() also reaches the HAL before
    // begin() runs. Pre-this-release EspHal held a
    // stale cfg copy and sized for SYNC even when
    // Link was running ASYNC — the wire contract
    // is unchanged, but the buffer shape followed
    // the HAL's field, not Link's.
    virtual void setMode(AutoLinkConfig::Mode) {}

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
