#pragma once
#include <stdint.h>

namespace autolink {

class ALink;

class ILink {
protected:
    ALink* link = nullptr;
public:
    virtual ~ILink() {}
    void bind(ALink* l) { link = l; }
    
    virtual void begin() = 0;
    virtual void setSpd(uint32_t s) = 0;
    virtual void sendBreak() = 0;
    virtual void tx(const uint8_t* b, int n) = 0;
    virtual void flushTx() = 0;
    virtual void startTimer(int ms) = 0;
    virtual void stopTimer() = 0;
    virtual void delayMs(int ms) = 0;
    
    virtual void lock() const = 0;
    virtual void unlock() const = 0;
    
    virtual void pushAppBuf(uint8_t b) = 0;
    virtual void pushAppBuf(const uint8_t* b, int n) = 0;
    virtual int popAppBuf() = 0;
    virtual int popAppBuf(uint8_t* b, int max_len) = 0;
    virtual int peekAppBuf() = 0;
    virtual int appBufAvailable() const = 0;
    virtual void clearAppBuf() = 0;
};

} // namespace autolink
