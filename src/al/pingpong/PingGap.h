
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

inline GapAction decideGapTransition(uint8_t currentGap, uint8_t lastNak,
                                     uint8_t lastAck, uint8_t &nextGap) {
    if (currentGap == PING_GAP_NO_GAP) {
        if (lastNak != PING_GAP_NO_GAP && lastAck != lastNak) {
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
    if (lastAck == currentGap) {
        nextGap = PING_GAP_NO_GAP;
        return GapAction::Resume;
    }
    nextGap = currentGap;
    return GapAction::Stay;
}

} // namespace autolink
