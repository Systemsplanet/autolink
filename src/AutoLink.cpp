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

    // Clamp the user-supplied config to the
    // declared array size before the HAL
    // and Link see it. A caller setting
    // allowedBaudsCount = 20 (above
    // AUTOLINK_MAX_BAUDS) would otherwise
    // OOB-read through allowedBauds(i) in
    // the sweep / lockOk path. The clamp
    // is idempotent — calling it on an
    // already-in-range cfg is a no-op.
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

// Intentionally empty. ARQ cache
// implementation lives in
// al/link/arq/ArqCache.cpp. Link reset /
// clearAll is reached by the link
// through its arqCache_ reference; the
// facade does not call into the cache
// directly from any other path.
} // namespace autolink
