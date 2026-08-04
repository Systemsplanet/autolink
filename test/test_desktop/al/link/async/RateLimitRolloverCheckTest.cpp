// The rate limiter paces admission rather than
// refusing oversize. A multi-window message
// (len > lineRateBps) is admitted and parks a
// time-based debt; subsequent offers of any
// size are refused via the debt gate until the
// debt elapses. Single-window messages are
// admitted when the budget allows and refused
// with RateLimited when it doesn't. Drain credit
// keeps the budget honest for bursts the wire is
// actively draining.
//
// Pin 1 (multi-window admitted, debt parked):
// a 2433 B message at 9600 (line rate 960 B/s)
// is multi-window. The sendMsg returns true and
// rateNextAllowedMs_ parks 3 windows of debt
// (ceil(2433 / 960) = 3, 3 * 1000 ms = 3000 ms).
// Toggle off (refuse oversize outright) -> red:
// the library's documented `1..cfg.maxMsg`
// (default 5120) cannot be carried at the slowest
// default baud, so refusing the message at the
// 5120/9600 boundary wedges the field app's own
// traffic.
//
// Pin 2 (debt gate refuses during parking):
// a second 2433 B offer at the same instant is
// refused with SendMsgReason::RateLimited. The
// debt is the source of truth, not the byte
// counter. Toggle off (drop the debt gate) ->
// red: a back-to-back oversize burst re-pumps
// the debt on every call.
//
// Pin 3 (drain credit recovers the window):
// advance the wall clock past the debt
// (3 * RATE_WINDOW_MS = 3000 ms). The next
// 2433 B offer is admitted and parks a fresh
// debt. The byte counter was reset when the
// debt cleared, so the window is genuinely
// fresh, not stale. Toggle off (drop the
// debt-elapse reset) -> red: the byte counter
// accumulates wire traffic across debts and the
// window overshoots.
//
// Pin 4 (single-window refused when full):
// a 960 B message at 9600 is single-window. The
// first offer is admitted and the byte counter
// goes to 960. The second offer at the same
// instant is refused with RateLimited because
// 960 + 960 > 960.
//
// Pin 5 (drain credit lets a subsequent offer
// through): advance time past the window. The
// drain credit (elapsed * lineRateBps / 1000)
// wipes the byte counter. The next 960 B offer
// is admitted.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <vector>
#    include "MockHal.h"
#    include "TestCfg.h"
#    include "NullArqCache.h"
#    include "accessors/LinkTestAccessor.h"
#    include "al/link/LinkWire.h"

int main() {
    using namespace autolink;
    std::cout
        << "=== rate limit paces oversize, refuses single-window overflow ===\n";
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.allowedBauds[0] = 9600;
    cfg.allowedBaudsCount = 1;
    cfg.idleTimeoutMs = 0;
    cfg.maxMsg = 65535;

    MockHal mHal, sHal;
    NullArqCache cache;
    Link link(mHal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    t.forceStateNoLock(State::OK);
    t.setSpdI(0);
    mHal.spd = 9600;

    t.setRateWindowStartMsForTest(0);
    t.setRateWindowBytesForTest(0);
    t.setRateNextAllowedMsForTest(0);
    mHal.now = 5000;
    sHal.now = 5000;

    Stats s0;
    link.getStats(s0);
    uint64_t baseDrops = s0.rateLimitedDrops;

    // Pin 1: 2433 B at 9600. lineRateBps = 960.
    // Multi-window (2433 > 960). Admit, park
    // ceil(2433 / 960) = 3 windows of debt
    // (3000 ms).
    std::vector<uint8_t> big(2433, 0xAA);
    bool ok1 = link.sendMsg(big.data(), (int)big.size());
    assert(ok1 &&
           "Multi-window message at 9600 must be admitted (paced, "
           "not refused — the library's documented `1..cfg.maxMsg` "
           "exceeds the slowest baud's per-second line rate)");
    int32_t next1 = t.rateNextAllowedMsForTest();
    assert(next1 >= 5000 + 3 * 1000 - 100 && next1 <= 5000 + 3 * 1000 + 100 &&
           "rateNextAllowedMs_ must park 3 windows of debt "
           "((len + line - 1) / line = 3 windows = 3000 ms)");

    // Pin 2: second oversize at the same instant.
    // The debt gate refuses regardless of size.
    bool ok2 = link.sendMsg(big.data(), (int)big.size());
    assert(!ok2 && "Second offer within the debt window must be refused");
    SendMsgReason reason2 = t.lastSendMsgReasonForTest();
    assert(reason2 == SendMsgReason::RateLimited &&
           "lastSendMsgReason_ must be RateLimited on debt-gate refusal");
    Stats s1;
    link.getStats(s1);
    assert(s1.rateLimitedDrops == baseDrops + 1 &&
           "rateLimitedDrops must increment on debt-gate refusal");

    // Pin 3: advance past the debt. The window
    // resets on debt elapse (rateWindowStartMs_
    // and rateWindowBytes_ both cleared), so the
    // next offer is admitted against a fresh
    // budget.
    mHal.now = 9000; // 4000 ms > 3000 ms debt
    bool ok3 = link.sendMsg(big.data(), (int)big.size());
    assert(ok3 &&
           "After the parked debt elapses, the next 2433 B offer "
           "is admitted against a fresh window (drain credit "
           "resets the counter when the debt clears)");
    int32_t next3 = t.rateNextAllowedMsForTest();
    assert(next3 >= 9000 + 3 * 1000 - 100 && next3 <= 9000 + 3 * 1000 + 100 &&
           "rateNextAllowedMs_ must re-park 3 windows of debt "
           "for the second oversize admit");

    // Pin 4: 960 B at 9600. lineRateBps = 960.
    // Single-window (960 == 960). Admit and
    // charge 960 to the byte counter. Second
    // offer at the same instant is refused
    // (window full at 960 == lineRateBps).
    mHal.now = 50000; // past the 3rd debt
    std::vector<uint8_t> small(960, 0xBB);
    bool ok4 = link.sendMsg(small.data(), (int)small.size());
    assert(ok4 && "Single-window message at 9600 must be admitted");
    uint32_t usedAfter4 = t.rateWindowBytesForTest();
    assert(usedAfter4 == 960 &&
           "Window counter must be charged with the single-window payload");
    bool ok5 = link.sendMsg(small.data(), (int)small.size());
    assert(!ok5 &&
           "Second 960 B offer within the same window must be refused "
           "(window full at 960 == lineRateBps)");

    // Pin 5: advance 2 s. The drain credit
    // (2 * 960 B/s == 1920 B wiped) clears the
    // counter. The next 960 B offer is admitted.
    mHal.now = 52000;
    bool ok6 = link.sendMsg(small.data(), (int)small.size());
    assert(ok6 &&
           "After 2 s, the window has drained (2 * 960 == 1920 B) "
           "and the next 960 B offer is admitted");

    std::cout << "  PASS (multi-window paced, single-window "
              << "overflow refused, drain credit recovers)\n";
    std::cout << "=== RateLimitRolloverCheck: PASS ===\n";
    return 0;
}

#endif
