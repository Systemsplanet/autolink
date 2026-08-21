// AL89 pin 7 / BreakWindowEpochTest. Extracted from
// FieldWedgeFixes89Test.cpp (AL90-17 split
// the monolithic 22.7 KB file into one .cpp
// per pin to keep each under the 15 KB cap,
// AGENTS.md rule 20a). The pin's logic is
// unchanged; only the file boundary and the
// function name (per AL90-15) move.
#include "FieldWedgeFixes89Common.h"

using namespace autolink;
using namespace autolink::field89;

// 0->1 SWP->OK transition. The summary
// discards windows whose epoch has
// changed since the window opened.
// Toggle off -> red.
void test_BreakWindowEpochTest() {
    std::cout << "\n=== Pin 7: BREAK-storm window epoch ===" << std::endl;
    std::string halSrc = readFile(projectRoot() + "/src/al/hal/EspHal.h");
    std::string eventSrc =
        readFile(projectRoot() + "/src/al/hal/EspHalUartEvent.h");
    std::string setSrc = readFile(projectRoot() + "/src/al/hal/EspHal.h");
    assert(!halSrc.empty() && !eventSrc.empty());
    // HAL has breakWindowEpoch_ field.
    assert(halSrc.find("breakWindowEpoch_") != std::string::npos &&
           "EspHal.h is missing breakWindowEpoch_ — the "
           "BREAK-storm window epoch field is gone. The "
           "field capture's 263-BREAK storm 53 ms after "
           "relock inflated a healthy session's storm "
           "summary because pre-lock BREAKs straddled the "
           "lock transition.");
    // setOkState bumps the epoch.
    assert(setSrc.find("breakWindowEpoch_++") != std::string::npos &&
           "setOkState no longer bumps breakWindowEpoch_ on "
           "0->1 transitions — the lock epoch never advances, "
           "so the window discard never fires.");
    // The event task uses the epoch.
    assert(eventSrc.find("window_epoch_at_open") != std::string::npos &&
           "EspHalUartEvent.h is missing window_epoch_at_open "
           "— the BREAK-storm window is no longer epoch-"
           "tagged at open, so the summary block can't "
           "discard windows that straddled a lock.");
    assert(eventSrc.find("windowStraddledLock") != std::string::npos &&
           "EspHalUartEvent.h is missing windowStraddledLock "
           "— the summary block no longer discards "
           "lock-straddling windows.");
    std::cout << "  PASS (breakWindowEpoch_ field, setOkState bump, "
                 "window_epoch_at_open, windowStraddledLock all present)"
              << std::endl;
}

// Pin 8 (AL89-8): the P3 slave camp
// re-arm is bounded by the
// resweepPrefDeadlineMs_ budget. The
// deadline is checked on every
// re-arm and the camp falls to P1
