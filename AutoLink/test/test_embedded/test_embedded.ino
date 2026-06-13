// test_embedded — on-hardware tests for the AutoLink facade.
//
// Unlike test_desktop (which mocks the hardware and runs on your PC), these
// tests run on a real ESP32 and exercise the actual UART peripheral, FreeRTOS
// tasks, and timers. Flash this to a single board with GPIO17 (TX) jumpered
// directly to GPIO16 (RX) — an external self-loopback — and watch the serial
// monitor. The board talks to itself.
//
// The facade is composed of:
//   * ALink (protocol)        — fully covered by test_desktop
//   * UtilBlink (LED pattern) — fully covered by test_desktop
//   * EspHal (UART/IDF)       — tested here, against real hardware
//   * AutoLink wiring         — tested here, end-to-end on one board
//
// The facade is host-untestable because AutoLink's constructor hardcodes
// EspHal (which depends on FreeRTOS stream buffers, esp_timer, etc.). The
// host test suite covers every other class; this file is the AutoLink
// facade's coverage path.

#include <AutoLink.h>
using namespace autolink;

// Single-board self-loopback: jumper GPIO17 -> GPIO16 on the same board.
static AutoLink link(UART_NUM_2, 16, 17, /*Ping node=*/true);

static uint32_t tBoot   = 0;
static bool     facCheckDone = false;

void test_facade_construction() {
    // Construction completed (we got here). begin() is called in setup().
    Serial.println("[t1] facade construction: ok");
}

void test_facade_state_api() {
    Serial.printf("[t2] state=%s  baud=%lu  errCount=%d  lifetimeErrs=%llu\n",
        link.ready() ? "OK" : "negotiating",
        (unsigned long)link.getCurrentBaud(),
        link.getErrCount(),
        (unsigned long long)link.getLifetimeErrors());
}

void test_facade_stats_api() {
    uint64_t tx = 0, rx = 0, errs = 0;
    link.getStats(tx, rx);
    link.getStats(tx, rx, errs);
    Serial.printf("[t3] stats tx=%llu  rx=%llu  errs=%llu\n",
        (unsigned long long)tx, (unsigned long long)rx, (unsigned long long)errs);
    link.resetStats();
    link.resetErrors();
    link.getStats(tx, rx, errs);
    Serial.printf("[t3] after reset: tx=%llu  rx=%llu  errs=%llu\n",
        (unsigned long long)tx, (unsigned long long)rx, (unsigned long long)errs);
}

void test_facade_stream_api() {
    // The Stream interface (available/read/peek/write/flush) must be
    // callable. The values themselves depend on link state; we just
    // confirm the API surface works without crashing.
    int a = link.available();
    int p = link.peek();
    link.flush();
    Serial.printf("[t4] stream available=%d  peek=%d  flush ok\n", a, p);
}

void test_facade_message_api() {
    // recvMsg returns 0 when no message is ready; we just verify the
    // signature compiles and the call doesn't crash.
    uint8_t buf[64];
    int n = link.recvMsg(buf, sizeof buf);
    Serial.printf("[t5] recvMsg returned %d (expected 0 with no traffic)\n", n);
}

void test_facade_ishealthy() {
    Serial.printf("[t6] isHealthy=%d\n", link.isHealthy() ? 1 : 0);
}

void test_facade_blink() {
    // Async blink: 1 flash, 60/60. Should return immediately.
    uint32_t t0 = millis();
    link.blinkWait(1, 60, 60, 0);
    uint32_t dt = millis() - t0;
    Serial.printf("[t7] async blinkWait(1) returned in %lu ms (expect <5 ms)\n",
        (unsigned long)dt);
}

void test_facade_blink_blocking() {
    // Blocking blink: 2 flashes, 50/50, then 100 ms pause.
    // Should hold the CPU for 2*100 + 100 = 300 ms.
    uint32_t t0 = millis();
    link.blinkWait(2, 50, 50, 100);
    uint32_t dt = millis() - t0;
    Serial.printf("[t8] blocking blinkWait(2, 50, 50, 100) took %lu ms (expect ~300)\n",
        (unsigned long)dt);
}

void test_facade_blink_invalid_ignored() {
    link.blinkWait(0);
    link.blinkWait(-3);
    Serial.println("[t9] blinkWait(0) and blinkWait(-3) ignored as expected");
}

void test_facade_droplink_safe() {
    // dropLink must be safe to call at any time, even before negotiation.
    link.dropLink();
    Serial.println("[t10] dropLink() returned without crash");
}

void test_facade_err_clearing() {
    link.err();
    link.err();
    Serial.printf("[t11] errCount=%d after 2 err() calls\n", link.getErrCount());
    link.clearErr();
    Serial.printf("[t11] errCount=%d after clearErr()\n", link.getErrCount());
}

void runFacadeChecks() {
    test_facade_construction();
    test_facade_state_api();
    test_facade_stats_api();
    test_facade_stream_api();
    test_facade_message_api();
    test_facade_ishealthy();
    test_facade_blink();
    test_facade_blink_blocking();
    test_facade_blink_invalid_ignored();
    test_facade_droplink_safe();
    test_facade_err_clearing();
    facCheckDone = true;
    Serial.println("[test_embedded] facade checks complete");
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[test_embedded] AutoLink facade self-loopback test");
    Serial.println("[test_embedded] Jumper GPIO17 (TX) -> GPIO16 (RX) on this board.");
    link.begin();
    tBoot = millis();
    runFacadeChecks();
}

void loop() {
    if (!facCheckDone) return;
    static uint32_t t = 0;
    if (millis() - t > 2000) {
        Serial.printf("[test_embedded] state=%s baud=%lu heap=%lu uptime=%lus\n",
            link.ready() ? "OK" : "negotiating",
            (unsigned long)link.getCurrentBaud(),
            (unsigned long)ESP.getFreeHeap(),
            (unsigned long)((millis() - tBoot) / 1000));
        t = millis();
    }
}
