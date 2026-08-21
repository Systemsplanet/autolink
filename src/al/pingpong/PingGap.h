
#pragma once
#include <stdint.h>

namespace autolink {

constexpr uint8_t PING_GAP_NO_GAP = 0xFF;

enum class GapAction : uint8_t {

    Stay,

    Enter,

    Update,

    Resume,
};

// gapPending answers "is `currentGap` (or, when entering,
// `lastNak`) still unacked right now" — asked of the link
// directly rather than inferred from lastAckSeq().
//
// Comparing against lastAckSeq() cannot work: it names only the
// most recently acked seq and advances thousands of times a
// second, while Ping::loop samples it at loop rate. Resume fired
// only if the sampler happened to land on the exact instant
// lastAck == currentGap, so in the field it effectively never
// fired (a 23 s stall on seq=189 logged zero "gap resumed"
// lines). A pending predicate is edge-free and cannot be missed.
inline GapAction decideGapTransition(uint8_t currentGap, uint8_t lastNak,
                                     bool gapPending, uint8_t &nextGap) {
    if (currentGap == PING_GAP_NO_GAP) {
        if (lastNak != PING_GAP_NO_GAP && gapPending) {
            nextGap = lastNak;
            return GapAction::Enter;
        }
        nextGap = PING_GAP_NO_GAP;
        return GapAction::Stay;
    }

    if (lastNak != PING_GAP_NO_GAP && lastNak != currentGap) {
        nextGap = lastNak;
        return GapAction::Update;
    }
    if (!gapPending) {
        nextGap = PING_GAP_NO_GAP;
        return GapAction::Resume;
    }
    nextGap = currentGap;
    return GapAction::Stay;
}

} // namespace autolink
