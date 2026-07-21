// BREAK suspicion state and baud-derived confirm / grace
// helpers. Split out of Link.h to keep the per-file
// 15 KB cap. The member fields (breakSuspectMs_,
// breakSuspectSeen_, breaksSuppressed_) stay in Link
// because they share the link lock with everything
// else; this header owns the per-link constant table
// and the helper functions.
#pragma once
#include "al/link/LinkWire.h"

namespace autolink {

class Link; // fwd

// Public so host tests can compute expected values
// without duplicating the literal.
struct LinkBreakConsts {
    static constexpr uint32_t BREAK_GRACE_MS = 20;
    static constexpr uint32_t BREAK_GRACE_FLOOR_MS = 2;
    static constexpr uint32_t BREAK_GRACE_FRAMES_NEEDED = 2;
    static constexpr uint32_t BREAK_CONFIRM_MS = 150;
    // Coalesce window: two BREAK events within this
    // many ms are treated as the same electrical event
    // (a single glitch surfaces as multiple BREAK /
    // framing-error interrupts at sub-ms spacing on the
    // ESP32 UART driver). Pinned by
    // BreakInterruptCoalesceTest.
    static constexpr uint32_t BREAK_COALESCE_MS = 10;
};

// Baud-derived grace: round-trip at the locked baud,
// with a floor so a 512000 lock still has room for the
// FIRST late-tail frame to be observed.
uint32_t breakGraceMs_unlocked(const Link &l);

// Baud-derived confirm window. Two qualifying frames
// must fit inside the deadline; at 9600 a single
// chunk's flight time exceeds BREAK_CONFIRM_MS and
// the two-frame-clear path is unreachable.
uint32_t breakConfirmMs_unlocked(const Link &l);

} // namespace autolink
