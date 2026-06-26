// verify_build.ino — mirrors the README's Pong.ino shape
// (file-scope PingPong + minimal setup/loop) so the
// cross-compile catches ArduinoDroid-specific ctor errors
// in the user-facing entry point. Per AGENTS.md rule 17,
// no RTOS work happens until upp.setup().

#include "AutoLink.h"
#include "PingPong.h"

using namespace autolink;

PingPong upp(PingPong::PONG, 115200, UART_NUM_2, 16, 17, nullptr, nullptr, 80);

void setup() { upp.setup(); }
void loop() { upp.loop(); }