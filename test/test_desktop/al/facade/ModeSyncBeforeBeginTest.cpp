// Regression pin: AutoLink::setMode(m) must update cfg_.mode, and
// AutoLink::begin() must not revert a deliberately-set mode.
//
// Defect class — facade cfg stale:
//   1. setMode(ASYNC) called pre-begin. link->mode() = ASYNC, hal->mode()
//      = ASYNC, but cfg_.mode stayed at the construction-time SYNC.
//   2. begin() logged "mode=SYNC" from the stale cfg_.mode, called
//      hal->setMode(cfg_.mode) which actively reverted the HAL back to
//      SYNC, then compared cfg_.mode against hal->getMode() and fired a
//      "mode mismatch" error on what was actually a healthy, fully
//      ASYNC-sized configuration.
//
// The fix:
//   - AutoLink::setMode() now also writes cfg_.mode = m.
//   - AutoLink::begin() no longer calls hal->setMode() at all (Link::begin
//     begins the HAL itself, from the link's config, and the link's config
//     already carries the mode setMode installed).
//   - The mismatch check now compares hal->getMode() against link->mode(),
//     not cfg_.mode, because cfg_.mode is no longer the source of truth —
//     link->mode() is what the buffers were sized for.
//
// The runtime check constructs a facade, calls setMode(ASYNC) (default
// Arduino-build cfg is SYNC; flip to ASYNC to make the change observable),
// then begin(), and asserts: facade cfg_.mode == ASYNC, link->mode() ==
// ASYNC, hal->getMode() == ASYNC, the logged "begin: starting" line says
// ASYNC, and no "mode mismatch at begin" error line was emitted.
//
// The source-grep pins catch a refactor that re-introduces the bug class:
//   - cfg_.mode = m must be in setMode's body (the actual root-cause fix).
//   - hal->setMode(cfg_.mode) must NOT be in begin's body (the revert).
//   - The mismatch check must read link->mode(), not cfg_.mode.
//
// Toggle each off individually -> the corresponding pin goes red.
#ifndef ARDUINO

#    include <cassert>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>
#    include <vector>

#    include "al/AutoLinkConfig.h"
#    include "al/hal/IHal.h"
#    include "al/link/Link.h"
#    include "al/util/log/Log.h"
#    include "AutoLink.h"
#    include "AutoLinkTestAccessor.h"
#    include "LinkTestAccessor.h"
#    include "MockHal.h"

using namespace autolink;

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

struct CapturedLine {
    char sev;
    std::string tag;
    std::string msg;
};
static std::vector<CapturedLine> g_captured;
static void captureSink(char sev, const char *tag, const char *msg, void *) {
    g_captured.push_back({ sev, tag ? tag : "", msg ? msg : "" });
}

bool containsModeMismatchError() {
    for (const auto &l : g_captured) {
        if (l.sev == 'E' && l.tag == "AutoLink" &&
            l.msg.find("mode mismatch at begin") != std::string::npos)
            return true;
    }
    return false;
}

std::string beginStartingLine() {
    for (const auto &l : g_captured) {
        if (l.tag == "AutoLink" &&
            l.msg.find("begin: starting") != std::string::npos)
            return l.msg;
    }
    return "";
}

// Source-grep helpers --------------------------------------------------------

std::string extractFunctionBody(const std::string &src,
                                const std::string &signature) {
    auto p = src.find(signature);
    if (p == std::string::npos)
        return "";
    auto brace = src.find('{', p);
    if (brace == std::string::npos)
        return "";
    int depth = 0;
    bool opened = false;
    size_t endPos = std::string::npos;
    for (size_t i = brace; i < src.size(); i++) {
        if (src[i] == '{') {
            depth++;
            opened = true;
        } else if (src[i] == '}') {
            depth--;
            if (opened && depth == 0) {
                endPos = i;
                break;
            }
        }
    }
    if (endPos == std::string::npos)
        return "";
    return src.substr(p, endPos - p + 1);
}

std::string stripComments(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n')
                i++;
        } else {
            out += s[i++];
        }
    }
    return out;
}

