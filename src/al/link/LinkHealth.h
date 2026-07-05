// Link keep/drop decisions as one pure function.
// No side-effects; truth-table testable. Every OK-state
// watchdog (tx-backpressure stall, asymmetric idle,
// symmetric idle, ARQ pool exhaustion) funnels through
// decideHealth so a check can't be silently mode-gated
// out — the bug shape behind three prior wedges.
#ifndef AUTOLINK_LINK_HEALTH_H
#define AUTOLINK_LINK_HEALTH_H
#include <stdint.h>

namespace autolink {

// Sender side of the wire. rejFirstMs/rejLastMs are the
// tx-reject streak (0 = no streak); pending/poolFull come
// from the ARQ; sync selects the mode-specific rows.
struct HealthState {
    uint32_t rejFirstMs = 0;
    uint32_t rejLastMs = 0;
    uint32_t lastRxMs = 0;
    uint32_t lastTxMs = 0;
    int pending = 0;
    uint64_t frameErrs = 0;
    bool poolFull = false;
    bool sync = false;
};

enum class HealthAction : uint8_t {
    Keep,
    DropTxStall,     // reject streak outlived idleTimeoutMs and is still live
    DropAsymIdle,    // TX pending, RX silent -> peer gone (ASYNC only)
    DropIdle,        // fully quiet past idleTimeoutMs with pending/errs (ASYNC only)
    DropPoolExhaust, // ARQ pool full with pending -> receiver not draining (ASYNC only)
};

// Asymmetric idle thresholds: RX silent past rxMs while a
// send happened within txMs and an ACK is outstanding.
inline constexpr int healthFastIdleRxMs() { return 300; }
inline constexpr int healthFastIdleTxMs() { return 1000; }

// Priority order matches the drop severity: a stalled
// sender first (both modes), then the ASYNC-only peer-gone
// and pool checks. Callers pass idleTimeoutMs > 0.
inline HealthAction decideHealth(const HealthState &h, uint32_t now,
                                 int idleTimeoutMs) {
    uint32_t idle = (uint32_t)idleTimeoutMs;
    if (h.rejFirstMs != 0 && (uint32_t)(now - h.rejFirstMs) > idle &&
        (uint32_t)(now - h.rejLastMs) <= idle)
        return HealthAction::DropTxStall;
    if (h.sync)
        return HealthAction::Keep;
    uint32_t rxAge = now - h.lastRxMs;
    uint32_t txAge = now - h.lastTxMs;
    if (rxAge > (uint32_t)healthFastIdleRxMs() &&
        txAge < (uint32_t)healthFastIdleTxMs() && h.pending > 0)
        return HealthAction::DropAsymIdle;
    if (rxAge > idle && txAge > idle && (h.pending > 0 || h.frameErrs > 0))
        return HealthAction::DropIdle;
    if (h.poolFull && h.pending > 0)
        return HealthAction::DropPoolExhaust;
    return HealthAction::Keep;
}

} // namespace autolink
#endif
