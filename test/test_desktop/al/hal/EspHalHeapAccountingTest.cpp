// Regression pins for the EspHal::begin() heap accounting, config
// propagation through begin(cfg), and httpd max_open_sockets = 3.
//
// Three defect classes pinned here:
//
//   1. EspHal::begin(cfg) copied only cfg.mode, not the whole config,
//      into EspHal's own cfg member. EspHal::cfg is a construction-time
//      copy (taken in the EspHal ctor, before AutoLinkWeb exists to cap
//      maxMsg), and the buffer floors are sized from THAT copy. A
//      later AutoLink::setMaxMsg() reaches Link::cfg.maxMsg and
//      AutoLink::cfg_.maxMsg but never EspHal::cfg.maxMsg, so the web
//      monitor's maxMsg cap had no effect on the buffers actually
//      installed — the GUI wedged on a heap the cap was supposed to
//      free. The fix: begin(cfg) assigns EspHal::cfg = c in full
//      before recomputing the floors.
//
//   2. EspHal::begin() over-committed heap. Calling
//      capFloorByHeap for streamBuf/rxBuf/txBuf with the SAME
//      cfg.heapReserveBytes as the reserve at every call, against an
//      already-shrunk running total — once any earlier buffer
//      clamped, the running total converged to exactly `reserve`, and
//      whichever buffer was sized last (tx) computed
//      avail = reserve - reserve = 0 regardless of genuine
//      availability. The fix (distributeHeapBudget() in
//      AutoLinkConfig.h) takes the reserve once up front and
//      distributes the remainder across the three buffers in order,
//      falling to 0 for a buffer only when even its own floor can't
//      be met from what's left. EspHal::begin() and this test both
//      call that one function — not two separate reimplementations
//      of the same arithmetic, which is how defect 1's cousin (the
//      heap math itself) went uncaught in an earlier release: the
//      test asserted against a hand-copied reimplementation of
//      begin()'s arithmetic, not the code that actually ran.
//
//   3. httpd max_open_sockets = 7 is over-spec for a heap-constrained
//      device. A browser opening index + /stats polling + /logs
//      needs three; seven is sized for headroom the device doesn't
//      have. Kept at 3.
//
// Pins:
//   A. EspHal::begin(const AutoLinkConfig&) must assign the WHOLE
//      config (cfg = c) before recomputing floors, not just the mode.
//      A refactor that narrows this back to setMode(c.mode) alone
//      flips this red — and silently reintroduces defect 1, since
//      nothing else here can exercise EspHal::begin() itself (it
//      needs real ESP-IDF driver calls the host build doesn't have;
//      AGENTS.md's host/hardware boundary applies here as it does to
//      the rest of EspHal.cpp).
//   B. EspHal::begin()'s heap accounting must call
//      distributeHeapBudget(cfg, ...) — a refactor that reintroduces
//      a hand-rolled three-call capFloorByHeap sequence (defect 2's
//      shape) flips this red.
//   C. EspHal::begin() must abort with the "post-allocation free
//      heap" error message when freeH is below the serviceable
//      floor.
//   D. The "begin:" info line must include a post-alloc free heap
//      token (e.g. `post-alloc free=%u`).
//   E. AutoLinkWebHttpd.cpp must set max_open_sockets = 3 (or a
//      small fixed value), not 7.
//   F. distributeHeapBudget() itself, exercised directly (not via
//      source-grep) against the field-numbers scenario, both with
//      and without the web monitor's maxMsg cap.
//   G. AutoLinkWeb's maxMsg cap value is 2048.
//
// A-C, D-E, G are source-grep pins (necessary given the host/hardware
// split — see A's comment). F is a true behavioural pin against the
// shared production function.
#ifndef ARDUINO

#    include <algorithm>
#    include <cassert>
#    include <cctype>
#    include <cstdint>
#    include <cstdlib>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>

#    include "al/AutoLinkConfig.h"

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

std::string flatten(std::string s) {
    std::string out;
    out.reserve(s.size());
    bool prevSpace = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!prevSpace)
                out += ' ';
            prevSpace = true;
        } else {
            out += c;
            prevSpace = false;
        }
    }
    return out;
}

std::string extractFnBody(const std::string &src, const std::string &sig) {
    auto p = src.find(sig);
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

void test_pin_begin_cfg_adopts_whole_config() {
    std::cout << "\n=== Pin A: EspHal::begin(cfg) adopts the whole config, "
                 "not just mode ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/hal/EspHal.h");
    assert(!src.empty());
    std::string body =
        extractFnBody(src, "void begin(const AutoLinkConfig &c) override {");
    assert(!body.empty() &&
           "EspHal must declare begin(const AutoLinkConfig&) inline in "
           "the header");
    std::string flat = flatten(stripComments(body));
    auto assignPos = flat.find("cfg = c;");
    assert(assignPos != std::string::npos &&
           "EspHal::begin(cfg) must assign cfg = c (the whole config) — "
           "assigning only the mode (setMode(c.mode) alone) leaves "
           "EspHal::cfg.maxMsg stale at whatever the EspHal constructor "
           "saw, so a maxMsg cap installed via AutoLink::setMaxMsg() "
           "after EspHal construction never reaches the buffer floors. "
           "This is the defect that let the web monitor's maxMsg cap "
           "silently fail to free any heap.");
    auto setModePos = flat.find("setMode(c.mode)", assignPos);
    assert(setModePos != std::string::npos && setModePos > assignPos &&
           "cfg = c must come BEFORE setMode(c.mode), so setMode()'s "
           "floor recompute sees the caller's real maxMsg rather than "
           "the stale construction-time value");
    std::cout << "  PASS (cfg = c precedes setMode(c.mode))" << std::endl;
}