// ============================================================================
// Pin 1 — runtime: setMode(ASYNC) → begin() must leave facade, link, HAL,
// and the log all reporting ASYNC, and must not log a mode-mismatch error.
// ============================================================================
void test_pin_setMode_async_then_begin_agrees_everywhere() {
    std::cout << "\n=== Pin 1: setMode(ASYNC) -> begin() agrees across "
                 "facade / link / HAL / log, no error ==="
              << std::endl;
    Log &L = Log::log();
    L.setLevel(Log::Level::INFO);
    L.setSink(captureSink);
    g_captured.clear();

    // Default Arduino build is SYNC. We want a visible mode
    // change, so flip to ASYNC and pin the post-begin agreement.
    // The host ctor that takes a HAL is the one that lets us
    // observe hal->getMode() — the production ctor (uart_port_t,
    // rx_pin, tx_pin, ...) wraps EspHal, whose host stub doesn't
    // track mode. MockHal does.
    AutoLinkConfig cfg;
    cfg.mode = AutoLinkConfig::Mode::SYNC;
    MockHal mockHal;
    AutoLink link(mockHal, true, cfg);
    AutoLinkTestAccessor t(link);
    assert(link.mode() == AutoLinkConfig::Mode::SYNC);
    assert(mockHal.getMode() == AutoLinkConfig::Mode::SYNC);

    link.setMode(AutoLinkConfig::Mode::ASYNC);
    assert(link.mode() == AutoLinkConfig::Mode::ASYNC);
    assert(mockHal.getMode() == AutoLinkConfig::Mode::ASYNC);

    // begin() — the runtime that exposes the bug class.
    // Host build has no real UART, so begin() returns the
    // best it can; what we pin is the *agreement* and the
    // *absence of the error*, not the boolean.
    (void)link.begin();

    // (a) link layer carries ASYNC.
    assert(t.link()->mode() == AutoLinkConfig::Mode::ASYNC &&
           "link->mode() must read ASYNC after setMode(ASYNC) + begin()");
    // (b) facade facade level reports ASYNC. mode() reads
    //     from link, so this is a re-statement of (a) but
    //     it's the surface the app sees.
    assert(link.mode() == AutoLinkConfig::Mode::ASYNC &&
           "AutoLink::mode() must report ASYNC after setMode(ASYNC) + begin()");
    // (c) HAL's view agrees. The link owns the HAL
    //     reference (Link::hw) and forwards setMode to
    //     it; the facade's setMode in turn called
    //     link->setMode. A healthy ASYNC handoff leaves
    //     hw.getMode() == ASYNC.
    IHal &hal =
        LinkTestAccessor(*AutoLinkTestAccessor(link).link()).hwForTest();
    assert(hal.getMode() == AutoLinkConfig::Mode::ASYNC &&
           "Link::hw.getMode() must report ASYNC after "
           "setMode(ASYNC) + begin() — the link layer was sized for "
           "ASYNC, and the HAL must agree or the buffers were sized "
           "for the wrong mode.");
    // (d) The "begin: starting" log line reports ASYNC,
    //     read from a source that's in sync with
    //     link->mode() (the bug was the line reading the
    //     stale cfg_.mode).
    std::string beginLine = beginStartingLine();
    assert(!beginLine.empty() && "begin() must log the 'begin: starting' line");
    assert(beginLine.find("mode=ASYNC") != std::string::npos &&
           "the 'begin: starting' log line must report mode=ASYNC "
           "(the bug had it read the stale cfg_.mode and report SYNC)");
    // (e) No "mode mismatch at begin" error fired. The
    //     error fires when hal->getMode() !=
    //     <compared-against>; with the fix, the
    //     compared-against is link->mode(), so they
    //     agree and no error fires.
    assert(!containsModeMismatchError() &&
           "begin() must not log a 'mode mismatch at begin' error "
           "after setMode(ASYNC) + begin() — the bug had the error "
           "fire on a healthy configuration because cfg_.mode was "
           "stale and the check compared against it instead of "
           "link->mode()");

    L.clearSink();
    std::cout << "  PASS (facade=ASYNC link=ASYNC log=ASYNC, no error)"
              << std::endl;
}

// ============================================================================
// Pin 2 — source-grep: AutoLink::setMode() must update cfg_.mode. The
// actual root-cause fix.
// ============================================================================
void test_pin_setMode_writes_cfg_mode() {
    std::cout << "\n=== Pin 2: AutoLink::setMode() writes cfg_.mode = m ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/include/AutoLink.h");
    assert(!src.empty());
    std::string body =
        extractFunctionBody(src, "void setMode(AutoLinkConfig::Mode m)");
    assert(!body.empty());
    std::string code = stripComments(body);
    assert(code.find("cfg_.mode = m") != std::string::npos &&
           "AutoLink::setMode() must write cfg_.mode = m — the facade's "
           "begin() log and (historically) the begin-time HAL re-set both "
           "read from cfg_, so leaving it stale produces a misleading "
           "boot log and an active mode revert. The Pin 1 runtime check "
           "depends on this write; toggling it off turns Pin 1 red.");
    std::cout << "  PASS (cfg_.mode = m is in setMode's body)" << std::endl;
}

