// PingPong role-selector wrapper. User-facing API:
// instantiate at namespace scope, call setup()/loop().
// Carries both Ping and Pong and dispatches by role at
// compile-time-free runtime branch.
#pragma once
#ifdef ARDUINO

#    include "al/pingpong/Ping.h"
#    include "al/pingpong/Pong.h"

namespace autolink
{
class PingPong
{
public:
    enum Role { PING, PONG };

    PingPong(Role role, uint32_t debugBaud,
             uart_port_t uartNum, int rxPin, int txPin,
             const char *ssid = nullptr,
             const char *password = nullptr,
             uint16_t webPort = 8765)
        : role_(role),
          ping_(debugBaud, uartNum, rxPin, txPin, ssid,
                password, webPort),
          pong_(debugBaud, uartNum, rxPin, txPin, ssid,
                password, webPort)
    {
    }

    PingPong(const PingPong &) = delete;
    PingPong &operator=(const PingPong &) = delete;

    void setup()
    {
        if (role_ == PING)
            ping_.setup();
        else
            pong_.setup();
    }

    void loop()
    {
        if (role_ == PING)
            ping_.loop();
        else
            pong_.loop();
    }

    Role role() const { return role_; }


    void setFillMode(Ping::FillMode m)
    {
        if (role_ == PING)
            ping_.setFillMode(m);
    }
    Ping::FillMode fillMode() const
    {
        return role_ == PING
            ? ping_.fillMode()
            : Ping::FillMode::SEQUENTIAL;
    }

private:
    Role role_;
    Ping ping_;
    Pong pong_;
};

} // namespace autolink
#endif