void test_pin_esphal_begin_uses_shared_distribution_fn() {
    std::cout << "\n=== Pin B: EspHal::begin() calls distributeHeapBudget() "
                 "===\n";
    std::string src = readFile(projectRoot() + "/src/al/hal/EspHal.cpp");
    assert(!src.empty());
    std::string body = extractFnBody(src, "void EspHal::begin() {");
    assert(!body.empty());
    std::string code = stripComments(body);
    assert(code.find("distributeHeapBudget(cfg,") != std::string::npos &&
           "EspHal::begin() must call distributeHeapBudget(cfg, ...) — "
           "the single shared function this test also calls directly in "
           "Pin F. A hand-rolled sequence of capFloorByHeap calls "
           "against a running total (the earlier bug shape) re-charges "
           "the reserve at every step and starves whichever buffer is "
           "sized last.");
    std::cout << "  PASS (begin() calls distributeHeapBudget)" << std::endl;
}

void test_pin_esphal_begin_aborts_below_serviceable_floor() {
    std::cout << "\n=== Pin C: EspHal::begin() aborts when post-alloc freeH "
                 "is below the serviceable floor ===\n";
    std::string src = readFile(projectRoot() + "/src/al/hal/EspHal.cpp");
    assert(!src.empty());
    std::string body = extractFnBody(src, "void EspHal::begin() {");
    assert(!body.empty());
    std::string code = stripComments(body);
    assert(code.find("kServiceableFloor") != std::string::npos &&
           "EspHal::begin() must define a serviceable-floor constant "
           "and gate on it");
    assert(code.find("post-allocation free heap") != std::string::npos &&
           "the post-alloc gate must log 'post-allocation free heap'");
    std::cout << "  PASS (begin() aborts on post-alloc free heap below floor)"
              << std::endl;
}

void test_pin_esphal_begin_logs_post_alloc_free_heap() {
    std::cout << "\n=== Pin D: EspHal::begin()'s info line includes a "
                 "post-alloc free heap token ===\n";
    std::string src = readFile(projectRoot() + "/src/al/hal/EspHal.cpp");
    assert(!src.empty());
    std::string body = extractFnBody(src, "void EspHal::begin() {");
    assert(!body.empty());
    assert(body.find("post-alloc free=") != std::string::npos &&
           "the 'begin:' info line must include a 'post-alloc free=' "
           "token so the operator can see what the three caps left for "
           "the rest of the system");
    std::cout << "  PASS (begin() info line includes post-alloc free)"
              << std::endl;
}

void test_pin_httpd_max_open_sockets_is_small() {
    std::cout << "\n=== Pin E: AutoLinkWeb sets httpd max_open_sockets <= 4 "
                 "(not 7) ===\n";
    std::string src =
        readFile(projectRoot() + "/src/al/web/handlers/AutoLinkWebHttpd.cpp");
    assert(!src.empty());
    std::string code = stripComments(src);
    auto pos = code.find("cfg.max_open_sockets = ");
    assert(pos != std::string::npos);
    std::string rhsSlice = code.substr(pos, 64);
    auto eqPos = rhsSlice.find('=');
    assert(eqPos != std::string::npos);
    std::string rhs = rhsSlice.substr(eqPos + 1);
    std::string digits;
    for (char c : rhs) {
        if (c >= '0' && c <= '9')
            digits += c;
        else if (!digits.empty())
            break;
    }
    assert(!digits.empty());
    int n = std::stoi(digits);
    assert(n <= 4 && n >= 2 &&
           "max_open_sockets must be in [2..4] — 3 for index + /stats + "
           "/logs; 7 was sized for headroom a heap-constrained device "
           "doesn't have");
    std::cout << "  PASS (max_open_sockets=" << n << ", in [2..4])"
              << std::endl;
}

