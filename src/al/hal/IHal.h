// Hardware abstraction boundary. ESP-IDF: EspHal
// implements it on top of UART + FreeRTOS stream
// buffer. Host tests: MockHal implements it on top of
// two memory pipes + a simulated clock. Link talks to
// IHal only — never to ESP-IDF directly.
#pragma once
#include <stdint.h>

namespace autolink
{
class Link;

class IHal
{
public:
    Link *link = nullptr;

    virtual ~IHal() {}
    void bind(Link *l) { link = l; }


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

    virtual void lock() const = 0;
    virtual void unlock() const = 0;

    virtual int pushAppBuf(const uint8_t *b,
                           int n) = 0;
    virtual int popAppBuf(uint8_t *b, int max_len) = 0;
    virtual int peekAppBuf() const = 0;
    virtual int appBufAvailable() const = 0;


    virtual int peekAt(uint8_t *out, int n,
                       int offset) const = 0;
    virtual void clearAppBuf() = 0;


    virtual void flushRxHw() {}
};

} // namespace autolink
