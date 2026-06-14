// Pong.ino — AutoLink pong node (v4.0.7).
//
// Echoes every complete message back to Ping. Reconnects after any
// link disruption automatically — no state machine needed in the
// sketch.
//
// v4.0.7: unified entry point. Both Ping.ino and Pong.ino are byte-
// identical apart from the PingPong::PING / PingPong::PONG enum
// value. To switch a board from Pong to Ping, change the enum and
// re-flash; no other source change needed.

#include <pingpong/PingPong.h>
using namespace autolink;

PingPong upp(
    PingPong::PONG,
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