// ============================================================================
// Pin F — runtime contract: distributeHeapBudget() against the
// field-numbers scenario, called directly (the same function
// EspHal::begin() calls — see Pin B).
//
// Field numbers reported from a FireBeetle-2 ESP32-E running
// AutoLinkWeb + WiFi + httpd: freeAtBegin=41996 (post-WiFi,
// pre-AutoLink), mode=ASYNC. The link layer's default maxMsg is
// 5120; the web monitor's ctor caps it to 2048 (see
// AutoLinkWeb::setLinkMaxMsg / the ctor) — PROVIDED the cap actually
// reaches EspHal::cfg.maxMsg via begin(cfg), which is what Pin A
// guards.
// ============================================================================
void test_pin_field_numbers_post_alloc_above_serviceable_floor() {
    std::cout << "\n=== Pin F: field-numbers scenario via "
                 "distributeHeapBudget() ===\n";
    constexpr size_t kServiceableFloor = 20 * 1024;
    const size_t freeAtBegin = 41996;

    // ---- 1. Uncapped (maxMsg = 5120): genuinely insufficient ----
    // Without the web monitor's cap this heap cannot serve a
    // 5120-byte maxMsg AND leave 20 KB for LWIP/httpd/WiFi — this is
    // a real shortfall, not an accounting artifact, and
    // distributeHeapBudget() must report it as one (0, not a
    // partial/garbage value) rather than silently degrading.
    {
        AutoLinkConfig cfg;
        cfg.mode = AutoLinkConfig::Mode::ASYNC;
        cfg.maxMsg = 5120;
        HeapDistribution d = distributeHeapBudget(cfg, freeAtBegin);
        std::cout << "  uncapped (maxMsg=5120): streamBuf=" << d.streamBuf
                  << " rxBuf=" << d.rxBuf << " txBuf=" << d.txBuf
                  << " post-alloc free=" << d.postFree << std::endl;
        assert(d.txBuf == 0 &&
               "an uncapped 5120-byte maxMsg against a 41996-byte free "
               "heap must report the shortfall as txBuf=0 (genuinely "
               "insufficient), which EspHal::begin() turns into a "
               "loud abort — not a partial ring that looks like success "
               "and wedges later");
    }

    // ---- 2. Capped (maxMsg = 2048, matching AutoLinkWeb's default):
    // must clear the serviceable floor ----
    {
        AutoLinkConfig cfg;
        cfg.mode = AutoLinkConfig::Mode::ASYNC;
        cfg.maxMsg = 2048;
        HeapDistribution d = distributeHeapBudget(cfg, freeAtBegin);
        std::cout << "  capped (maxMsg=2048):   streamBuf=" << d.streamBuf
                  << " rxBuf=" << d.rxBuf << " txBuf=" << d.txBuf
                  << " post-alloc free=" << d.postFree
                  << " (need >= " << kServiceableFloor << ")" << std::endl;
        assert(d.streamBuf > 0 && d.rxBuf > 0 && d.txBuf > 0 &&
               "with the web monitor's maxMsg=2048 cap, none of the "
               "three buffers should be starved to 0 on a 41996-byte "
               "free heap");
        assert(d.postFree >= kServiceableFloor &&
               "with the maxMsg=2048 cap, the field device's post-alloc "
               "free heap must be >= the 20 KB serviceable floor. If "
               "this goes red while Pin A is green, the cap reached "
               "EspHal but the distribution math itself regressed.");
    }
    std::cout << "  PASS (uncapped reports the real shortfall; capped "
                 "clears the floor)"
              << std::endl;
}

void test_pin_autolink_web_maxmsg_cap_is_2048() {
    std::cout << "\n=== Pin G: AutoLinkWeb's maxMsg cap is 2048 (not 4096+) "
                 "===\n";
    std::string src = readFile(projectRoot() + "/src/al/web/AutoLinkWeb.h");
    assert(!src.empty());
    auto pos = src.find("kDefaultWebMaxMsgCap = ");
    assert(pos != std::string::npos);
    std::string rhsSlice = src.substr(pos, 80);
    auto eqPos = rhsSlice.find('=');
    assert(eqPos != std::string::npos);
    std::string rhs = rhsSlice.substr(eqPos + 1);
    std::string digits;
    for (char c : rhs) {
        if (c >= '0' && c <= '9')
            digits += c;
        else if (!digits.empty())
            break;
    }
    assert(!digits.empty());
    int n = std::stoi(digits);
    assert(n == 2048 &&
           "kDefaultWebMaxMsgCap must be 2048 — see Pin F for the "
           "arithmetic that value is based on");
    std::cout << "  PASS (kDefaultWebMaxMsgCap=" << n << ")" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== EspHal Heap Accounting + Config Propagation + httpd "
                 "max_open_sockets pins ===\n";
    test_pin_begin_cfg_adopts_whole_config();
    test_pin_esphal_begin_uses_shared_distribution_fn();
    test_pin_esphal_begin_aborts_below_serviceable_floor();
    test_pin_esphal_begin_logs_post_alloc_free_heap();
    test_pin_httpd_max_open_sockets_is_small();
    test_pin_field_numbers_post_alloc_above_serviceable_floor();
    test_pin_autolink_web_maxmsg_cap_is_2048();
    std::cout << "\n=== EspHalHeapAccountingTest completed ===" << std::endl;
    return 0;
}

#endif
