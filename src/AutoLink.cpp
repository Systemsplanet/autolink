
#include "AutoLink.h"
#ifdef AUTOLINK_HOST_TEST
#    include "EspHalStub.h"
#endif
#include <cstdio>

namespace autolink {

#ifdef ARDUINO
AutoLink::AutoLink(uart_port_t u_num, int rx_pin, int tx_pin, bool isMasterNode,
                   AutoLinkConfig cfg)
    : blinkHal(cfg.ledPin), blinker(blinkHal) {
    blinkHal.bind(&blinker);

    cfg.clampToMaxBauds();

    hal =
        IHalPtr(std::make_unique<EspHal>(u_num, rx_pin, tx_pin, cfg).release());
    link = std::make_unique<Link>(*hal, arqCache_, isMasterNode, cfg);
}
#elif defined(AUTOLINK_HOST_TEST)
AutoLink::AutoLink(uart_port_t u_num, int rx_pin, int tx_pin, bool isMasterNode,
                   AutoLinkConfig cfg) {
    cfg.clampToMaxBauds();
    hal =
        IHalPtr(std::make_unique<EspHal>(u_num, rx_pin, tx_pin, cfg).release());
    link = std::make_unique<Link>(*hal, arqCache_, isMasterNode, cfg);
}
#endif

} // namespace autolink