// ============================================================================
// Pin 3 — source-grep: AutoLink::begin() must NOT call hal->setMode().
// The call actively reverts a mode the app deliberately installed via
// setMode() before begin().
// ============================================================================
void test_pin_begin_does_not_revert_hal_mode() {
    std::cout << "\n=== Pin 3: AutoLink::begin() does not call "
                 "hal->setMode(cfg_.mode) ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/include/AutoLink.h");
    assert(!src.empty());
    std::string body = extractFunctionBody(src, "bool begin() {");
    assert(!body.empty());
    std::string code = stripComments(body);
    assert(code.find("hal->setMode(cfg_.mode)") == std::string::npos &&
           "AutoLink::begin() must not call hal->setMode(cfg_.mode) — "
           "cfg_.mode is a construction-time snapshot, not the live "
           "mode; re-applying it on begin() silently reverts any mode "
           "the app installed via setMode() before begin(). Link::begin() "
           "begins the HAL from the link's own config (which IS the live "
           "mode), so the explicit facade-side handoff is both wrong and "
           "redundant.");
    std::cout << "  PASS (begin's body has no hal->setMode(cfg_.mode) call)"
              << std::endl;
}

// ============================================================================
// Pin 4 — source-grep: the mode-mismatch check must compare
// hal->getMode() against link->mode(), not cfg_.mode. cfg_.mode was the
// stale value; link->mode() is what the buffers were sized for.
// ============================================================================
void test_pin_mismatch_check_uses_link_mode() {
    std::cout << "\n=== Pin 4: mode-mismatch check compares hal vs link, "
                 "not hal vs cfg ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/include/AutoLink.h");
    assert(!src.empty());
    std::string body = extractFunctionBody(src, "bool begin() {");
    assert(!body.empty());
    std::string code = stripComments(body);
    // The check must reference link->mode() — both sides
    // of the comparison, in some order. cfg_.mode must
    // not appear in the comparison (it may still appear
    // in the log line, which is fine).
    assert(code.find("link->mode()") != std::string::npos &&
           "AutoLink::begin()'s mode-mismatch check must reference "
           "link->mode() — the link's mode is the live value the "
           "buffers were sized for, the facade's cfg_.mode is a "
           "construction-time snapshot.");
    // The comparison must NOT condition on cfg_.mode. Find
    // the mismatch branch (the if) and assert no
    // cfg_.mode in its condition.
    auto ifPos = code.find("if (hal && hal->getMode()");
    assert(ifPos != std::string::npos &&
           "begin() must contain the "
           "`if (hal && hal->getMode() != link->mode())` check — "
           "this is the disagreement guard that catches a custom HAL "
           "that ignored setMode(). Re-introducing the cfg_.mode "
           "comparison is the bug class we are pinning against.");
    // The check must NOT use cfg_.mode. Locate the
    // comparison's RHS and assert it is link->mode().
    auto rhs = code.find("!=", ifPos);
    assert(rhs != std::string::npos);
    auto paren = code.find(')', rhs);
    assert(paren != std::string::npos);
    std::string cond = code.substr(ifPos, paren - ifPos + 1);
    assert(cond.find("cfg_.mode") == std::string::npos &&
           "the mode-mismatch check's condition must NOT reference "
           "cfg_.mode — that was the bug. The facade's cfg_ is a "
           "construction-time snapshot; the link's mode is the live "
           "value the buffers match against.");
    std::cout << "  PASS (check uses link->mode() and ignores cfg_.mode)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== ModeSyncBeforeBegin Regression (setMode/begin "
                 "agreement) ==="
              << std::endl;
    test_pin_setMode_async_then_begin_agrees_everywhere();
    test_pin_setMode_writes_cfg_mode();
    test_pin_begin_does_not_revert_hal_mode();
    test_pin_mismatch_check_uses_link_mode();
    std::cout << "\n=== ModeSyncBeforeBeginTest completed ===" << std::endl;
    return 0;
}

#endif
