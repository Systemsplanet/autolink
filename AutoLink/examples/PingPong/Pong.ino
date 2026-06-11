// Pong — AutoLink slave node for the ping-pong throughput/echo test.
//
// Flash this onto one ESP32 and Ping.ino onto another. Wire them:
//   Pong GPIO17 (TX) -> Ping GPIO16 (RX)
//   Pong GPIO16 (RX) <- Ping GPIO17 (TX)
//   shared GND
//
// Pong echoes back every complete message it receives and logs throughput +
// error counts every 5 seconds. Pass WiFi credentials (last three args) to
// enable the live web dashboard, or omit them for a UART-only link.

#include "UtilPong.h"
using namespace autolink;

UtilPong pong(
    115200,       // Serial debug baud
    UART_NUM_2,   // UART port for the AutoLink wire
    16,           // RX pin
    17,           // TX pin
    "YourSSID",   // WiFi SSID   — omit (or pass nullptr) to disable web monitor
    "password",   // WiFi password
    80            // web server port
);

void setup() { pong.setup(); }
void loop()  { pong.loop();  }
