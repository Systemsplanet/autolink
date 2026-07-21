
#include "AutoLink.h"
#ifdef AUTOLINK_HOST_TEST
#    include "EspHalStub.h"
#endif
#include <cstdio>

namespace autolink {

// Construction is split between include/AutoLink.h's member
// initializers (isMasterNode_ / cfg_ capture
// pair) and the per-build-mode bodies below. The bodies wire
// up the HAL and Link; the header's ctor sets the diagnostic
// member fields. Both are needed so the dtor / begin() logs
// can read role and config without a stale constructor arg.
#ifdef ARDUINO
AutoLink::AutoLink(uart_port_t u_num, int rx_pin, int tx_pin, bool isMasterNode,
                   AutoLinkConfig cfg)
    : blinkHal(cfg.ledPin), blinker(blinkHal) {
    blinkHal.bind(&blinker);

    cfg.clampToMaxBauds();
    isMasterNode_ = isMasterNode;
    cfg_ = cfg;

    hal =
        IHalPtr(std::make_unique<EspHal>(u_num, rx_pin, tx_pin, cfg).release());
    link = std::make_unique<Link>(*hal, arqCache_, isMasterNode, cfg);
}
#elif defined(AUTOLINK_HOST_TEST)
AutoLink::AutoLink(uart_port_t u_num, int rx_pin, int tx_pin, bool isMasterNode,
                   AutoLinkConfig cfg) {
    cfg.clampToMaxBauds();
    isMasterNode_ = isMasterNode;
    cfg_ = cfg;
    hal =
        IHalPtr(std::make_unique<EspHal>(u_num, rx_pin, tx_pin, cfg).release());
    link = std::make_unique<Link>(*hal, arqCache_, isMasterNode, cfg);
}
#endif

} // namespace autolink
