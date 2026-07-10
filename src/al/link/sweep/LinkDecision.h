
#ifndef AUTOLINK_LINK_DECISION_H
#define AUTOLINK_LINK_DECISION_H
#include <stdint.h>
#include <stddef.h>

namespace autolink {

inline int roundTripMs(uint32_t baud) {
    return (int)(2.0 * (5.0 * 10.0 / baud * 1000.0) + 0.5);
}
enum class SwpPhase : uint8_t {
    None = 0,
    Phase1,
    Phase2,
    Phase3,
};

enum class SwpPhaseAction : uint8_t {
    SendPongAck,
    PromoteToPhase2,
    PromoteToPhase3,
    Lock,
    FallbackLockSlowest,
    DropToPhase1,
    Stay,
};

inline SwpPhaseAction decideMasterPhase1Timeout(int, int) {
    return SwpPhaseAction::Stay;
}

inline SwpPhaseAction decideMasterPhase1Ack() {
    return SwpPhaseAction::PromoteToPhase2;
}

inline SwpPhaseAction decideMasterPhase2Ack() {
    return SwpPhaseAction::PromoteToPhase3;
}

inline SwpPhaseAction decideMasterPhase3Ack(int acks, int needed) {
    return (acks >= needed) ? SwpPhaseAction::Lock : SwpPhaseAction::Stay;
}

inline SwpPhaseAction decideMasterPhase2Timeout(int spdI, int baudCount) {
    return (spdI + 1 >= baudCount) ? SwpPhaseAction::FallbackLockSlowest
                                   : SwpPhaseAction::Stay;
}

inline SwpPhaseAction decideMasterPhase3Timeout(int nextBaud, int baudCount) {
    return (nextBaud >= baudCount) ? SwpPhaseAction::FallbackLockSlowest
                                   : SwpPhaseAction::Stay;
}

inline SwpPhaseAction decidePongPhase1Ping() {
    return SwpPhaseAction::PromoteToPhase2;
}

inline SwpPhaseAction decidePongPhase2Ping() {
    return SwpPhaseAction::PromoteToPhase3;
}

inline SwpPhaseAction decidePongPhase3Ack(int acks, int needed) {
    return (acks >= needed) ? SwpPhaseAction::Lock : SwpPhaseAction::Stay;
}

inline SwpPhaseAction decidePongPhase3Ping(int acks, int needed) {
    return (acks >= needed) ? SwpPhaseAction::Lock : SwpPhaseAction::Stay;
}

inline SwpPhaseAction decidePongPhase1Timeout() {
    return SwpPhaseAction::DropToPhase1;
}

inline SwpPhaseAction decidePongPhase2Timeout(int spdI, int) {
    return (spdI < 0) ? SwpPhaseAction::DropToPhase1 : SwpPhaseAction::Stay;
}

inline bool isLockPayload(uint8_t payload, int baudCount, int *outBaudIdx) {
    if (payload < 0x44)
        return false;
    int idx = payload - 0x44;
    if (idx >= baudCount)
        return false;
    if (outBaudIdx)
        *outBaudIdx = idx;
    return true;
}

inline bool isBaudIndexPayload(uint8_t payload, int baudCount,
                               int *outBaudIdx) {
    if (payload >= (uint8_t)baudCount)
        return false;
    if (outBaudIdx)
        *outBaudIdx = payload;
    return true;
}

static constexpr int LD_SEQ_WRAP = 254;

enum class GapClass {
    Forward,
    Stale,
    Gap,
};

inline GapClass classifyGap(uint8_t cobsSeq, uint8_t rxSeq, bool rxSeqSet,
                            int *outDiff = nullptr) {
    if (!rxSeqSet) {
        if (outDiff)
            *outDiff = 0;
        return GapClass::Forward;
    }
    uint8_t expected = (rxSeq == LD_SEQ_WRAP - 1) ? 0 : (uint8_t)(rxSeq + 1);
    if (cobsSeq == expected) {
        if (outDiff)
            *outDiff = 0;
        return GapClass::Forward;
    }
    int diff = (int)cobsSeq - (int)rxSeq;
    if (diff < 0)
        diff += LD_SEQ_WRAP;
    if (outDiff)
        *outDiff = diff;

    return (diff == 0 || diff > LD_SEQ_WRAP / 2) ? GapClass::Stale
                                                 : GapClass::Gap;
}

enum class ArqAction { Hold, Retx, Drop };

inline ArqAction decideArqSlot(uint32_t ageMs, uint8_t retxCount,
                               uint32_t ackRtoMs, uint8_t maxRetx) {
    if (ageMs < ackRtoMs)
        return ArqAction::Hold;
    if (retxCount >= maxRetx)
        return ArqAction::Drop;
    return ArqAction::Retx;
}

// The deleted-replaced sweep helper and its small enum were only
// called by the dropped onTimerSwp_unlocked path that routed the
// sweep arm through EnterLck. The current sweep uses direct
// state-machine branches (decidePongPhase1Timeout /
// decideMasterPhase1Ack / decidePongPhase2Ping /
// decideMasterPhase2Ack / decidePongPhase3Ping /
// decideMasterPhase3Ack) which are still pinned by their own
// tests below.

inline int jitterPhase1Dwell(int baseMs, uint32_t seed) {
    if (baseMs <= 1)
        return baseMs < 1 ? 1 : baseMs;
    int span = baseMs / 6;
    if (span < 1)
        span = 1;
    uint32_t r = seed * 2654435761u + 0x9E3779B9u;
    int off = (int)(r % (uint32_t)(2 * span + 1)) - span;
    int d = baseMs + off;
    return d < 1 ? 1 : d;
}

enum class AppBufAction { Accept, HoldAck };

inline AppBufAction decideAppBuf(int accepted, int incoming) {
    return (accepted < incoming) ? AppBufAction::HoldAck : AppBufAction::Accept;
}

enum class ResetAction { StartAtSlowest };

inline ResetAction decideResetPolicy(bool, int, int) {
    return ResetAction::StartAtSlowest;
}

} // namespace autolink
#endif
