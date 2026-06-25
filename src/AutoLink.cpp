// AutoLink facade implementation.
// ARQ cache lives in al/link/arq/ArqCache.cpp;
// the facade passes it to the Link via
// the Link ctor's IArqCache& parameter.
// Reference semantics make the
// cache-outlives-link contract
// unbreakable at compile time. Public
// constructor body is out-of-line so the
// header only forward-declares EspHal
// under AUTOLINK_HOST_TEST and the
// production HAL boundary stays clean.
// The host body pulls in
// test/common/EspHalStub.h via the
// -I../common include path the test
// Makefile sets up. The device body
// uses the real EspHal from al/hal/EspHal.h.
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
    constexpr int kHdr = MSG_HDR;
    size_t need = 2 * 16 * (cfg.maxMsg + kHdr);
    if (cfg.streamBufferSize < need)
        cfg.streamBufferSize = need;
    size_t need_tx = 16 * ((cfg.maxMsg + kHdr) * 5 / 4 + 64);
    if (cfg.txBufferSize < need_tx)
        cfg.txBufferSize = need_tx;

    hal =
        IHalPtr(std::make_unique<EspHal>(u_num, rx_pin, tx_pin, cfg).release());
    link = std::make_unique<Link>(*hal, arqCache_, isMasterNode, cfg);
}
#elif defined(AUTOLINK_HOST_TEST)
AutoLink::AutoLink(uart_port_t u_num, int rx_pin, int tx_pin, bool isMasterNode,
                   AutoLinkConfig cfg) {
    constexpr int kHdr = 6;
    size_t need = 2 * 16 * (cfg.maxMsg + kHdr);
    if (cfg.streamBufferSize < need)
        cfg.streamBufferSize = need;
    size_t need_tx = 16 * ((cfg.maxMsg + kHdr) * 5 / 4 + 64);
    if (cfg.txBufferSize < need_tx)
        cfg.txBufferSize = need_tx;

    hal =
        IHalPtr(std::make_unique<EspHal>(u_num, rx_pin, tx_pin, cfg).release());
    link = std::make_unique<Link>(*hal, arqCache_, isMasterNode, cfg);
}
#endif

// Intentionally empty. ARQ cache
// implementation lives in
// al/link/arq/ArqCache.cpp. Link reset /
// clearAll is reached by the link
// through its arqCache_ reference; the
// facade does not call into the cache
// directly from any other path.
} // namespace autolink