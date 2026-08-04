// Regression pin: LinkArq / LinkSweep drive
// the link through LinkContext, not
// friendship. Two checks:
//
// 1. Source-level: Link.h does NOT
//    `friend class LinkSweep/LinkArq`,
//    and the helper headers and .cpp
//    files do NOT `#include
//    "al/link/Link.h"`. Together this
//    proves the helpers cannot reach
//    Link's privates even by accident.
//
// 2. Compile + runtime: a mock context
//    that is *not* a Link drives the
//    helpers. If a future change reverts
//    the helper signatures back to
//    `Link&`, this test stops compiling
//    (a mock is not a Link). If the
//    helpers ever re-introduce a
//    friendship path, the source-level
//    check catches it.
//
// LinkReorder / IReorderSink were removed
// in the GBN rewrite (the receiver no
// longer holds out-of-order frames); the
// third leg of this pin went with them.
#ifndef ARDUINO

#    include <cassert>
#    include <cstdio>
#    include <cstring>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>
#    include <vector>

#    include "al/link/ISweepCtx.h"
#    include "al/link/LinkWire.h"
#    include "al/link/arq/LinkArq.h"
#    include "al/link/sweep/LinkSweep.h"

namespace {

// Mock LinkContext — not a Link. Records
// every call so the runtime pin can assert
// what the helper asked the link for.
struct CallLog {
    int hwLock = 0;
    int hwUnlock = 0;
    int hwSetSpd = 0;
    int hwStartTimer = 0;
    int sendFrame = 0;
    int sweepPongReset = 0;
    mutable uint32_t nowMs = 0; // mock clock — advances per call
    int currentSpdI = -1;
    int sentSpdI = -1;
    uint32_t setBaud = 0;
    int timerMs = -1;
    uint8_t lastFrame = 0;
};

class MockCtx : public autolink::ISweepCtx {
public:
    CallLog log;

    void hwLock() override { log.hwLock++; }
    void hwUnlock() override { log.hwUnlock++; }
    uint32_t hwNowMs() const override {
        // Advance so spin-waits can
        // actually time out. Each call
        // returns a value that grows
        // past the previous one — the
        // test seeds log.nowMs once and
        // the rest is automatic.
        return log.nowMs++;
    }
    void hwSetSpd(uint32_t b) override {
        log.hwSetSpd++;
        log.setBaud = b;
    }
    void hwStartTimer(int ms) override {
        log.hwStartTimer++;
        log.timerMs = ms;
    }
    void resetSweepPongCount() override { log.sweepPongReset++; }
    void sendFrame(uint8_t p) override {
        log.sendFrame++;
        log.lastFrame = p;
    }
    void sendSweepFrame(uint8_t p) override {
        log.sendFrame++;
        log.lastFrame = p;
    }

    bool masterRole() const override { return true; }
    int currentSpdI() const override { return log.currentSpdI; }
    void setCurrentSpdI(int i) override { log.sentSpdI = i; }
    int allowedBaudsCount() const override { return 2; }
    uint32_t allowedBaud(int i) const override {
        static const uint32_t bauds[2] = { 115200, 9600 };
        return bauds[i];
    }
    int delayMs() const override { return 50; }
};

// 1. Source-level structural pin.
// Reads Link.h and the two helper
// headers/impls, asserts the helper
// cannot reach Link privates.
static std::string slurp(const std::string &path) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "FAIL: cannot open " << path << std::endl;
        assert(false);
    }
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

static void test_friend_declarations_removed_from_Link_h() {
    std::cout << "\n=== friend class LinkSweep/LinkArq absent from Link.h ==="
              << std::endl;
    const std::string path = "../../src/al/link/Link.h";
    std::string body = slurp(path);

    // The strings we'd see if either helper
    // friend were re-introduced. Anchored on
    // the bare class names so a stray "friend
    // class LinkSweepTests" or a comment
    // containing the substring doesn't
    // false-fire.
    const std::vector<std::string> forbidden = {
        "friend class LinkSweep;",
        "friend class LinkArq;",
    };
    for (const auto &needle : forbidden) {
        if (body.find(needle) != std::string::npos) {
            std::cerr << "FAIL: '" << needle << "' is still in " << path
                      << std::endl;
            assert(false && "friend declaration must be removed");
        }
    }
    std::cout << "PASS (no friend declarations for the helpers)" << std::endl;
}

