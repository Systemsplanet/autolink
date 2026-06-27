// Source-level regression test for the
// log / error-handling hygiene fixes in
// Ping, Pong, and Link.
//
//   - Ping::loop() "not ready" log must
//     NOT print a garbage swpAge on a
//     paused boot (where tSweepStall_
//     was never set and reads as a
//     huge delta since boot). When
//     paused_, emit a dedicated
//     "waiting for Start" line.
//   - Ping's `got < 0` recv-rejected
//     handler and matchEcho_ mismatch
//     must drain stale rx bytes via
//     `base_.comm_.flushRx()` AFTER
//     `clearQueue_()`. Without the
//     flush, the stale echo from the
//     previous frame poisons the next
//     recv() and the error spiral
//     keeps firing.
//   - Pong's "send failed during
//     pause / pre-ready window" log
//     must be a warning, not an
//     error, and must use the
//     "send skipped (link not ready)"
//     text instead of "SEND FAILED
//     (link dropped)" — the failure
//     is benign when the link hasn't
//     come up yet.
//   - Link.cpp's "WIRING?" diagnostic
//     must fire EXACTLY ONCE on first
//     crossing of emptySweeps > 10,
//     not every ~1.5 s as the prior
//     "reset to 5" loop produced.
//     Toggling back to
//     `emptySweeps = 5` trips the pin.
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

std::string extractFnBody(const std::string &src, const std::string &name) {
    auto start = src.find(name);
    if (start == std::string::npos)
        return "";
    auto bodyStart = src.find('{', start);
    if (bodyStart == std::string::npos)
        return "";
    int depth = 0;
    for (size_t i = bodyStart; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0)
                return src.substr(start, i + 1 - start);
        }
    }
    return "";
}

void test_ping_not_ready_log_branches_on_paused() {
    std::cout << "\n=== Ping::loop 'not ready' log branches on paused_ ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());

    // Pin 1a: the "not ready" debug
    // line must exist (still informative
    // when the link IS sweeping).
    assert(body.find("not ready") != std::string::npos);

    // Pin 1b: there must be an explicit
    // paused branch that emits a
    // distinct message instead of the
    // garbage swpAge. The pre-fix code
    // always printed
    // "not ready  swpAge=%lu ms" which
    // read as a multi-million-ms number
    // on paused boot (tSweepStall_ is
    // zero until the link comes up,
    // so (now - 0) wraps through
    // millis()' 32-bit space). Toggling
    // back to the single-branch form
    // trips here.
    auto pausedPos = body.find("if (paused_)");
    assert(pausedPos != std::string::npos);

    auto branchStart = body.find('{', pausedPos);
    assert(branchStart != std::string::npos);
    int depth = 0;
    size_t branchEnd = body.size();
    for (size_t i = branchStart; i < body.size(); i++) {
        if (body[i] == '{')
            depth++;
        else if (body[i] == '}') {
            depth--;
            if (depth == 0) {
                branchEnd = i + 1;
                break;
            }
        }
    }
    std::string pausedBranch = body.substr(pausedPos, branchEnd - pausedPos);
    // The C++ source splits "waiting
    // for " and "Start" across adjacent
    // string literals (the runtime
    // string is "paused (waiting for
    // Start)"). Either side of the
    // break is enough to pin the
    // intent — what we forbid is the
    // swpAge leak, not the prose.
    assert(pausedBranch.find("waiting for") != std::string::npos);
    assert(pausedBranch.find("Start") != std::string::npos);
    // The paused branch must NOT print
    // the swpAge arithmetic — that is
    // the entire bug.
    assert(pausedBranch.find("swpAge") == std::string::npos);

    // The "else" arm must keep the
    // swpAge diagnostic for the actual
    // sweep-stall case so operators can
    // still see how long the link has
    // been stuck.
    assert(body.find("swpAge=") != std::string::npos);
    std::cout
        << "  PASS (paused branch emits dedicated line, no swpAge leak)\n";
}

void test_ping_drains_rx_after_clearQueue() {
    std::cout
        << "\n=== Ping drains rx after clearQueue_ on got<0 + mismatch ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());

    // Pin 2a: after the `got < 0`
    // recv-rejected branch's
    // clearQueue_(), the loop must
    // call base_.comm_.flushRx() to
    // drain the stale echo bytes that
    // poisoned the next recv(). The
    // pre-fix comment "NO flushRx,
    // NO BREAK" was the bug — the
    // drain was deliberately skipped
    // and the error spiral
    // self-perpetuated.
    auto gotPos = body.find("if (got < 0)");
    assert(gotPos != std::string::npos);
    auto gotBranchStart = body.find('{', gotPos);
    assert(gotBranchStart != std::string::npos);
    int depth = 0;
    size_t gotBranchEnd = body.size();
    for (size_t i = gotBranchStart; i < body.size(); i++) {
        if (body[i] == '{')
            depth++;
        else if (body[i] == '}') {
            depth--;
            if (depth == 0) {
                gotBranchEnd = i + 1;
                break;
            }
        }
    }
    std::string gotBranch = body.substr(gotPos, gotBranchEnd - gotPos);
    assert(gotBranch.find("clearQueue_()") != std::string::npos);
    // flushRx must appear AFTER the
    // clearQueue_() inside the same
    // branch.
    auto clearPos = gotBranch.find("clearQueue_()");
    auto flushPos = gotBranch.find("base_.comm_.flushRx()");
    assert(flushPos != std::string::npos);
    assert(flushPos > clearPos);

    // Pin 2b: same drain in the per-recv
    // mismatch path. Without it the
    // local pending table clears but
    // the appBuf still holds the bytes
    // that produced the mismatch, and
    // the next recv() reads them as a
    // fresh (mismatching) frame.
    //
    // this release: matchEcho_ is gone. Pong
    // does NOT echo the payload back;
    // the wire-level ACK is the only
    // Pong-side response. The mismatch
    // path is no longer in matchEcho_
    // but in the got<0 branch (CRC /
    // desync) and the isAcked-driven
    // drain in loop(). The "got<0 +
    // clearQueue + flushRx" pin above
    // already covers the per-recv drain.
    // We additionally pin the absence
    // of matchEcho_ — if a future
    // change re-adds the echo path,
    // the absence pin trips red.
    assert(src.find("void matchEcho_") == std::string::npos);

    // Pin 2c: the pre-fix "NO flushRx,
    // NO BREAK" comment prose was
    // explicit about the bug — if a
    // future change puts it back, the
    // test fails loudly.
    assert(src.find("NO flushRx, NO BREAK") == std::string::npos);

    std::cout << "  PASS (got<0 + drain rx via flushRx after clearQueue_; "
                 "matchEcho_ removed in this release)\n";
}

