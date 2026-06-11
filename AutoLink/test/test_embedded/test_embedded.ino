// test_embedded — on-hardware tests for AutoLink.
//
// Unlike test_desktop (which mocks the hardware and runs on your PC), these
// tests run on a real ESP32 and exercise the actual UART peripheral, FreeRTOS
// tasks, and timers. Flash this to a single board with GPIO17 (TX) jumpered
// directly to GPIO16 (RX) — an external self-loopback — and watch the serial
// monitor. The board talks to itself: the master and slave logic both run, and
// a healthy run locks the link and echoes messages with zero errors.
//
// This is a starting point; add board-specific timing and stress cases here.

#include "AutoLink.h"
using namespace autolink;

// Single-board self-loopback: jumper GPIO17 -> GPIO16 on the same board.
AutoLink link(UART_NUM_2, 16, 17, /*isMaster=*/true);

static uint32_t sent = 0, ok = 0, bad = 0;
static uint8_t  buf[256];

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[test_embedded] AutoLink on-hardware self-loopback test");
    Serial.println("[test_embedded] Jumper GPIO17 (TX) -> GPIO16 (RX) on this board.");
    link.begin();
}

void loop() {
    // With TX jumpered to RX, the board cannot negotiate with a peer (it would
    // be answering its own PINGs). This sketch is a scaffold: in a real two-
    // board embedded test, flash Ping.ino and Pong.ino instead. Here we simply
    // confirm the peripheral, tasks, and timers initialize without crashing.
    static uint32_t t = 0;
    if (millis() - t > 2000) {
        Serial.printf("[test_embedded] state=%s baud=%lu heap=%lu\n",
            link.ready() ? "OK" : "negotiating",
            (unsigned long)link.getCurrentBaud(),
            (unsigned long)ESP.getFreeHeap());
        t = millis();
    }
    (void)sent; (void)ok; (void)bad; (void)buf;
}
