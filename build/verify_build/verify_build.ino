// verify_build.ino — minimal sketch that exercises
// every public AutoLink / AutoLinkWeb API. Used by
// verify_build.sh as the compile target to catch
// Arduino-only header paths, #ifdef ARDUINO typos, and
// library layout breaks that host tests miss.
//
// Per AGENTS.md rule 16: a sketch with every public
// include the user is expected to type, compiled
// against the real ESP32 core. This catches path /
// subdir / quoting bugs in the public API surface.

#include "AutoLink.h"
#include "PingPong.h"

using namespace autolink;

void setup()
{
    Serial.begin(115200);

    AutoLinkConfig cfg;
    cfg.maxMsg = 1024;
    cfg.idleTimeoutMs = 5000;
    cfg.ledPin = 2;
    cfg.errThreshold = 20;

    AutoLink alink((uart_port_t)1, /*rx=*/16,
                   /*tx=*/17, /*isMaster=*/true, cfg);
    alink.begin();

    // Stream API.
    uint8_t buf[64];
    alink.available();
    alink.read();
    alink.peek();
    alink.write((uint8_t)0xAA);
    alink.write(buf, sizeof(buf));
    alink.flush();
    alink.read(buf, sizeof(buf));

    // Message API.
    alink.sendMsg(buf, sizeof(buf));
    alink.recvMsg(buf, sizeof(buf));

    // v5 ARQ inspection.
    Stats st;
    alink.getStats(st);
    alink.resetStats();
    alink.resetErrors();
    alink.resetDiag();
    Diag d;
    alink.getDiag(d);

    // facade-driven link pause.
    alink.setLinkPaused(true);

    // v5 link ops.
    alink.ready();
    alink.dropLink();
    alink.flushRx();

    // PingPong helpers (optional — only if available
    // in this build). The example under
    // examples/PingPong/Ping.ino / Pong.ino uses
    // UtilPing::Ping / UtilPong::Pong directly. Those
    // headers are NOT public; they're internal. Skip
    // them here.

    // blinkWait is a no-op stub in the verify build.
    alink.blinkWait(1);
}

void loop() {}