// Pong.ino — AutoLink pong node.
//
// Echoes every complete message back to Ping. Reconnects after any
// link disruption automatically — no state machine needed in the
// sketch.
//
// To switch a board from Pong to Ping, change PingPong::PONG to
// PingPong::PING and re-flash. The two .ino files are otherwise
// identical.

#include "PingPong.h"
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
