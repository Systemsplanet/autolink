// AL89 pin 3 / UartEvTaskPinnedToProtocolCoreTest. AL-D1: converted
// from a source-grep to a real runtime observation. Task-core
// pinning itself has no host-observable effect (there is no real
// FreeRTOS scheduler here) — but the ARGUMENT VALUE
// EspHal::begin() passes to xTaskCreatePinnedToCore is real,
// compiled code path behavior. This build links and RUNS
// EspHal.cpp for real (ARDUINO defined, AUTOLINK_HOST_TEST
// undefined — the opposite macro shape the rest of this project's
// host tests use, which is why this pin needed to move out of the
// shared FieldWedgeFixes89 binary into its own target; see
// test/scripts/env/install_system_stubs.py for the stub set this
// depends on, and AL-A3's host_syntax_check_arduino_tus.sh, which
// established that EspHal.cpp compiles clean against these same
// stubs). The stub's xTaskCreatePinnedToCore now captures the core
// argument it was actually called with
// (g_lastXTaskCreatePinnedToCoreArg) instead of discarding it.
#define ARDUINO 10607
#include "al/hal/EspHal.h"
#include "al/hal/EspHalUartEvent.h"
#include <cassert>
#include <cstdio>
#include <iostream>

using namespace autolink;

// Pin 3 (AL89-3): uart_event_task must be pinned to core 0 (the
// protocol core on ESP32-D0WD), not core 1. The field capture's
// app-task starvation at 512000 baud (8.24 s of dead loopTask, an
// 84-message app-buf-full storm) was produced by pinning the UART
// event task to the SAME core as the Arduino loopTask. Toggle off
// (revert EspHal.cpp's evCore back to a literal 1) -> red: this
// test actually calls EspHal::begin() and reads back the real
// argument the stub's xTaskCreatePinnedToCore was invoked with.
int main() {
    std::cout << "\n=== Pin 3: uart_ev_task pinned to core 0 ==="
              << std::endl;

    // Clear enough resource-creation floors that begin() reaches
    // the xTaskCreatePinnedToCore call instead of aborting earlier
    // on a stubbed-out "insufficient heap" / "handle creation
    // failed" branch — those branches are pinned by other tests
    // (EspHalBeginAndHealthTest, EspHalHeapAccountingTest); this
    // test needs begin() to run to completion.
    g_stubFreeHeapBytes = 1000000;
    g_stubStreamBufferCreateReturn = (StreamBufferHandle_t)1;
    g_stubSemaphoreCreateBinaryReturn = (void *)1;
    g_stubSemaphoreCreateMutexReturn = (SemaphoreHandle_t)1;
    g_stubTimerCreateReturn = (TimerHandle_t)1;
    g_lastXTaskCreatePinnedToCoreArg = -999; // sentinel: "never called"

    AutoLinkConfig cfg;
    EspHal hal(UART_NUM_1, 16, 17, cfg);
    hal.begin();

    std::cout << "  xTaskCreatePinnedToCore called with core="
              << g_lastXTaskCreatePinnedToCoreArg << std::endl;

    if (g_lastXTaskCreatePinnedToCoreArg == -999) {
        std::cerr << "\nFAIL: xTaskCreatePinnedToCore was never called — "
                     "begin() aborted before reaching the uart_event_task "
                     "creation call (check the resource-creation stub "
                     "overrides above are still valid for this EspHal.cpp "
                     "shape)."
                  << std::endl;
        assert(false);
    }
    if (g_lastXTaskCreatePinnedToCoreArg == 1) {
        std::cerr << "\nFAIL: uart_event_task is pinned to core 1 — the "
                     "field capture's app-task starvation at 512000 baud "
                     "(8.24 s of dead loopTask, 84-message app-buf-full "
                     "storm) is the same shape a core-1 pin produces. "
                     "AL89-3's fix pins to core 0 (the protocol core on "
                     "ESP32-D0WD)."
                  << std::endl;
        assert(false);
    }
    if (g_lastXTaskCreatePinnedToCoreArg != 0) {
        std::cerr << "\nFAIL: uart_event_task was pinned to core "
                  << g_lastXTaskCreatePinnedToCoreArg
                  << ", expected 0 (this build's dual-core default) or "
                     "tskNO_AFFINITY (the single-core fallback, a large "
                     "sentinel value, not seen on this dual-core stub "
                     "path)."
                  << std::endl;
        assert(false);
    }
    std::cout << "  PASS (core=0, not the field-capture's core=1)"
              << std::endl;
    return 0;
}
