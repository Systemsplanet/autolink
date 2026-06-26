// Ping.ino — AutoLink ping node.
//
// Sends random-length messages; Pong (Pong.ino) echoes
// each one back. Pair two ESP32 boards with TX<->RX
// crossed and shared GND.
//
// To switch a board from Ping to Pong, change
// PingPong::PING to PingPong::PONG and re-flash. The
// two .ino files are otherwise identical.

#include "PingPong.h"
using namespace autolink;

PingPong upp(PingPong::PING,
             115200,       // Serial baud
             UART_NUM_2,   // UART port
             16,           // RX pin
             17,           // TX pin
             "<changeme>", // WiFi SSID, or nullptr to
                           // disable web monitor
             "<changeme>", // WiFi password
             80            // web server port
);

void setup() { upp.setup(); }
void loop() { upp.loop(); }
