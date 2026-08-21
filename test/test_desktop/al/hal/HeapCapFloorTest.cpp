// Pins the Open-1 heap-headroom cap: capFloorByHeap()
// must shrink an ASYNC buffer ask that would eat into
// cfg.heapReserveBytes, must grant the full ask when
// heap is plentiful, unknown (0, host stub), or the
// cap is disabled (reserve=0), and must report 0
// (not minFloor) when the heap-stripped headroom
// can't even afford minFloor — the J1 contract. The
// caller (Link::begin()'s gate) needs to see the
// shortfall to fail loudly; the pre-J1 shape
// returned minFloor unconditionally, which made the
// gate unreachable on a starved heap. Toggle-off
// (return want unconditionally) turns the
// tight-heap rows red.
#ifndef ARDUINO

#    include <cassert>
#    include <iostream>
#    include "al/AutoLinkConfig.h"

using namespace autolink;

int main() {
    struct Row {
        size_t want, minFloor, freeHeap, reserve, expect;
        const char *why;
    };
    const Row rows[] = {
        { 10252, 5126, 40000, 16384, 10252, "plenty: full grant" },
        { 10252, 5126, 26636, 16384, 10252, "exact fit: full grant" },
        { 10252, 5126, 26635, 16384, 10251, "1 short: shrink by 1" },
        { 10252, 5126, 20000, 16384, 0, "tight: avail < minFloor -> 0 (J1)" },
        // J1: the next 3 rows
        // report 0 (not
        // minFloor) when
        // heap-stripped
        // headroom can't
        // afford minFloor.
        // The Link::begin()
        // gate needs to see
        // the shortfall.
        { 10252, 5126, 16384, 16384, 0, "reserve-only heap: 0 (J1)" },
        { 10252, 5126, 1000, 16384, 0, "heap < reserve: 0 (J1)" },
        { 10252, 5126, 0, 16384, 10252, "heap unknown (host): no cap" },
        { 10252, 5126, 100, 0, 10252, "reserve 0: cap disabled" },
        { 4000, 5126, 100000, 16384, 4000, "minFloor > want: want wins" },
    };
    for (const Row &r : rows) {
        size_t got = capFloorByHeap(r.want, r.minFloor, r.freeHeap, r.reserve);
        if (got != r.expect) {
            std::cout << "FAIL: " << r.why << " got=" << got
                      << " expect=" << r.expect << std::endl;
            assert(false);
        }
    }
    std::cout << "PASS: capFloorByHeap table (9 rows)" << std::endl;
    return 0;
}

#endif
