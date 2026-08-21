// Source-level regression test for Fix 6: a delivered BREAK during
// SWP ran uart_flush_input before onBreak() — and onBreak()
// returns early in any non-OK state — so the flush destroyed
// in-flight sweep frames with no compensating recovery logic. The
// guard source (the peer's own setSpd transition) is different from
// POST_SETSPD_BREAK_GUARD_MS, which only covers *local* setSpd.
//
// Fix: mirror Link's OK/SWP state to the HAL via a volatile field
// (EspHal.h's `okState`, set through IHal::setOkState from
// Link::changeState_unlocked — the UART event task runs outside
// Link's lock and can't read Link::state directly) and skip the
// flush on a delivered BREAK while not OK.
//
// EspHal.h/EspHalUartEvent.h are ESP32-only and cannot compile or
// run in the host suite (no Arduino/ESP-IDF here) — this pin is
// necessarily source-level, matching every other EspHal fix pinned
// in this directory. Revert any of the four pieces below -> red.
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

// Pin 1: IHal declares the setOkState hook (default no-op, so
// MockHal and every other IHal implementation need no changes).
void test_ihal_declares_set_ok_state() {
    std::cout << "\n=== Pin 1: IHal::setOkState hook declared ===" << std::endl;
    std::string ihal = readFile(projectRoot() + "/src/al/hal/IHal.h");
    assert(!ihal.empty());
    assert(ihal.find("virtual void setOkState(bool") != std::string::npos &&
           "IHal must declare setOkState so Link can mirror its state "
           "to the HAL without every IHal implementation needing an "
           "override");
    std::cout << "  PASS" << std::endl;
}

// Pin 2: EspHal carries the volatile okState field and overrides
// setOkState to write it — same shape as the pre-existing `running`
// flag, so the UART event task (a plain C function, no `this`) can
// read it through the `hal` pointer without locking.
void test_esphal_has_ok_state_field_and_override() {
    std::cout << "\n=== Pin 2: EspHal has volatile okState + override ==="
              << std::endl;
    std::string h = readFile(projectRoot() + "/src/al/hal/EspHal.h");
    assert(!h.empty());
    assert(h.find("volatile bool okState") != std::string::npos &&
           "EspHal must carry a volatile okState field readable from "
           "the UART event task without Link's lock");
    assert(h.find("void setOkState(bool ok) override") != std::string::npos &&
           "EspHal must override setOkState to write okState");
    std::cout << "  PASS" << std::endl;
}

// Pin 3: Link::changeState_unlocked calls hw.setOkState on every
// transition — the only place Link's state changes.
void test_change_state_calls_set_ok_state() {
    std::cout << "\n=== Pin 3: changeState_unlocked propagates to the HAL ==="
              << std::endl;
    std::string core = readFile(projectRoot() + "/src/al/link/LinkCore.cpp");
    assert(!core.empty());
    auto fnPos = core.find("void Link::changeState_unlocked(");
    assert(fnPos != std::string::npos);
    auto brace = core.find('{', fnPos);
    assert(brace != std::string::npos);
    int depth = 0;
    bool opened = false;
    size_t endPos = std::string::npos;
    for (size_t i = brace; i < core.size(); i++) {
        if (core[i] == '{') {
            depth++;
            opened = true;
        } else if (core[i] == '}') {
            depth--;
            if (opened && depth == 0) {
                endPos = i;
                break;
            }
        }
    }
    assert(endPos != std::string::npos);
    std::string body = core.substr(fnPos, endPos - fnPos + 1);
    assert(body.find("hw.setOkState(newState == State::OK)") !=
               std::string::npos &&
           "changeState_unlocked must call hw.setOkState on every "
           "transition, or the HAL's mirror of Link's state goes "
           "stale and the BREAK-flush guard reads a wrong value");
    std::cout << "  PASS" << std::endl;
}

// Pin 4: AL97-6 moved the raw RX flush out of the UART_BREAK
// handler entirely (it now runs only on link-layer BREAK
// confirmation — see EspHalBreakFlushOnConfirmOnlyTest.cpp). The
// original okState-gate this pin asserted is gone along with the
// call it gated; what remains from this fix is the okState/
// breakWindowEpoch_ wiring itself (Pins 1-3), which other BREAK-
// window bookkeeping still depends on. This pin now asserts the
// handler contains NO uart_flush_input call at all, so a future
// regression that reintroduces flush-on-delivery (gated or not)
// still goes red here.
void test_uart_break_handler_gates_flush_on_ok_state() {
    std::cout << "\n=== Pin 4: UART_BREAK handler no longer flushes RX "
                 "on delivery (moved to link-layer confirm, AL97-6) ==="
              << std::endl;
    std::string ev = readFile(projectRoot() + "/src/al/hal/EspHalUartEvent.h");
    assert(!ev.empty());
    auto breakCase = ev.find("UART_BREAK");
    assert(breakCase != std::string::npos);
    auto onBreakPos = ev.find("hal->events()->onBreak();", breakCase);
    assert(onBreakPos != std::string::npos);
    std::string breakBranch = ev.substr(breakCase, onBreakPos - breakCase);
    assert(breakBranch.find("uart_flush_input") == std::string::npos &&
           "AL97-6: the UART_BREAK handler must not call "
           "uart_flush_input at delivery time any more — see "
           "EspHalBreakFlushOnConfirmOnlyTest.cpp for the replacement "
           "confirm-time flush. Toggle the old delivery-time call "
           "back in -> red.");
    std::cout << "  PASS (flush no longer runs at delivery time)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== EspHal BREAK-flush guard (Fix 6) ===" << std::endl;
    test_ihal_declares_set_ok_state();
    test_esphal_has_ok_state_field_and_override();
    test_change_state_calls_set_ok_state();
    test_uart_break_handler_gates_flush_on_ok_state();
    std::cout << "\nAll EspHalBreakFlushGuard pins passed." << std::endl;
    return 0;
}
#endif
