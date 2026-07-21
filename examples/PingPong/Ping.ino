
#include "PingPong.h"
using namespace autolink;

PingPong upp(PingPong::PING, 115200, UART_NUM_2, 16, 17, "<changeme>",

             "<changeme>", 80);

void setup() { upp.setup(); }
void loop() { upp.loop(); }
