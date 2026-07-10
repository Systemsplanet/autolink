
#pragma once
#include "al/link/ISweepCtx.h"
#include "al/link/LinkWire.h"
#include <stdint.h>

namespace autolink {

enum class SweepPhase : uint8_t { NONE = 0, PHASE1, PHASE2, PHASE3 };

struct SweepDwells {
    int phase1;
    int phase2[16];
    int phase2Slave[16];
    int phase3;
    int phase2Total;
};

class LinkSweep {
public:
    LinkSweep() = default;

    SweepPhase phase() const { return phase_; }
    int phase3Baud() const { return phase3Baud_; }
    int phase3Acks() const { return phase3Acks_; }

    void setPhase(SweepPhase p) { phase_ = p; }
    void reset() {
        phase_ = SweepPhase::NONE;
        phase3Baud_ = -1;
        phase3Acks_ = 0;
    }
    void incPhase3Acks() { phase3Acks_++; }

    void enterPhase1(ISweepCtx &ctx);
    void enterPhase2(ISweepCtx &ctx);
    void enterPhase3(ISweepCtx &ctx, int chosenBaud);

    void computeDwells(ISweepCtx &ctx);

    int phase1ArmMs(ISweepCtx &ctx);

    const SweepDwells &dwells() const { return dwells_; }
    SweepDwells &dwells() { return dwells_; }

private:
    SweepPhase phase_ = SweepPhase::NONE;
    int phase3Baud_ = -1;
    int phase3Acks_ = 0;
    SweepDwells dwells_{};
};

} // namespace autolink
