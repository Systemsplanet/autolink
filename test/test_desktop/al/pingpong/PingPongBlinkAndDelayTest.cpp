// Source-level pins for the ping-blink-on-send,
// pong-blink-on-recv, and HTML GUI default delay-ms
// fixes. All three changes are tiny shape-level fixes
// that don't need a runtime test (the runtime path is
// already exercised by WireSimClosedLoopTest's blink
// accounting) — the regression risk is a future
// refactor that drops the blinkWait call site or
// reverts the default selection. Source-grep locks
// the shape.
//
// Pins:
//   1. Ping.h calls base_.comm_.blinkWait(1) inside
//      the send loop, after a successful sendMsg.
//   2. Pong.h calls base_.comm_.blinkWait(1) inside
//      the recv loop (already present pre-this-release;
//      lock the shape).
//   3. dashboard_html_part_b.html's <select id="delayMs">
//      has the option value="50" with the `selected`
//      attribute.
//   4. AutoLinkConfig::txDelayMs default == 50.
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

void test_ping_blink_on_send() {
    std::cout << "\n=== Pin 1: Ping.h blinks once per successful send ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/pingpong/Ping.h");
    assert(!src.empty());
    // The send loop builds queue_[tail_], then advances
    // tail_ / count_. The blinkWait(1) call must sit
    // between queue_[tail_].len = n and the txDelayMs
    // timestamp update.
    auto sendStart = src.find("if (!base_.comm_.sendMsg(sendBuf_");
    assert(sendStart != std::string::npos);
    auto sendEnd =
        src.find("if (fillMode_ == FillMode::SEQUENTIAL)", sendStart);
    assert(sendEnd != std::string::npos);
    std::string slice = src.substr(sendStart, sendEnd - sendStart);
    assert(slice.find("base_.comm_.blinkWait(1)") != std::string::npos &&
           "send loop must blink once per successful sendMsg");
    std::cout << "  PASS (base_.comm_.blinkWait(1) inside send loop)"
              << std::endl;
}

void test_pong_blink_on_recv() {
    std::cout
        << "\n=== Pin 2: Pong.h blinks once per valid message received ==="
        << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/pingpong/Pong.h");
    assert(!src.empty());
    auto p = src.find("while ((n = base_.comm_.recv(base_.buf_");
    assert(p != std::string::npos);
    // Walk forward to find the recv loop's tail. The
    // blink must be inside the loop, not before it.
    auto end = src.find("if (n < 0)", p);
    assert(end != std::string::npos);
    std::string slice = src.substr(p, end - p);
    assert(slice.find("base_.comm_.blinkWait(1)") != std::string::npos &&
           "Pong's recv loop must blink per valid message");
    std::cout << "  PASS (base_.comm_.blinkWait(1) inside Pong recv loop)"
              << std::endl;
}

void test_dashboard_default_delayMs_is_0() {
    std::cout << "\n=== Pin 3: HTML GUI delay-ms default is 0 ===" << std::endl;
    std::string src =
        readFile(projectRoot() + "/src/al/web/dashboard_html_part_b.html");
    assert(!src.empty());
    // The select must have an option with value="0" and
    // the `selected` attribute, and the previous default
    // (value="50" selected / value="100" selected) must
    // be gone.
    auto sel = src.find("<select class=\"ping-only\" id=\"delayMs\"");
    assert(sel != std::string::npos);
    auto selEnd = src.find("</select>", sel);
    assert(selEnd != std::string::npos);
    std::string slice = src.substr(sel, selEnd - sel);
    assert(slice.find("value=\"0\" selected") != std::string::npos &&
           "value=0 must be the selected option in the delay-ms dropdown");
    assert(slice.find("value=\"50\" selected") == std::string::npos &&
           "value=50 must NOT be the selected option");
    assert(slice.find("value=\"100\" selected") == std::string::npos &&
           "value=100 must NOT be the selected option");
    std::cout << "  PASS (HTML default delay-ms is 0)" << std::endl;
}

void test_config_default_txDelayMs_is_0() {
    std::cout << "\n=== Pin 4: AutoLinkConfig::txDelayMs default == 0 ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/AutoLinkConfig.h");
    assert(!src.empty());
    auto p = src.find("int txDelayMs = ");
    assert(p != std::string::npos);
    auto end = src.find(";", p);
    assert(end != std::string::npos);
    std::string slice = src.substr(p, end - p);
    // The RHS must be 0 (and not 50 / 100, the previous defaults).
    assert(slice.find("= 0") != std::string::npos &&
           "txDelayMs default must be 0");
    assert(slice.find("= 50") == std::string::npos &&
           "txDelayMs default must NOT be 50");
    assert(slice.find("= 100") == std::string::npos &&
           "txDelayMs default must NOT be 100");
    std::cout << "  PASS (firmware default txDelayMs is 0)" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running Ping/Pong Blink + Delay Default Tests ==="
              << std::endl;
    test_ping_blink_on_send();
    test_pong_blink_on_recv();
    test_dashboard_default_delayMs_is_0();
    test_config_default_txDelayMs_is_0();
    std::cout << "\n=== Ping/Pong Blink + Delay Default Tests PASS ==="
              << std::endl;
    return 0;
}

#endif // !ARDUINO