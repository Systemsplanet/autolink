// Boundary invariants from the
// god-class split follow-up. One pin
// per fix; each pin reads the source
// and asserts the post-fix shape is
// intact. Toggle off (revert the fix)
// and the matching pin flips red.
//
// Six pins, one per fix:
//   1. EspHal::setSpd() drains TX
//      before the baud retune.
//   2. IHal no longer holds a Link*
//      back-reference; the HAL dispatches
//      via an ILinkEvents listener.
//   3. WINDOW ownership inverted:
//      Ping owns the pipeline window, the
//      cache accepts it via ctor and
//      validates POOL_SIZE >= 2*window.
//   4. AutoLinkConfig clamps
//      allowedBaudsCount to
//      AUTOLINK_MAX_BAUDS at the ctor
//      (and provides a validating accessor).
//   5. _test_forwardResync test flag
//      evicted from the user-facing
//      AutoLinkConfig (moved to a
//      LinkTestAccessor toggle).
//   6. The magic-256 / magic-6 chunk/
//      pool static_assert references
//      MAX_CHUNK + MSG_HDR and
//      ArqCache::POOL_BUF_MAX by name
//      instead of literals.
//
// Source-grep only — each pin flips
// red if a future refactor reverts
// the matching fix.
#ifndef ARDUINO

#    include <cassert>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>

namespace {

std::string readFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.good())
        return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string projectRoot() {
    std::string base = ".";
    for (int i = 0; i < 8; i++) {
        std::ifstream pf(base + "/AGENTS.md");
        if (pf.good())
            return base;
        base += "/..";
    }
    return ".";
}

void test_esphal_setSpd_drains_tx_before_retune() {
    std::cout
        << "\n=== Pin 1: EspHal::setSpd() drains TX via uart_wait_tx_done "
           "before the baud retune ==="
        << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/hal/EspHal.h");
    assert(!src.empty());

    // Locate the setSpd body. The method
    // takes uint32_t spd and overrides the
    // IHal::setSpd virtual.
    auto fnPos = src.find("void setSpd(uint32_t spd) override");
    assert(fnPos != std::string::npos);

    size_t scan = fnPos;
    int depth = 0;
    bool foundOpen = false;
    size_t endPos = std::string::npos;
    for (; scan < src.size(); scan++) {
        if (src[scan] == '{') {
            depth++;
            foundOpen = true;
        } else if (src[scan] == '}') {
            depth--;
            if (foundOpen && depth == 0) {
                endPos = scan;
                break;
            }
        }
    }
    assert(endPos != std::string::npos);
    std::string body = src.substr(fnPos, endPos - fnPos + 1);

    // The TX drain must be a real
    // function call (uart_wait_tx_done(...)),
    // not just a comment mention. Look for
    // the open paren right after the symbol.
    auto drainPos = body.find("uart_wait_tx_done(");
    auto setPos = body.find("uart_set_baudrate");
    assert(drainPos != std::string::npos);
    assert(setPos != std::string::npos);
    assert(drainPos < setPos &&
           "uart_wait_tx_done(...) must precede uart_set_baudrate");

    // And the existing flush-input + retune
    // chain is intact after the drain.
    assert(body.find("uart_flush_input") != std::string::npos);

    std::cout << "  PASS (uart_wait_tx_done present, ordered before "
                 "uart_set_baudrate)\n";
}

