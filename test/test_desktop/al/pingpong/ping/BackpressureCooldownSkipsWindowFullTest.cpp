// AL88-3: GbnWindowFull is normal, self-clearing flow control — the
// window drains as soon as the next ACK burst lands (one RTT), which
// is far sooner than the flat BACKPRESSURE_COOLDOWN_MS (1000 ms).
// Arming the cooldown on it anyway left the ASYNC pipeline idle for
// ~970 ms out of every ~1000 ms cycle even though the peer had
// already freed room — field-measured throughput at ~3% of SYNC's on
// an identically-configured link (243 vs 1352 echoes over 61 s on
// the same wire).
//
// Ping.h's send loop is Arduino-gated (#ifdef ARDUINO) and can't run
// on the host suite, so this suite follows the established pattern
// in this file's sibling tests (Pin 5b/5c in ModeSyncAsyncFixesTest)
// of pinning the fix at the source level: the cooldown assignment
// must sit behind a guard that excludes GbnWindowFull specifically,
// not fire unconditionally on every SendMsgReason.
//
// Toggle off (drop the `!= SendMsgReason::GbnWindowFull` guard,
// restoring the bare unconditional assignment) -> red.

#include <cassert>
#include <iostream>
#include "TestPaths.h"

using namespace autolink;

void test_cooldown_assignment_excludes_gbn_window_full() {
    std::cout << "\n=== Test: backpressure cooldown does not arm on "
                 "GbnWindowFull ===\n";
    std::ifstream f(testRepoPath("src/al/pingpong/Ping.h"));
    assert(f.good());
    std::string src((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    assert(!src.empty());

    auto cooldownPos = src.find("backpressureCoolUntilMs_ =\n"
                                "                        millis() + "
                                "BACKPRESSURE_COOLDOWN_MS;");
    // clang-format may reflow the RHS onto one line or several —
    // fall back to a looser anchor on the assignment's LHS/RHS pair
    // if the exact wrap point drifts.
    if (cooldownPos == std::string::npos) {
        auto lhs = src.find("backpressureCoolUntilMs_ =");
        while (lhs != std::string::npos) {
            std::string near = src.substr(lhs, 120);
            if (near.find("BACKPRESSURE_COOLDOWN_MS") != std::string::npos) {
                cooldownPos = lhs;
                break;
            }
            lhs = src.find("backpressureCoolUntilMs_ =", lhs + 1);
        }
    }
    assert(cooldownPos != std::string::npos &&
           "could not locate the backpressureCoolUntilMs_ = millis() + "
           "BACKPRESSURE_COOLDOWN_MS assignment in the send-failure branch");

    // Walk backward from the assignment to the nearest enclosing
    // `if (` and confirm it names GbnWindowFull with a `!=` — i.e.
    // the assignment is conditioned on "reason is not window-full",
    // not unconditional.
    std::string before = src.substr(0, cooldownPos);
    auto ifPos = before.rfind("if (");
    assert(ifPos != std::string::npos &&
           "the cooldown assignment must sit inside an if-guard");
    std::string guard = src.substr(ifPos, cooldownPos - ifPos);
    assert(guard.find("SendMsgReason::GbnWindowFull") != std::string::npos &&
           guard.find("!=") != std::string::npos &&
           "the guard immediately enclosing the cooldown assignment must "
           "test lastSendMsgReason() != SendMsgReason::GbnWindowFull — "
           "arming the cooldown on ordinary window-full backpressure "
           "leaves the ASYNC pipeline idle for the full 1000 ms cooldown "
           "even though the peer already freed room within one RTT");

    // Positive control: the switch/cause-string machinery above still
    // recognises GbnWindowFull as its own case (this suite would be
    // vacuous if that enum arm had been removed instead of guarded).
    assert(src.find("case SendMsgReason::GbnWindowFull:") != std::string::npos);

    std::cout << "  PASS (cooldown assignment guarded against "
                 "GbnWindowFull)\n";
}

int main() {
    std::cout << "=== Running BackpressureCooldownSkipsWindowFull Tests ==="
              << std::endl;
    test_cooldown_assignment_excludes_gbn_window_full();
    std::cout << "\n=== BackpressureCooldownSkipsWindowFull Tests Completed "
                 "Successfully ==="
              << std::endl;
    return 0;
}
