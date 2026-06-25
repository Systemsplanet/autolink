// Baud-sweep phase machine: PHASE1 slowest,
// PHASE2 top-down, PHASE3 2-of-3 confirmation.
// Owns sweepPhase_, phase3Baud_, phase3Acks_;
// Link owns the I/O surface (setSpd, sendFrame,
// startTimer, lockOk).
#pragma once
#include "al/link/LinkContext.h"
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

    // Phase enter/transition. All four are
    // callable under the link lock; they ask
    // the context for setSpd/sendFrame/
    // startTimer and call Link::lockOk on the
    // way to OK. LinkContext (not Link) keeps
    // this helper out of Link's friend list.
    void enterPhase1(LinkContext &ctx);
    void enterPhase2(LinkContext &ctx);
    void enterPhase3(LinkContext &ctx, int chosenBaud);

    // Dwell table recompute from current cfg
    // (called once on Link::begin).
    void computeDwells(LinkContext &ctx);

    // Phase-1 jitter: ARDUINO uses the seed;
    // host tests use the raw dwell.
    int phase1ArmMs(LinkContext &ctx);

    const SweepDwells &dwells() const { return dwells_; }
    SweepDwells &dwells() { return dwells_; }

private:
    SweepPhase phase_ = SweepPhase::NONE;
    int phase3Baud_ = -1;
    int phase3Acks_ = 0;
    SweepDwells dwells_{};
};

} // namespace autolink