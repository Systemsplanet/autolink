// Ping — AutoLink ping node for the ping-pong throughput/echo test.
//
// Flash this onto one ESP32 and Pong.ino onto another. Wire them:
//   Ping TX(GPIO17) ──► Pong RX(GPIO16)
//   Ping RX(GPIO16) ◄── Pong TX(GPIO17)
//   shared GND
//
// Ping sends random-length messages, verifies the echoes Pong sends back,
// and logs throughput + error counts every 5 seconds. Pass WiFi credentials
// (last three args) to enable the live web dashboard, or omit them for a
// UART-only link.

#include "UtilPing.h"
using namespace autolink;

UtilPing ping(
    115200,       // Serial debug baud
    UART_NUM_2,   // UART port for the AutoLink wire
    16,           // RX pin
    17,           // TX pin
    "YourSSID",   // WiFi SSID   — omit (or pass nullptr) to disable web monitor
    "password",   // WiFi password
    80            // web server port
);

void setup() { ping.setup(); }
void loop()  { ping.loop();  }
