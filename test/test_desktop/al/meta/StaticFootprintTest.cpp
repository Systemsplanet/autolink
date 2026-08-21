// AL-A2: this library had exactly one host-checkable static-memory
// assertion (Log.h's own static_assert on the ring alone) and no
// test summing this library's total known static (.bss) footprint.
// The dram0_0_seg overflow (see docs/Version.md's entry on it)
// was a real cross-compile failure that no host test could have
// caught, because nothing on host measured the actual combined
// size of what this library declares as static/function-local-
// static.
//
// This test measures it directly — not by grep, by sizeof() against
// the real class definitions, built with ESP_PLATFORM defined so
// Log.h takes its device code path (the espRing_ member, guarded
// `#ifdef ESP_PLATFORM`, is otherwise invisible to a plain host
// build and sizeof(Log) silently reports ~120 bytes instead of the
// ~23.8 KB it actually is on device).
//
// Scope, stated plainly: this covers the fixed static contributors
// this library's own source declares (the Log singleton, the httpd
// chunk buffer) — not the whole firmware image. It cannot see
// Arduino core, WiFi/BT buffers, or any other library's .bss/.data,
// and a real dram0_0_seg overflow can still happen from something
// entirely outside this library's control. What it DOES catch: any
// future change to this library that grows ITS OWN static
// footprint — the exact shape of the QUEUE_CAP regression
// (docs/Version.md) — before any cross-compile is available. See
// AutoLinkConfig.h's AUTOLINK_STATIC_DRAM_BUDGET for the budget and
// its rationale.
#ifndef ARDUINO

#    include <cassert>
#    include <iostream>

#    ifndef ESP_PLATFORM
#        define ESP_PLATFORM 1
#    endif

#    include "al/AutoLinkConfig.h"
#    include "al/util/log/Log.h"

using namespace autolink;

namespace {

// The httpd chunk buffer (AutoLinkWebHandlersData.cpp's handleRoot)
// is a function-local static inside an ARDUINO-only TU — it cannot
// be sizeof()'d directly from a host TU the way Log can, since the
// whole file only compiles under ARDUINO (see
// test/scripts/env/host_syntax_check_arduino_tus.sh, AL-A3). Its
// size is fixed and stated in its own declaration
// (`static char chunk[1024];`), so it is added here as a named
// constant rather than re-declared or guessed at — a change to that
// buffer's size is still caught by AL-A3 type-checking the file
// (it will still compile) but not by this budget check unless this
// constant is updated to match. That gap is deliberate: silently
// re-declaring a buffer here to "measure" it would drift from the
// real one the moment either changed independently.
constexpr size_t kHttpdChunkBufferBytes = 1024;

void test_log_singleton_is_measured_with_the_ring_included() {
    std::cout << "\n=== Pin: sizeof(Log) on the ESP_PLATFORM code path "
                 "includes espRing_, not just the host stand-in ==="
              << std::endl;
    // The whole reason AL-A2 exists: on a plain host build (no
    // ESP_PLATFORM), Log.h's #else branch is a small std::deque-
    // backed stand-in and sizeof(Log) reports a number nowhere near
    // what actually lands in the device's .bss. This file forces
    // ESP_PLATFORM above specifically so this measurement is real.
    size_t logSize = sizeof(Log);
    std::cout << "  sizeof(Log) [ESP_PLATFORM path] = " << logSize << " B"
              << std::endl;
    assert(logSize > 20000 &&
           "sizeof(Log) is suspiciously small for the ESP_PLATFORM code "
           "path — espRing_ may not be included (check the #ifdef "
           "ESP_PLATFORM guard in Log.h is still active), which would "
           "make every other assertion in this file measure nothing");
    std::cout << "  PASS" << std::endl;
}

void test_static_footprint_is_within_budget() {
    std::cout << "\n=== Pin: this library's known static (.bss) "
                 "contributors stay within AUTOLINK_STATIC_DRAM_BUDGET ==="
              << std::endl;
    size_t total = sizeof(Log) + kHttpdChunkBufferBytes;
    std::cout << "  sizeof(Log)              = " << sizeof(Log) << " B"
              << std::endl;
    std::cout << "  httpd chunk buffer        = " << kHttpdChunkBufferBytes
              << " B" << std::endl;
    std::cout << "  total                     = " << total << " B"
              << std::endl;
    std::cout << "  AUTOLINK_STATIC_DRAM_BUDGET = "
              << AUTOLINK_STATIC_DRAM_BUDGET << " B" << std::endl;
    if (total > AUTOLINK_STATIC_DRAM_BUDGET) {
        std::cerr << "\nFAIL: known static contributors total " << total
                   << " B, over the " << AUTOLINK_STATIC_DRAM_BUDGET
                   << " B budget in AutoLinkConfig.h. This is exactly "
                      "the shape of the historical QUEUE_CAP "
                      "regression that a real cross-compile caught as a "
                      "dram0_0_seg overflow — either lower the "
                      "contributor that grew, or raise the budget "
                      "deliberately with a comment stating why the "
                      "device has room for it."
                   << std::endl;
        assert(false);
    }
    std::cout << "  PASS (" << (AUTOLINK_STATIC_DRAM_BUDGET - total)
              << " B headroom)" << std::endl;
}

} // namespace

int main() {
    test_log_singleton_is_measured_with_the_ring_included();
    test_static_footprint_is_within_budget();
    std::cout << "\nAll StaticFootprint pins passed." << std::endl;
    return 0;
}

#else
int main() { return 0; }
#endif
