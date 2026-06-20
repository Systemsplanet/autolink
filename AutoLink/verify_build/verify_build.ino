// verify_build.ino — minimal compile-check sketch for the AutoLink library.
// Exercises every public entry point to force the compiler to instantiate
// the templates and catch any include/signature issues at build time.
//
// This sketch does NOT run on hardware — it is only compiled.
//
// Board: DFRobot FireBeetle-ESP32 (the same board ArduinoDroid is targeting).
// Core:  esp32:esp32@3.3.5 (the user's exact version).

#include "AutoLink.h"
#include "AutoLinkWeb.h"
#include "Log.h"
#include <Arduino.h>

using namespace autolink;

namespace {

constexpr int APP_BUF_SIZE  = 1024;
constexpr int WEB_BUF_SIZE  = 4096;

uint8_t appBuf[APP_BUF_SIZE];
uint8_t webBuf[WEB_BUF_SIZE];

AutoLinkConfig makeConfig() {
    AutoLinkConfig c;
    c.maxMsg        = 1024;
    c.idleTimeoutMs = 5000;
    c.ledPin        = 9;  // FireBeetle-ESP32 builtin LED
    c.errThreshold  = 20;
    c.reliableMode  = true;
    return c;
}

AutoLink*      g_link = nullptr;
AutoLinkWeb*   g_web  = nullptr;
AutoLinkConfig g_cfg;

} // namespace

void setup() {
    Serial.begin(115200);
    pinMode(9, OUTPUT);

    g_cfg = makeConfig();

    g_link = new AutoLink(/*uart=*/(uart_port_t)1, /*rx=*/16, /*tx=*/17, /*isMaster=*/true, g_cfg);
    g_link->begin();
    g_link->resetStats();
    g_link->resetErrors();
    g_link->resetDiag();

    g_web = new AutoLinkWeb(*g_link);
    g_web->setRole("Ping");
    g_web->begin("verify-ssid", "verify-pass", 8765);

    // Stream API
    uint8_t out[1] = { 0x42 };
    g_link->send(out, 1);

    // Touch every Stats field to force the struct to be linked.
    Stats st = {};
    g_link->getStats(st);
    (void)st.tx;
    (void)st.rx;
    (void)st.discCount;
    (void)st.frameErrs;

    // Touch AutoLinkWeb public API.
    g_web->isUp();

    Log::getLog().setLevel(Log::DEBUG);
    Log::getLog().info("verify", "compile-check OK, AUTOLINK_VERSION=%s", AUTOLINK_VERSION);
}

void loop() {
    if (g_link && g_link->available() > 0) {
        (void)g_link->read();
    }
    delay(10);
}
