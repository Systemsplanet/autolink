// Sweep state-machine as pure functions.
// No side-effects; truth-table testable.
#ifndef AUTOLINK_LINK_DECISION_H
#define AUTOLINK_LINK_DECISION_H
#include <stdint.h>
#include <stddef.h>

namespace autolink {

// Round-trip dwell formula shared by
// LinkSweep's computeDwells / enterPhase3
// and Link::handleSwp_unlocked's master
// P3 ACK rearm. 5 chars * 10 bits/char
// (start + 8 data + stop) over the link
// baud, doubled for the round trip, plus
// a half-ms slop. Pure function — no
// state, no side effects, no I/O.
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
    // Caller has already decremented spdI.
    // Drop only if we walked past the start
    // of the list (post-decrement < 0).
    return (spdI < 0) ? SwpPhaseAction::DropToPhase1 : SwpPhaseAction::Stay;
}

// LOCK payload: raw byte = baud_index + 0x44.
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

// REQ payload: raw byte = baud index.
inline bool isBaudIndexPayload(uint8_t payload, int baudCount,
                               int *outBaudIdx) {
    if (payload >= (uint8_t)baudCount)
        return false;
    if (outBaudIdx)
        *outBaudIdx = payload;
    return true;
}

// Seq space 0..253; 0xFE/0xFF reserved
// as wire discriminators (NAK/ACK).
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
    // diff > WRAP/2 means seq went backward.
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

enum class SwpAction {
    SendPingSame,
    SendPingAdvance,
    EnterLck,
    RestartSweep,
};

inline SwpAction decideSwpTick(int spdI, int baudCount, int pingSample,
                               int samplesPerBaud, bool lckExhausted) {
    if (lckExhausted)
        return SwpAction::RestartSweep;
    if (spdI >= baudCount)
        return SwpAction::EnterLck;
    if (pingSample + 1 >= samplesPerBaud)
        return SwpAction::SendPingAdvance;
    return SwpAction::SendPingSame;
}

// Jitter P1 dwell ±1/6 of base to reduce
// master/pong PING collision.
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

enum class LckAction {
    SendReq,
    DropAndResweep,
};

inline LckAction decideLckTick(int lckRetries, int maxRetries) {
    return (lckRetries > maxRetries) ? LckAction::DropAndResweep
                                     : LckAction::SendReq;
}

enum class AppBufAction { Accept, HoldAck };

inline AppBufAction decideAppBuf(int accepted, int incoming) {
    return (accepted < incoming) ? AppBufAction::HoldAck : AppBufAction::Accept;
}

enum class ResetAction { StartAtSlowest };

// Any break/line-error always restarts
// at P1 slowest baud; preferredBaud_ ignored.
inline ResetAction decideResetPolicy(bool, int, int) {
    return ResetAction::StartAtSlowest;
}

} // namespace autolink
#endif
