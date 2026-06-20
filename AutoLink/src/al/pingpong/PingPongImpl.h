// PingPong.h — unified entry point for the AutoLink ping-pong echo test.
//
// Constructor takes a Role (PING or PONG); setup()/loop() dispatch to
// the matching internal member. The other member is held but never
// started, costing a few hundred bytes of unused static state in
// exchange for no conditional inheritance.
//
// Usage:
//   #include "PingPong.h"
//   using namespace autolink;
//   PingPong upp(PingPong::PING, 115200, UART_NUM_2, 16, 17);
#pragma once
#ifdef ARDUINO

#include "al/pingpong/UtilPing.h"
#include "al/pingpong/UtilPong.h"

namespace autolink {

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

    // DEPRECATED bool overload — kept for legacy sketch compat.
    // Maps bool to the Role enum. New code should use PING/PONG
    // directly; this will be removed in a future major version.
    PingPong(bool         isPing,
             uint32_t     debugBaud,
             uart_port_t  uartNum,
             int          rxPin,
             int          txPin,
             const char*  ssid     = nullptr,
             const char*  password = nullptr,
             uint16_t     webPort  = 8765)
        : PingPong(isPing ? PING : PONG,
                   debugBaud, uartNum, rxPin, txPin,
                   ssid, password, webPort)
    {}

    // Both members own hardware resources (AutoLink + AutoLinkWeb,
    // LED timer, etc.); copies would dangle.
    PingPong(const PingPong&)            = delete;
    PingPong& operator=(const PingPong&) = delete;

    void setup() {
        if (role_ == PING) ping_.setup();
        else               pong_.setup();
    }

    void loop() {
        if (role_ == PING) ping_.loop();
        else               pong_.loop();
    }

    Role role() const { return role_; }

    // Fill mode (Sequential/Random). Wired by UtilPing::setup() via
    // a static thunk + setFillModeHook. No-op on Pong (mode is a
    // Ping-only concept).
    void setFillMode(UtilPing::FillMode m) {
        if (role_ == PING) ping_.setFillMode(m);
    }
    UtilPing::FillMode fillMode() const {
        return role_ == PING ? ping_.fillMode() : UtilPing::FillMode::SEQUENTIAL;
    }

private:
    Role     role_;
    UtilPing ping_;
    UtilPong pong_;
};

} // namespace autolink
#endif // ARDUINO
