// BREAK helper implementations. Split out of Link.h.
// The helpers read cfg / spdI / roundTripMs through the
// Link accessors, which is the single source of truth
// for those values.
#include "al/link/timers/LinkBreak.h"
#include "al/link/Link.h"

namespace autolink {

// Baud-derived grace: round-trip at the locked baud,
// with a floor so a 512000 lock still has room for the
// FIRST late-tail frame to be observed.
uint32_t breakGraceMs_unlocked(const Link &l) {
    if (l.spdIAcc() < 0 || l.spdIAcc() >= l.cfgAcc().allowedBaudsCount)
        return LinkBreakConsts::BREAK_GRACE_MS;
    uint32_t b = l.cfgAcc().allowedBaudSafe(l.spdIAcc());
    int rt = roundTripMs(b);
    if (rt < (int)LinkBreakConsts::BREAK_GRACE_FLOOR_MS)
        rt = (int)LinkBreakConsts::BREAK_GRACE_FLOOR_MS;
    return (uint32_t)rt;
}

// Baud-derived confirm window. Two qualifying frames
// must fit inside the deadline; at 9600 a single
// chunk's flight time exceeds BREAK_CONFIRM_MS and
// the two-frame-clear path is unreachable.
uint32_t breakConfirmMs_unlocked(const Link &l) {
    if (l.spdIAcc() < 0 || l.spdIAcc() >= l.cfgAcc().allowedBaudsCount)
        return LinkBreakConsts::BREAK_CONFIRM_MS;
    uint32_t baud = l.cfgAcc().allowedBaudSafe(l.spdIAcc());
    if (baud == 0)
        return LinkBreakConsts::BREAK_CONFIRM_MS;
    // Two qualifying frames (MAX_CHUNK + MSG_HDR
    // bytes each) must fit inside the window.
    uint32_t twoChunkFlight =
        (uint32_t)(MAX_CHUNK + MSG_HDR) * 20u * 1000u / baud;
    return twoChunkFlight > LinkBreakConsts::BREAK_CONFIRM_MS
        ? twoChunkFlight
        : LinkBreakConsts::BREAK_CONFIRM_MS;
}

} // namespace autolink