static void test_helper_sources_do_not_include_Link_h() {
    std::cout << "\n=== helpers don't include al/link/Link.h ===" << std::endl;
    const std::vector<std::string> paths = {
        "../../src/al/link/arq/LinkArq.h",
        "../../src/al/link/arq/LinkArq.cpp",
        "../../src/al/link/sweep/LinkSweep.h",
        "../../src/al/link/sweep/LinkSweep.cpp",
    };
    for (const auto &p : paths) {
        std::string body = slurp(p);
        if (body.find("al/link/Link.h") != std::string::npos) {
            std::cerr << "FAIL: '" << p << "' still includes al/link/Link.h"
                      << std::endl;
            assert(false && "helper must not include Link.h");
        }
    }
    std::cout << "PASS (helpers stay out of Link.h)" << std::endl;
}

// 2. Runtime pin: drive the helpers
// against a MockCtx that is *not* a
// Link. Compiling this block is itself
// part of the pin — if the helpers
// require Link&, MockCtx is not a Link
// and the file won't build.
static void test_sweep_drives_linkcontext() {
    std::cout << "\n=== LinkSweep drives a mock LinkContext ===" << std::endl;
    autolink::LinkSweep sweep;
    sweep.dwells().phase1 = 50;
    sweep.dwells().phase2[0] = 50;
    sweep.dwells().phase2[1] = 50;
    sweep.dwells().phase3 = 100;
    MockCtx ctx;
    ctx.log.nowMs = 1000;
    ctx.log.currentSpdI = -1;

    sweep.enterPhase1(ctx);
    assert(ctx.log.hwSetSpd == 1);
    assert(ctx.log.setBaud == 9600); // slowest of {115200, 9600}
    assert(ctx.log.sentSpdI == 1);   // slowest index
    assert(ctx.log.sendFrame == 1);
    assert(ctx.log.lastFrame == autolink::PING_CMD);
    assert(ctx.log.hwStartTimer == 1);
    assert(ctx.log.timerMs == 50);

    sweep.enterPhase2(ctx);
    assert(ctx.log.sentSpdI == 0);
    assert(ctx.log.setBaud == 115200);
    assert(ctx.log.hwStartTimer == 2);
    assert(ctx.log.timerMs == 50);

    sweep.enterPhase3(ctx, 0);
    assert(ctx.log.setBaud == 115200);
    assert(ctx.log.sendFrame == 3); // P1, P2, P3 each fire one
    assert(ctx.log.lastFrame == autolink::PING_CMD);
    std::cout << "PASS (3 phase entries → 3 setSpd + 3 sendFrame)" << std::endl;
}

static void test_arq_waitforack_drops_lock_and_retakes() {
    std::cout << "\n=== LinkArq::waitForAck drops and re-takes the "
                 "context lock ==="
              << std::endl;
    autolink::LinkArq arq;
    arq.clearAll();
    // Mark seq 7 as in-flight at t=0.
    arq.onSent(7, 7, 0);
    assert(arq.isPending(7));

    MockCtx ctx;
    ctx.log.nowMs = 0;
    // timeoutMs=1 means: deadline is t0+1 = 1.
    // ctx.log.nowMs stays at 0 the whole time,
    // so the spin never sees an ACK and times
    // out after one iteration.
    bool acked = arq.waitForAck(ctx, 7, 1);
    assert(!acked);
    assert(!arq.isPending(7));
    // Lock accounting: the spin took the lock
    // on timeout exit. Caller is expected to
    // already hold it on entry — waitForAck
    // drops and re-grabs. After the call:
    //   hwUnlock called once (the drop)
    //   hwLock called once (the re-take)
    assert(ctx.log.hwUnlock == 1);
    assert(ctx.log.hwLock == 1);
    std::cout << "PASS (lock balanced: 1 unlock, 1 re-lock)" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running LinkContext isolation tests ===" << std::endl;
    test_friend_declarations_removed_from_Link_h();
    test_helper_sources_do_not_include_Link_h();
    test_sweep_drives_linkcontext();
    test_arq_waitforack_drops_lock_and_retakes();
    std::cout << "\n=== LinkContext isolation tests PASS ===" << std::endl;
    return 0;
}

#endif
