// What the baud-sweep state machine needs from its host.
#pragma once
#include "al/link/IHalCtx.h"

namespace autolink {

class ISweepCtx : public IHalCtx {
public:
    virtual void sendFrame(uint8_t payload) = 0;
    virtual void sendSweepFrame(uint8_t payload) = 0;
    virtual bool masterRole() const = 0;
    virtual int currentSpdI() const = 0;
    virtual void setCurrentSpdI(int i) = 0;
    virtual int allowedBaudsCount() const = 0;
    virtual uint32_t allowedBaud(int i) const = 0;
    virtual int delayMs() const = 0;
    // Reset the per-sweep-round PONG tally. Called at
    // every P1 entry so the master's p2/p3 fallback
    // paths can demand at least one PONG before
    // declaring OK. Pinned by FallbackRequiresPongTest.
    virtual void resetSweepPongCount() = 0;
};

} // namespace autolink
