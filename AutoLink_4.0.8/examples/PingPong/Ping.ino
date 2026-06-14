// Ping.ino — AutoLink ping node (v4.0.7).
//
// Sends random-length messages; Pong (Pong.ino) echoes each one back.
// Pair two ESP32 boards with TX<->RX crossed and shared GND.
//
// v4.0.7: unified entry point. Both Ping.ino and Pong.ino are byte-
// identical apart from the PingPong::PING / PingPong::PONG enum
// value. To switch a board from Ping to Pong, change the enum and
// re-flash; no other source change needed.

#include <pingpong/PingPong.h>
using namespace autolink;

PingPong upp(
    PingPong::PING,
    115200,                // Serial baud
    UART_NUM_2,            // UART port
    16,                    // RX pin
    17,                    // TX pin
    "<changeme>",          // WiFi SSID, or nullptr to disable web monitor
    "<changeme>",          // WiFi password
    80                     // web server port
);

void setup() { upp.setup(); }
void loop()  { upp.loop();  }
