// Sweep-phase state machine as pure functions
// returning action enums. Side effects (counter bumps,
// state mutation, log calls) live in the caller.
// Truth-table testable with no I/O.
#ifndef AUTOLINK_LINK_DECISION_H
#define AUTOLINK_LINK_DECISION_H

#include <stdint.h>
#include <stddef.h>

namespace autolink
{
enum class SwpPhase : uint8_t {
    None = 0,
    Phase1 = 1,
    Phase2 = 2,
    Phase3 = 3,
};

enum class SwpPhaseAction : uint8_t {
    SendPongAck,
    PromoteToPhase3,
    Lock,
    FallbackLockSlowest,
    DropToPhase1,
    Stay,
};

inline SwpPhaseAction decideMasterPhase1Timeout(int,
                                                int)
{
    return SwpPhaseAction::Stay;
}

inline SwpPhaseAction decideMasterPhase1Ack()
{
    return SwpPhaseAction::Lock;
}

inline SwpPhaseAction decideMasterPhase2Ack()
{
    return SwpPhaseAction::PromoteToPhase3;
}

inline SwpPhaseAction
decideMasterPhase3Ack(int phase3Acks, int phase3Needed)
{
    if (phase3Acks >= phase3Needed)
        return SwpPhaseAction::Lock;
    return SwpPhaseAction::Stay;
}

inline SwpPhaseAction
decideMasterPhase2Timeout(int spdI, int baudCount)
{
    if (spdI + 1 >= baudCount)
        return SwpPhaseAction::FallbackLockSlowest;
    return SwpPhaseAction::Stay;
}

inline SwpPhaseAction
decideMasterPhase3Timeout(int nextBaud, int baudCount)
{
    if (nextBaud >= baudCount)
        return SwpPhaseAction::FallbackLockSlowest;
    return SwpPhaseAction::Stay;
}

inline SwpPhaseAction decidePongPhase1Ping()
{
    return SwpPhaseAction::SendPongAck;
}

inline SwpPhaseAction decidePongPhase2Ping()
{
    return SwpPhaseAction::PromoteToPhase3;
}

inline SwpPhaseAction
decidePongPhase3Ack(int phase3Acks, int phase3Needed)
{
    if (phase3Acks >= phase3Needed)
        return SwpPhaseAction::Lock;
    return SwpPhaseAction::Stay;
}

inline SwpPhaseAction decidePongPhase1Timeout()
{
    return SwpPhaseAction::DropToPhase1;
}

inline SwpPhaseAction
decidePongPhase2Timeout(int spdI, int baudCount)
{
    (void)baudCount;
    if (spdI - 1 < 0)
        return SwpPhaseAction::DropToPhase1;
    return SwpPhaseAction::Stay;
}

// Decode a baud-index payload for the LOCK frame
// (offset 0x44). Raw byte = baud index; valid iff <
// baudCount.
inline bool isLockPayload(uint8_t payload,
                          int baudCount,
                          int *outBaudIdx)
{
    if (payload < 0x44)
        return false;
    int idx = payload - 0x44;
    if (idx >= baudCount)
        return false;
    if (outBaudIdx)
        *outBaudIdx = idx;
    return true;
}

// Decode a baud-index payload for the legacy REQ path.
// Raw byte = baud index; valid iff < baudCount.
inline bool isBaudIndexPayload(uint8_t payload,
                               int baudCount,
                               int *outBaudIdx)
{
    if (payload >= (uint8_t)baudCount)
        return false;
    if (outBaudIdx)
        *outBaudIdx = payload;
    return true;
}

// Modulus for gap-classification math. The data seq
// space is 254 (0..253); 0xFE/0xFF are reserved as
// wire discriminators (NAK/ACK).
static constexpr int LD_SEQ_WRAP = 254;

enum class GapClass {
    Forward,
    Stale,
    Gap,
};

inline GapClass classifyGap(uint8_t cobsSeq,
                            uint8_t rxSeq,
                            bool rxSeqSet,
                            int *outDiff = nullptr)
{
    if (!rxSeqSet) {
        if (outDiff)
            *outDiff = 0;
        return GapClass::Forward;
    }
    uint8_t expected = (rxSeq == LD_SEQ_WRAP - 1)
        ? 0
        : (uint8_t)(rxSeq + 1);
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
    if (diff == 0 || diff > LD_SEQ_WRAP / 2) {
        return GapClass::Stale;
    }
    return GapClass::Gap;
}

enum class ArqAction {
    Hold,
    Retx,
    Drop,
};

inline ArqAction decideArqSlot(uint32_t ageMs,
                               uint8_t retxCount,
                               uint32_t ackRtoMs,
                               uint8_t maxRetx)
{
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

inline SwpAction decideSwpTick(int spdI, int baudCount,
                               int pingSample,
                               int samplesPerBaud,
                               bool lckExhausted)
{
    if (lckExhausted)
        return SwpAction::RestartSweep;
    if (spdI >= baudCount)
        return SwpAction::EnterLck;
    if (pingSample + 1 >= samplesPerBaud)
        return SwpAction::SendPingAdvance;
    return SwpAction::SendPingSame;
}

inline int jitterPhase1Dwell(int baseMs, uint32_t seed)
{
    if (baseMs <= 1)
        return baseMs < 1 ? 1 : baseMs;
    int span = baseMs / 6;
    if (span < 1)
        span = 1;
    uint32_t r = seed * 2654435761u + 0x9E3779B9u;
    int off =
        (int)(r % (uint32_t)(2 * span + 1)) - span;
    int d = baseMs + off;
    return d < 1 ? 1 : d;
}

enum class LckAction {
    SendReq,
    DropAndResweep,
};

inline LckAction decideLckTick(int lckRetries,
                               int maxRetries)
{
    if (lckRetries > maxRetries)
        return LckAction::DropAndResweep;
    return LckAction::SendReq;
}

enum class IdleAction {
    Hold,
    Drop,
};

inline IdleAction decideIdleWatchdog(uint32_t rxAgeMs,
                                     uint32_t txAgeMs,
                                     int idleTimeoutMs)
{
    if (idleTimeoutMs <= 0)
        return IdleAction::Hold;
    if (rxAgeMs > (uint32_t)idleTimeoutMs &&
        txAgeMs > (uint32_t)idleTimeoutMs)
        return IdleAction::Drop;
    return IdleAction::Hold;
}

enum class KeepaliveAction {
    Hold,
    Emit,
};

inline KeepaliveAction
decideKeepalive(uint32_t txAgeMs, int idleTimeoutMs,
                bool linkPaused)
{
    if (linkPaused)
        return KeepaliveAction::Hold;
    if (idleTimeoutMs <= 0)
        return KeepaliveAction::Hold;
    if (txAgeMs >= (uint32_t)(idleTimeoutMs / 3))
        return KeepaliveAction::Emit;
    return KeepaliveAction::Hold;
}

enum class AppBufAction {
    Accept,
    HoldAck,
};

inline AppBufAction decideAppBuf(int accepted,
                                 int incoming)
{
    return (accepted < incoming)
        ? AppBufAction::HoldAck
        : AppBufAction::Accept;
}

enum class ResetAction {
    StartAtSlowest,
};

// Per directive: any break or line error restarts in
// PHASE 1 at the slowest baud, ignoring preferredBaud_
// and retry count.
inline ResetAction decideResetPolicy(bool, int, int)
{
    return ResetAction::StartAtSlowest;
}

} // namespace autolink

#endif