void test_pong_send_failed_demoted_to_warning() {
    // this release: Pong no longer echoes the payload back
    // and therefore has no per-recv send-failure
    // branch. The whole pin set from 5.3.x is gone
    // (Pong's loop now only reads). This test now
    // pins the absence of the pre-fix send path so a
    // future re-introduction is intentional.
    std::cout << "\n=== Pong this release: no payload echo (send path gone) ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Pong.h");
    assert(!src.empty());

    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());

    // Pre-fix Pong called base_.comm_.send(...) to
    // echo the received payload back. this release drops
    // that path; the wire-level ACK (extended with
    // bytes-recvd in LinkTx::sendAckFrame_unlocked)
    // is the entire Pong-side response. Pin the
    // absence of the echo call site.
    assert(src.find("SEND FAILED (link dropped)") == std::string::npos);
    assert(src.find("send skipped (link not ready)") == std::string::npos);
    // The diagnostic ack log line in Pong's loop is
    // now "echo <seq> <bytes>" (crc=ok implicit).
    // Pin that the new shape is what's emitted.
    assert(src.find("echo %u %d") != std::string::npos);
    std::cout << "  PASS (Pong is ack-only; no echo path; "
                 "'echo <seq> <bytes>' diagnostic)\n";
}

void test_link_wiring_spam_ratelimits() {
    std::cout << "\n=== Link.cpp 'WIRING?' spam fires exactly once ==="
              << std::endl;
    std::string root = projectRoot();
    // The onTimerSwp_unlocked method moved to
    // LinkTimers.cpp after the god-class
    // split. Read that TU and search for the
    // WIRING? log site.
    std::string src = readFile(root + "/src/al/link/LinkTimers.cpp");
    assert(!src.empty());

    // Pin 4a: the WIRING? log must
    // gate on a one-shot predicate,
    // not an `if > 10` with an
    // unconditional reset. The pre-fix
    // shape:
    //     if (emptySweeps > 10) {
    //         if (!wasEverOk_) {
    //             Log::log().error(TAG, "WIRING? ...");
    //         }
    //         emptySweeps = 5;
    //     }
    // re-triggered every ~1.5 s on a
    // dead wire. The new shape:
    //     if (emptySweeps == 11) {
    //         if (!wasEverOk_) {
    //             Log::log().error(TAG, "WIRING? ...");
    //         }
    //     }
    // fires once and stays silent.
    auto wiringPos = src.find("WIRING?");
    assert(wiringPos != std::string::npos);

    // Locate the OUTER if-block
    // gating the WIRING? emission.
    // The outer predicate contains
    // `emptySweeps`; the inner one
    // contains `wasEverOk_`. We want
    // the outer one. Walk back from
    // the WIRING log to the nearest
    // `if (...emptySweeps...`
    // pattern — that's the
    // one-shot predicate.
    auto sweepPos = src.rfind("emptySweeps", wiringPos);
    assert(sweepPos != std::string::npos);
    auto ifPos = src.rfind("if (", sweepPos);
    assert(ifPos != std::string::npos);

    auto blockStart = src.find('{', ifPos);
    assert(blockStart != std::string::npos);
    int depth = 0;
    size_t blockEnd = src.size();
    for (size_t i = blockStart; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0) {
                blockEnd = i + 1;
                break;
            }
        }
    }
    std::string wiringIf = src.substr(ifPos, blockEnd - ifPos);

    // Predicate must compare against
    // a fixed boundary (== 11), not
    // a `>` (which combined with the
    // reset-to-5 below re-fires
    // forever).
    assert(wiringIf.find("emptySweeps == 11") != std::string::npos);
    // The block must NOT reset
    // emptySweeps back to 5 — that
    // reset was the source of the
    // spam. The fix lets the counter
    // keep climbing silently so the
    // == 11 predicate never re-fires.
    assert(wiringIf.find("emptySweeps = 5") == std::string::npos);

    // Pin 4b: the message text must
    // describe the wiring requirement
    // (TX/RX crossover, shared GND)
    // — operator-actionable.
    assert(src.find("TX->RX crossover") != std::string::npos);
    assert(src.find("shared GND") != std::string::npos);

    std::cout
        << "  PASS (WIRING? fires once on emptySweeps == 11, no reset-to-5 "
           "spam)\n";
}

} // namespace

int main() {
    std::cout << "=== Running PingPong Log-Hygiene Tests ===" << std::endl;
    test_ping_not_ready_log_branches_on_paused();
    test_ping_drains_rx_after_clearQueue();
    test_pong_send_failed_demoted_to_warning();
    test_link_wiring_spam_ratelimits();
    std::cout << "\n=== PingPong Log-Hygiene Tests Completed ===" << std::endl;
    return 0;
}

#endif