void test_ihal_no_link_pointer_and_dispatches_via_ilinkevents() {
    std::cout << "\n=== Pin 2: IHal has no Link* field; HALs dispatch via "
                 "ILinkEvents ==="
              << std::endl;
    std::string ihal = readFile(projectRoot() + "/src/al/hal/IHal.h");
    std::string linkH = readFile(projectRoot() + "/src/al/link/Link.h");
    std::string espHal = readFile(projectRoot() + "/src/al/hal/EspHal.h");
    std::string mockHal = readFile(projectRoot() + "/test/common/MockHal.h");

    // The public `link` field on IHal is gone
    // (it was a `Link *link = nullptr;` member).
    // Any `Link *` member on IHal would
    // re-introduce the cycle we're fixing.
    assert(ihal.find("Link *link") == std::string::npos);
    assert(ihal.find("Link* link") == std::string::npos);

    // IHal has the one-shot setEvents hook
    // and an ILinkEvents listener interface.
    assert(ihal.find("class ILinkEvents") != std::string::npos);
    assert(ihal.find("setEvents") != std::string::npos);

    // Link implements ILinkEvents (multiple
    // inheritance: ... , public ILinkEvents).
    assert(linkH.find("ILinkEvents") != std::string::npos);

    // Link::Link wires itself into the HAL
    // via setEvents, not the old bind().
    std::string linkCore =
        readFile(projectRoot() + "/src/al/link/LinkCore.cpp");
    assert(linkCore.find("hw.setEvents(*this)") != std::string::npos);
    assert(linkCore.find("hw.bind(") == std::string::npos);

    // EspHal dispatches via events(), not
    // hal->link->... . The HAL never sees
    // a Link*.
    assert(espHal.find("events()->onRx") != std::string::npos);
    assert(espHal.find("events()->onBreak") != std::string::npos);
    assert(espHal.find("events()->onTimer") != std::string::npos);
    // No `link->` paths anywhere in EspHal.
    assert(espHal.find("->link->") == std::string::npos);

    // MockHal mirrors the new shape: pipe_data
    // and sendBreak dispatch through events().
    assert(mockHal.find("events()->onRx") != std::string::npos);
    assert(mockHal.find("events()->onBreak") != std::string::npos);
    assert(mockHal.find("events()->onTimer") != std::string::npos);
    assert(mockHal.find("->link->") == std::string::npos);

    std::cout << "  PASS (no IHal::link, ILinkEvents wired, setEvents at "
                 "Link ctor, HALs dispatch through events())\n";
}

void test_window_owned_by_ping_cache_validates_injected_window() {
    std::cout
        << "\n=== Pin 3: WINDOW ownership lives with Ping (flow "
           "controller); ArqCache ctor validates POOL_SIZE >= 2*window ==="
        << std::endl;
    std::string arqH = readFile(projectRoot() + "/src/al/link/arq/ArqCache.h");
    std::string arqCpp =
        readFile(projectRoot() + "/src/al/link/arq/ArqCache.cpp");
    std::string pingH = readFile(projectRoot() + "/src/al/pingpong/Ping.h");
    std::string cfgH = readFile(projectRoot() + "/src/al/AutoLinkConfig.h");
    std::string alH = readFile(projectRoot() + "/include/AutoLink.h");

    // ArqCache.h no longer declares WINDOW.
    // The cache is pure storage; the flow
    // controller owns the window.
    assert(arqH.find("static constexpr int WINDOW = ") == std::string::npos);

    // ArqCache ctor takes a window parameter.
    // Default-arg fallback lets tests construct
    // a cache without specifying one.
    assert(arqH.find("ArqCache(int window") != std::string::npos);

    // ArqCache.cpp ctor validates the guard.
    // On the host build: assert(POOL_SIZE >= window * 2).
    // On the device build: error log + silent-drop
    // (the runtime form, since static_assert
    // can't see the ctor argument).
    assert(arqCpp.find("POOL_SIZE < window * 2") != std::string::npos ||
           arqCpp.find("POOL_SIZE >= window * 2") != std::string::npos);

    // Ping.h declares WINDOW itself (not
    // re-exported from ArqCache). The value
    // must read from the autolink-wide constant
    // in AutoLinkConfig.h, not from ArqCache.
    assert(pingH.find("WINDOW = AUTOLINK_ARQ_PIPELINE_WINDOW") !=
           std::string::npos);
    assert(pingH.find("ArqCache::WINDOW") == std::string::npos);

    // The pipeline window lives as a
    // project-wide constant in AutoLinkConfig.h.
    assert(cfgH.find("AUTOLINK_ARQ_PIPELINE_WINDOW") != std::string::npos);

    // AutoLink constructs the cache with the
    // pipeline window. Member-init or ctor body
    // — both are valid.
    assert(alH.find("AUTOLINK_ARQ_PIPELINE_WINDOW") != std::string::npos);

    std::cout << "  PASS (ArqCache has no WINDOW, ctor takes window, "
                 "Ping owns WINDOW, AutoLink forwards it)\n";
}

