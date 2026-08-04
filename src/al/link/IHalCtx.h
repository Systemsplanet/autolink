// HAL passthrough seam: lock, clock, baud, timer.
#pragma once
#include <stdint.h>

namespace autolink {

class IHalCtx {
public:
    virtual ~IHalCtx() = default;
    virtual void hwLock() = 0;
    virtual void hwUnlock() = 0;
    virtual uint32_t hwNowMs() const = 0;
    virtual void hwSetSpd(uint32_t b) = 0;
    virtual void hwStartTimer(int ms) = 0;
};

} // namespace autolink
