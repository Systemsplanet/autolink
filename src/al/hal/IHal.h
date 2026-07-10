
#pragma once
#include <stdint.h>
#ifdef AUTOLINK_HOST_TEST
#    include <cassert>
#endif
#include "al/AutoLinkConfig.h"
#include "al/util/Log.h"

namespace autolink {

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

    virtual void setMode(AutoLinkConfig::Mode) {}

    virtual int tx(const uint8_t *b, int n) = 0;
    virtual void flushTx() = 0;
    // Best-effort drain of bytes already queued for TX, so a link
    // reset doesn't spill pre-BREAK bytes into the next baud.
    virtual void discardTx() {}
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
