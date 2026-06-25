// Baud-sweep phase machine: PHASE1 slowest,
// PHASE2 top-down, PHASE3 2-of-3 confirmation.
// Owns sweepPhase_, phase3Baud_, phase3Acks_;
// Link owns the I/O surface (setSpd, sendFrame,
// startTimer, lockOk).
#pragma once
#include <stdint.h>

namespace autolink {
class Link;

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
    // Link for setSpd/sendFrame/startTimer
    // and call Link::lockOk on the way to OK.
    void enterPhase1(Link &l);
    void enterPhase2(Link &l);
    void enterPhase3(Link &l, int chosenBaud);
    void enterResweep(Link &l);

    // Dwell table recompute from current cfg
    // (called once on Link::begin).
    void computeDwells(Link &l);

    // Phase-1 jitter: ARDUINO uses the seed;
    // host tests use the raw dwell.
    int phase1ArmMs(Link &l);

    const SweepDwells &dwells() const { return dwells_; }
    SweepDwells &dwells() { return dwells_; }

private:
    SweepPhase phase_ = SweepPhase::NONE;
    int phase3Baud_ = -1;
    int phase3Acks_ = 0;
    SweepDwells dwells_{};
};

} // namespace autolink