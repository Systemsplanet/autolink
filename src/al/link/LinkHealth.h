
#ifndef AUTOLINK_LINK_HEALTH_H
#define AUTOLINK_LINK_HEALTH_H
#include <stdint.h>

namespace autolink {

struct HealthState {
    uint32_t rejFirstMs = 0;
    uint32_t rejLastMs = 0;
    uint32_t lastRxMs = 0;
    uint32_t lastTxMs = 0;

    // ARQ RTO; watchdogs wait out 2 x RTO before treating one-sided
    // silence as a gone peer (retx needs a chance to draw a reply).
    uint32_t rtoMs = 0;
    int pending = 0;
    uint64_t frameErrs = 0;
    bool poolFull = false;
    bool sync = false;
};

enum class HealthAction : uint8_t {
    Keep,
    DropTxStall,
    DropAsymIdle,
    DropIdle,
    DropPoolExhaust,
    DropDeadLink,
};

inline constexpr int healthFastIdleRxMs() { return 300; }
inline constexpr int healthFastIdleTxMs() { return 1000; }

inline HealthAction decideHealth(const HealthState &h, uint32_t now,
                                 int idleTimeoutMs) {
    uint32_t idle = (uint32_t)idleTimeoutMs;
    if (h.rejFirstMs != 0 && (uint32_t)(now - h.rejFirstMs) > idle &&
        (uint32_t)(now - h.rejLastMs) <= idle)
        return HealthAction::DropTxStall;
    uint32_t rxAge = now - h.lastRxMs;
    uint32_t txAge = now - h.lastTxMs;
    // Dead-link backstop. BOTH modes: a link that's been
    // quiet in both directions past idleTimeoutMs AND has
    // an actual in-flight message is dead, not just idle.
    // Narrower than DropIdle (pending>0 only — no frameErrs
    // alternative) so a legitimately-idle link with no
    // outstanding traffic still passes. This is the
    // SYNC-mode backstop the bench log showed was missing:
    // the SYNC retx ladder exhausts in ~maxRetx*RTO (2.5s
    // default), and a wedged peer leaves us with a
    // pending op and no reply past the idle window. For
    // ASYNC this is reached only when DropIdle didn't
    // match — which only happens when pending=0 (the
    // DropIdle case), so this is a no-op for ASYNC and
    // runs only for SYNC in practice.
    if (h.pending > 0 && rxAge > idle && txAge > idle)
        return HealthAction::DropDeadLink;
    if (h.sync)
        return HealthAction::Keep;

    if (rxAge > (uint32_t)healthFastIdleRxMs() && rxAge > 2 * h.rtoMs &&
        txAge < (uint32_t)healthFastIdleTxMs() && h.pending > 0)
        return HealthAction::DropAsymIdle;
    if (rxAge > idle && txAge > idle && (h.pending > 0 || h.frameErrs > 0))
        return HealthAction::DropIdle;

    // Pool full + live RX is routine flood backpressure; drop only
    // when RX is silent past the repair horizon.
    if (h.poolFull && h.pending > 0 && rxAge > (uint32_t)healthFastIdleRxMs() &&
        rxAge > 2 * h.rtoMs)
        return HealthAction::DropPoolExhaust;
    return HealthAction::Keep;
}

} // namespace autolink
#endif