void test_autolink_config_clamps_allowed_baud_count() {
    std::cout << "\n=== Pin 4: AutoLinkConfig clamps allowedBaudsCount at "
                 "construction (and exposes a validating accessor) ==="
              << std::endl;
    std::string cfgH = readFile(projectRoot() + "/src/al/AutoLinkConfig.h");
    std::string alCpp = readFile(projectRoot() + "/src/AutoLink.cpp");
    std::string alH = readFile(projectRoot() + "/include/AutoLink.h");

    // AutoLinkConfig exposes a clamp
    // helper and a safe accessor.
    assert(cfgH.find("clampToMaxBauds()") != std::string::npos);
    assert(cfgH.find("allowedBaudSafe") != std::string::npos);

    // Both AutoLink ctors (production and
    // host-test) call clampToMaxBauds()
    // before passing the cfg to the HAL/Link.
    assert(alCpp.find("cfg.clampToMaxBauds()") != std::string::npos);
    assert(alH.find("cfg.clampToMaxBauds()") != std::string::npos);

    std::cout << "  PASS (clampToMaxBauds + allowedBaudSafe present; "
                 "both AutoLink ctors call clamp)\n";
}

void test_test_forward_resync_evicted_from_user_config() {
    std::cout
        << "\n=== Pin 5: _test_forwardResync evicted from AutoLinkConfig; "
           "moved to LinkTestAccessor toggle ==="
        << std::endl;
    std::string cfgH = readFile(projectRoot() + "/src/al/AutoLinkConfig.h");
    std::string linkH = readFile(projectRoot() + "/src/al/link/Link.h");
    std::string linkRx = readFile(projectRoot() + "/src/al/link/LinkRx.cpp");
    std::string accH =
        readFile(projectRoot() + "/test/common/LinkTestAccessor.h");

    // AutoLinkConfig no longer carries the
    // user-facing test flag.
    assert(cfgH.find("_test_forwardResync") == std::string::npos);

    // Link holds a private member, set via
    // the test accessor.
    assert(linkH.find("testForwardResync_") != std::string::npos);

    // LinkRx reads the Link member, not the
    // removed config field.
    assert(linkRx.find("testForwardResync_") != std::string::npos);
    assert(linkRx.find("cfg._test_forwardResync") == std::string::npos);

    // LinkTestAccessor exposes the toggle.
    assert(accH.find("setForwardResync") != std::string::npos);

    std::cout << "  PASS (config flag gone, member + accessor on Link side)\n";
}

void test_chunk_pool_static_assert_uses_named_constants() {
    std::cout
        << "\n=== Pin 6: chunk/pool static_assert uses MAX_CHUNK + MSG_HDR "
           "and ArqCache::POOL_BUF_MAX (no magic literals) ==="
        << std::endl;
    // The post-fix shape lives in every TU
    // that includes LinkContext.h and
    // ArqCache.h. Check the four TUs that
    // assert on the chunk/pool coupling.
    const char *tus[] = {
        "src/al/link/LinkCore.cpp",   "src/al/link/LinkTx.cpp",
        "src/al/link/LinkRx.cpp",     "src/al/link/LinkSweep.cpp",
        "src/al/link/LinkTimers.cpp", "src/al/link/LinkApi.cpp",
    };
    for (const char *rel : tus) {
        std::string src = readFile(projectRoot() + "/" + rel);
        assert(!src.empty());
        // The pre-fix shape used the magic
        // literal `6` and `256`. Either of
        // those in the chunk/pool assert
        // means the fix was reverted.
        assert(src.find("MAX_CHUNK + 6 <= 256") == std::string::npos);
        // The post-fix shape references both
        // MAX_CHUNK + MSG_HDR and
        // ArqCache::POOL_BUF_MAX by name.
        assert(src.find("MAX_CHUNK + MSG_HDR") != std::string::npos);
        assert(src.find("ArqCache::POOL_BUF_MAX") != std::string::npos);
    }

    std::cout << "  PASS (all six TUs use named constants; no magic "
                 "256/6)\n";
}

} // namespace

int main() {
    std::cout << "=== Running Boundary Invariants Suite ===" << std::endl;
    test_esphal_setSpd_drains_tx_before_retune();
    test_ihal_no_link_pointer_and_dispatches_via_ilinkevents();
    test_window_owned_by_ping_cache_validates_injected_window();
    test_autolink_config_clamps_allowed_baud_count();
    test_test_forward_resync_evicted_from_user_config();
    test_chunk_pool_static_assert_uses_named_constants();
    std::cout << "\n=== Boundary Invariants Suite PASS ===" << std::endl;
    return 0;
}

#endif
