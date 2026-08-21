// Source-level regression test for AL97-6: EspHalUartEvent.h's
// UART_BREAK case called uart_flush_input() on every DELIVERED
// BREAK — including the first, unconfirmed one, and any
// coalesced-but-past-debounce duplicate that the link's own
// two-frame-clear later resolves as a healthy link, not just a
// confirmed peer drop. A field capture at sustained 512000-baud
// ASYNC traffic showed 3 delivered BREAKs in one session; only the
// third confirmed (state -> SWP). All three flushed the raw UART
// driver RX ring, destroying in-flight bytes for the two glitches
// the link went on to decide were NOT a real drop.
//
// Fix: the raw HAL-level flush (IHal::flushRxHw(), pre-existing
// hook already used at lock time — see SettleGateTest) moves to the
// link layer's two actual BREAK-confirm sites in LinkTimersOk.cpp:
// the timeout-confirm branch (elapsed >= confirmMs) and the
// fast-confirm branch (breakConfirmPending_, consumed at the top of
// onTimer()). EspHalUartEvent.h's okState/breakWindowEpoch_
// bookkeeping is unrelated and unchanged — only the flush call
// moves.
//
// EspHal.h/EspHalUartEvent.h are ESP32-only and cannot compile or
// run in the host suite (no Arduino/ESP-IDF here) — the delivery-
// side pin is necessarily source-level, matching every other EspHal
// fix pinned in this directory (see EspHalBreakFlushGuardTest.cpp).
// The confirm-side pin is also source-level since it's asserting a
// call exists in a specific branch, not a return value — MockHal's
// flushRxHw() is IHal's no-op default with no call counter, so a
// runtime pin here would only prove the no-op ran, not which branch
// called it.
#ifndef ARDUINO

#    include <cassert>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>
#    include "TestPaths.h"

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

// Pin 1: EspHalUartEvent.h's UART_BREAK case no longer calls
// uart_flush_input directly. Toggle back to red by restoring the
// `if (hal->okState) uart_flush_input(hal->uart_num);` line ahead
// of `hal->events()->onBreak();`.
void test_event_hook_does_not_flush_on_delivery() {
    std::cout << "\n=== Pin 1: UART_BREAK delivery no longer flushes RX "
                 "directly ===" << std::endl;
    std::string ev = readFile(testRepoPath("src/al/hal/EspHalUartEvent.h"));
    assert(!ev.empty());
    auto breakCase = ev.find("UART_BREAK");
    assert(breakCase != std::string::npos);
    auto onBreakPos = ev.find("hal->events()->onBreak();", breakCase);
    assert(onBreakPos != std::string::npos);
    std::string breakBranch = ev.substr(breakCase, onBreakPos - breakCase);
    assert(breakBranch.find("uart_flush_input") == std::string::npos &&
           "the UART_BREAK case must not call uart_flush_input directly "
           "any more — a delivered-but-unconfirmed BREAK must not "
           "destroy in-flight RX. The flush moves to the link layer's "
           "confirm sites (LinkTimersOk.cpp), reached via "
           "IHal::flushRxHw().");
    // BREAK-window epoch tracking (detects a peer setSpd
    // transition mid-window) is unrelated and must still be
    // intact — this pin is about the flush call only.
    assert(ev.find("breakWindowEpoch_") != std::string::npos &&
           "breakWindowEpoch_ tracking must remain — it still gates "
           "other BREAK-window bookkeeping, only the flush moved off "
           "of okState");
    std::cout << "  PASS" << std::endl;
}

// Pin 2: the timeout-confirm branch in LinkTimersOk.cpp (elapsed >=
// confirmMs, the genuine-peer-drop verdict that already clears the
// app buffer) also flushes the raw HAL RX via hw.flushRxHw(),
// alongside the existing hw.clearAppBuf() call.
void test_timeout_confirm_flushes_rx_hw() {
    std::cout << "\n=== Pin 2: timeout-confirm branch calls "
                 "hw.flushRxHw() ===" << std::endl;
    std::string src = readFile(testRepoPath("src/al/link/timers/LinkTimersOk.cpp"));
    assert(!src.empty());
    auto confirmPos = src.find("BREAK -> resweep\"");
    assert(confirmPos != std::string::npos);
    auto secondOccurrence =
        src.find("BREAK -> resweep\"", confirmPos + 1);
    // There are two "BREAK -> resweep" log sites (fast-confirm in
    // onTimer(), and timeout-confirm in onTimerOk_unlocked()). Find
    // the one that follows hw.clearAppBuf() — that's the
    // timeout-confirm branch.
    auto clearPos = src.find("hw.clearAppBuf();");
    assert(clearPos != std::string::npos);
    auto resetPos = src.find("ResetReason::HealthWatchdog", clearPos);
    assert(resetPos != std::string::npos);
    std::string window = src.substr(clearPos, resetPos - clearPos);
    assert(window.find("hw.flushRxHw();") != std::string::npos &&
           "the timeout-confirm branch must call hw.flushRxHw() "
           "between hw.clearAppBuf() and the reset_unlocked call — "
           "app-level and HAL-level RX state must clear together on "
           "a confirmed peer drop");
    (void)secondOccurrence;
    std::cout << "  PASS" << std::endl;
}

// Pin 3: the fast-confirm branch (breakConfirmPending_, consumed at
// the top of onTimer()) also flushes the raw HAL RX.
void test_fast_confirm_flushes_rx_hw() {
    std::cout << "\n=== Pin 3: fast-confirm branch calls "
                 "hw.flushRxHw() ===" << std::endl;
    std::string src = readFile(testRepoPath("src/al/link/timers/LinkTimersOk.cpp"));
    assert(!src.empty());
    auto pendingPos = src.find("if (breakConfirmPending_) {");
    assert(pendingPos != std::string::npos);
    auto returnPos = src.find("return;", pendingPos);
    assert(returnPos != std::string::npos);
    std::string branch = src.substr(pendingPos, returnPos - pendingPos);
    assert(branch.find("hw.flushRxHw();") != std::string::npos &&
           "the fast-confirm (breakConfirmPending_) branch must also "
           "call hw.flushRxHw() before returning — a confirmed peer "
           "drop via either confirm path must flush the raw HAL RX, "
           "not just the timeout path");
    std::cout << "  PASS" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== EspHal BREAK flush moved to confirm-only "
                 "(AL97-6) ===" << std::endl;
    test_event_hook_does_not_flush_on_delivery();
    test_timeout_confirm_flushes_rx_hw();
    test_fast_confirm_flushes_rx_hw();
    std::cout << "\nAll EspHalBreakFlushOnConfirmOnly pins passed."
              << std::endl;
    return 0;
}
#endif
