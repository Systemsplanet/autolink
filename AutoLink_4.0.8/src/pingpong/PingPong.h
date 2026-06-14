// PingPong.h — v4.0.7 unified entry point for the AutoLink ping-pong
// echo test.
//
// Replaces the v4.0.0..v4.0.6 split where the user had to pick one of
// two separate headers (util/UtilPing.h, util/UtilPong.h) and instantiate
// UtilPing or UtilPONG. v4.0.7 collapses both into one class with a
// Role enum (PING or PONG); the constructor holds both an internal
// UtilPing and an internal UtilPONG and forwards setup()/loop() to
// whichever one the user picked at construction time.
//
// The actual implementation files (UtilPing.h, UtilPong.h) live
// alongside this header in src/pingpong/ and are unchanged. This file
// is a thin facade; the behavior matches the v4.0.0..v4.0.6 split
// exactly.
//
// Usage:
//   #include <pingpong/PingPong.h>
//   using namespace autolink;
//
//   PingPong upp(PingPong::PING, 115200, UART_NUM_2, 16, 17);
//   // optional: WiFi SSID, password, web server port
//   // PingPong upp(PingPong::PING, 115200, UART_NUM_2, 16, 17,
//   //              "YourSSID", "password", 80);
//
//   void setup() { upp.setup(); }
//   void loop()  { upp.loop();  }
#pragma once
#ifdef ARDUINO

#include "UtilPing.h"
#include "UtilPong.h"

namespace autolink {

// ----------------------------------------------------------------------------
// PingPong — single-class entry point for the echo test.
//
// Constructor takes a Role (PING or PONG) plus the standard set of
// hardware + optional WiFi args. Holds both UtilPing and UtilPong
// internally and dispatches setup()/loop() to the one matching the
// chosen role. The unused member costs a few hundred bytes of static
// state (an empty `Pending[8]` array on the PING side, an
// `echoCount_`/`tNotReady_` on the PONG side) but is otherwise free
// and avoids any conditional inheritance / template specialization.
//
// v4.0.7: replacing the v4.0.0..v4.0.6 split, which forced the user
// to pick one of two header files at compile time and made the
// Ping.ino / Pong.ino examples look very different from each other
// (different class name, different constructor signature layout).
// With the unified PingPong the two examples are byte-identical apart
// from the role enum value.
// ----------------------------------------------------------------------------
class PingPong {
public:
    enum Role { PING, PONG };

    PingPong(Role         role,
             uint32_t     debugBaud,
             uart_port_t  uartNum,
             int          rxPin,
             int          txPin,
             const char*  ssid     = nullptr,
             const char*  password = nullptr,
             uint16_t     webPort  = 8765)
        : role_(role)
        , ping_(debugBaud, uartNum, rxPin, txPin, ssid, password, webPort)
        , pong_(debugBaud, uartNum, rxPin, txPin, ssid, password, webPort)
    {}

    // Non-copyable — both members own hardware resources (AutoLink +
    // AutoLinkWeb, the LED timer, etc.) and copying would dangle.
    PingPong(const PingPong&)            = delete;
    PingPong& operator=(const PingPong&) = delete;

    // setup() is forwarded to whichever role is in effect. The other
    // member's setup() is never called, so its LED timer / web monitor
    // never starts.
    void setup() {
        if (role_ == PING) ping_.setup();
        else               pong_.setup();
    }

    // loop() is forwarded every Arduino tick. The unused member's
    // loop() is never called.
    void loop() {
        if (role_ == PING) ping_.loop();
        else               pong_.loop();
    }

    // Role accessor for downstream code that wants to introspect.
    Role role() const { return role_; }

private:
    Role     role_;
    UtilPing ping_;
    UtilPong pong_;
};

} // namespace autolink
#endif // ARDUINO
