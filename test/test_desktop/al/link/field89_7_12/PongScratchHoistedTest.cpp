// AL89 pin 12 / PongScratchHoistedTest. AL-D1: converted from a
// source-grep (checking the string "uint8_t scratch_[..." appears
// in Pong.h and "uint8_t tmp[..." does not) to a real structural
// observation. Pong.h is ARDUINO-only (matching the shape AL-A3
// proved compiles clean on host against the stub set), so this
// links and computes real sizeof() values rather than grepping
// text — a rename of the member (which would fool the old grep)
// changes nothing this test checks; only the actual memory layout
// does.
#define ARDUINO 10607
#include "al/pingpong/Pong.h"
#include <cassert>
#include <cstdio>
#include <iostream>

using namespace autolink;

// Pin 12: two adjacent 5 KB stack-local `tmp` buffers in Pong's
// settle / post-settle branches crushed the loopTask's 8 KB stack
// frame. The fix hoists a single `scratch_` buffer to a Pong
// member instead. A stack-local buffer is invisible to sizeof() —
// it exists only while its function is on the call stack — so if
// scratch_ were reverted back to a stack-local `tmp`, sizeof(Pong)
// would shrink by roughly BUF_SIZE (5120 B) and land close to
// sizeof(PingPongBase) plus a small constant. Toggle off (move
// scratch_ back to a stack-local declaration) -> red.
int main() {
    std::cout << "\n=== Pin 12: Pong scratch buffer hoisted to member ==="
              << std::endl;
    size_t pongSz = sizeof(Pong);
    size_t baseSz = sizeof(PingPongBase);
    size_t bufSize = PingPongBase::BUF_SIZE;
    std::cout << "  sizeof(Pong)=" << pongSz
              << " sizeof(PingPongBase)=" << baseSz
              << " BUF_SIZE=" << bufSize << std::endl;
    // A member costs at least its declared size; allow generous
    // overhead below that (alignment, vtable if any) but the gap
    // must be within shouting distance of BUF_SIZE, not a small
    // constant a stack-local buffer's absence would leave behind.
    size_t delta = pongSz > baseSz ? pongSz - baseSz : 0;
    if (delta < bufSize) {
        std::cerr << "\nFAIL: sizeof(Pong) - sizeof(PingPongBase) = "
                  << delta << " B, less than BUF_SIZE (" << bufSize
                  << " B). Pong no longer carries a "
                     "PingPongBase::BUF_SIZE-sized member beyond its "
                     "base class — scratch_ is likely back to being a "
                     "stack-local declaration inside a member function, "
                     "the exact shape that crushed the loopTask's 8 KB "
                     "stack frame with two adjacent 5 KB buffers."
                  << std::endl;
        assert(false);
    }
    std::cout << "  PASS (sizeof(Pong) exceeds sizeof(PingPongBase) by "
              << delta << " B, >= BUF_SIZE=" << bufSize
              << " — scratch_ is a real member, not stack-local)"
              << std::endl;
    return 0;
}
