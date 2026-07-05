// User-facing role selector.
// std::variant holds only the active role;
// inactive role is never constructed.
#pragma once
#ifdef ARDUINO

#    include "al/pingpong/Ping.h"
#    include "al/pingpong/Pong.h"
#    include <variant>

namespace autolink {
class PingPong {
public:
    enum Role { PING, PONG };

    PingPong(Role role, uint32_t debugBaud, uart_port_t uartNum, int rxPin,
             int txPin, const char *ssid = nullptr,
             const char *password = nullptr, uint16_t webPort = 8765)
        : role_id_(role),
          active_(role == PING
                      ? std::variant<std::monostate, Ping,
                                     Pong>{ std::in_place_index<1>, debugBaud,
                                            uartNum, rxPin, txPin, ssid,
                                            password, webPort }
                      : std::variant<std::monostate, Ping, Pong>{
                            std::in_place_index<2>, debugBaud, uartNum, rxPin,
                            txPin, ssid, password, webPort }) {}

    PingPong(const PingPong &) = delete;
    PingPong &operator=(const PingPong &) = delete;

    void setup() {
        std::visit(
            [](auto &r) {
                if constexpr (!std::is_same_v<std::decay_t<decltype(r)>,
                                              std::monostate>)
                    r.setup();
            },
            active_);
    }

    void loop() {
        std::visit(
            [](auto &r) {
                if constexpr (!std::is_same_v<std::decay_t<decltype(r)>,
                                              std::monostate>)
                    r.loop();
            },
            active_);
    }

    Role role() const { return role_id_; }

    void setFillMode(Ping::FillMode m) {
        if (auto *p = std::get_if<Ping>(&active_))
            p->setFillMode(m);
    }
    Ping::FillMode fillMode() const {
        if (auto *p = std::get_if<Ping>(&active_))
            return p->fillMode();
        return Ping::FillMode::SEQUENTIAL;
    }

private:
    Role role_id_;
    std::variant<std::monostate, Ping, Pong> active_;
};

} // namespace autolink
#endif