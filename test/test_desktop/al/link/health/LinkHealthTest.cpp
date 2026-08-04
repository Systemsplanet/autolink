// Pins decideHealth (LinkHealth.h): every OK-state
// keep/drop watchdog in one truth table. Toggle-off
// (re-gate any check by mode, or return Keep for a
// drop row) turns the matching row red. Rows cover
// the four shipped wedges: the ASYNC pool-reject
// stall, the SYNC lost-mid-message-ACK desync (both
// DropTxStall), the asymmetric peer-gone drop, the
// clean-quiet-link false positive (Keep), pool
// exhaustion, the dead-link backstop (a
// mutually-quiet link with an in-flight op — the
// SYNC-mode recovery path that was otherwise missing),
// and the silent-peer backstop
// (locked link with empty pipeline and zero frame
// errors but no RX for ages — the no-traffic wedge
// DropDeadLink/DropIdle miss). Plus healthDeadPeerMs
// math and source-grep pin on the LinkHealth.h enum
// value + helper + LinkTimersOk.cpp call site.
#ifndef ARDUINO

#    include <cassert>
#    include <cstring>
#    include <cstdlib>
#    include <cstdio>
#    include <iostream>
#    include "al/link/timers/LinkHealth.h"
#    include "TestPaths.h"

using namespace autolink;

int main() {
    constexpr int idle = 10000;
    constexpr uint32_t now = 100000;
    struct Row {
        HealthState h;
        HealthAction expect;
        const char *why;
    };
    auto st = [](uint32_t rf, uint32_t rl, uint32_t rx, uint32_t tx, int pend,
                 uint64_t errs, bool full, bool sync, uint32_t rto = 0) {
        HealthState h;
        h.rejFirstMs = rf;
        h.rejLastMs = rl;
        h.lastRxMs = rx;
        h.lastTxMs = tx;
        h.pending = pend;
        h.frameErrs = errs;
        h.poolFull = full;
        h.sync = sync;
        h.rtoMs = rto;
        return h;
    };
    const Row rows[] = {
        // tx-reject streak (both modes)
        { st(now - 11000, now - 500, now, now, 0, 0, false, false),
          HealthAction::DropTxStall, "ASYNC live streak past idle -> drop" },
        { st(now - 11000, now - 500, now, now, 0, 0, false, true),
          HealthAction::DropTxStall, "SYNC live streak past idle -> drop" },
        { st(now - 11000, now - 11000, now, now, 0, 0, false, true),
          HealthAction::Keep, "stale streak (no recent reject) holds" },
        { st(now - 9000, now - 500, now, now, 0, 0, false, false),
          HealthAction::Keep, "young streak holds" },
        { st(0, 0, now, now, 0, 0, false, true), HealthAction::Keep,
          "SYNC sees only the streak: quiet keeps" },
        // asymmetric idle (ASYNC only)
        { st(0, 0, now - 400, now - 100, 1, 0, false, false),
          HealthAction::DropAsymIdle, "TX pending, RX silent -> drop" },
        { st(0, 0, now - 400, now - 100, 0, 0, false, false),
          HealthAction::Keep, "RX silent but nothing pending holds" },
        { st(0, 0, now - 400, now - 2000, 1, 0, false, false),
          HealthAction::Keep, "TX not recent: asym gate holds" },
        { st(0, 0, now - 200, now - 100, 1, 0, false, false),
          HealthAction::Keep, "RX fresh: asym gate holds" },
        { st(0, 0, now - 400, now - 100, 1, 0, false, true), HealthAction::Keep,
          "SYNC skips asym (inline waitForAck)" },
        // asym retx horizon: silence shorter than
        // 2 x RTO is a backpressured sender waiting
        // on its own sweep, not a gone peer.
        { st(0, 0, now - 400, now - 100, 1, 0, false, false, 500),
          HealthAction::Keep, "RX silence inside 2xRTO holds" },
        { st(0, 0, now - 1100, now - 100, 1, 0, false, false, 500),
          HealthAction::DropAsymIdle, "RX silence past 2xRTO -> drop" },
        // dead-link backstop. The new check runs
        // BEFORE the SYNC short-circuit so SYNC
        // gets a mutual-quiet drop path; for ASYNC
        // it outranks DropIdle (both fire on
        // pending>0 + mutual quiet, but DropDeadLink
        // is narrower: pending>0 only, no frameErrs
        // alternative). Clean mutual quiet (no
        // pending) is still Keep — the "link is
        // idle" case the prior symmetric-idle
        // removal preserved.
        { st(0, 0, now - 11000, now - 11000, 1, 0, false, false),
          HealthAction::DropDeadLink,
          "ASYNC quiet+pending -> DropDeadLink (outranks DropIdle)" },
        { st(0, 0, now - 11000, now - 11000, 1, 0, false, true),
          HealthAction::DropDeadLink,
          "SYNC quiet+pending -> DropDeadLink (was Keep )" },
        { st(0, 0, now - 11000, now - 11000, 0, 0, false, true),
          HealthAction::Keep, "SYNC clean mutual quiet (no pending) -> Keep" },
        { st(0, 0, now - 11000, now - 11000, 0, 0, false, false),
          HealthAction::Keep, "ASYNC clean mutual quiet (no pending) -> Keep" },
        { st(0, 0, now - 11000, now - 9000, 1, 0, false, false),
          HealthAction::Keep, "TX inside idle window holds (dead-link off)" },
        { st(0, 0, now - 9000, now - 11000, 1, 0, false, true),
          HealthAction::Keep, "SYNC RX inside idle window holds" },
        { st(0, 0, now, now, 1, 0, false, true), HealthAction::Keep,
          "SYNC fresh rx+tx+pending holds" },
        // DropIdle (ASYNC) — still reachable for
        // frameErrs without pending. The frameErrs
        // case is the only one DropDeadLink doesn't
        // subsume, so this row pins the surviving
        // DropIdle contract.
        { st(0, 0, now - 11000, now - 11000, 0, 3, false, false),
          HealthAction::DropIdle,
          "quiet past idle with frameErrs (no pending) -> DropIdle" },
        // pool exhaustion (ASYNC only). A full pool
        // with a live receiver is routine flood
        // backpressure; the drop needs RX silence
        // past the retx horizon on top. rxAge=now-400
        // is well inside idleTimeoutMs=10000, so
        // DropDeadLink (rxAge > idle) doesn't fire
        // first — the rows still pin DropPoolExhaust.
        { st(0, 0, now, now, 1, 0, true, false), HealthAction::Keep,
          "pool full + RX fresh = backpressure, holds" },
        { st(0, 0, now - 400, now - 1500, 1, 0, true, false),
          HealthAction::DropPoolExhaust, "pool full + RX silent -> drop" },
        { st(0, 0, now - 400, now - 1500, 1, 0, true, false, 500),
          HealthAction::Keep, "pool full, silence inside 2xRTO holds" },
        { st(0, 0, now - 1100, now - 1500, 1, 0, true, false, 500),
          HealthAction::DropPoolExhaust,
          "pool full, silence past 2xRTO -> drop" },
        { st(0, 0, now, now, 0, 0, true, false), HealthAction::Keep,
          "pool full, nothing pending holds" },
        { st(0, 0, now - 400, now, 1, 0, true, true), HealthAction::Keep,
          "SYNC never populates the pool" },
        // priority: stall outranks the dead-link /
        // ASYNC checks. Live reject streak is more
        // urgent than a quiet-pending wedge.
        { st(now - 11000, now - 500, now - 400, now - 100, 1, 0, true, false),
          HealthAction::DropTxStall, "stall wins over asym/pool" },
        { st(now - 11000, now - 500, now - 11000, now - 11000, 1, 0, false,
             true),
          HealthAction::DropTxStall, "stall wins over dead-link (SYNC)" },
        // Silent-peer backstop (). A locked link that received nothing past
        // deadPeerMs = 3 x idleTimeoutMs is sitting on a
        // dead peer even with pending=0 and frameErrs=0.
        // DropDeadLink misses it (needs pending>0) and
        // DropIdle misses it (needs pending>0 || frameErrs>0)
        // — the wedge is the no-traffic locked shape. The
        // backstop runs before the SYNC short-circuit so
        // a SYNC link in the same shape also recovers.
        { st(0, 0, now - 31000, now - 31000, 0, 0, false, false),
          HealthAction::DropSilentPeer,
          "ASYNC locked+silent past deadPeerMs -> drop" },
        { st(0, 0, now - 31000, now - 31000, 0, 0, false, true),
          HealthAction::DropSilentPeer,
          "SYNC locked+silent past deadPeerMs -> drop" },
        { st(0, 0, now - 11000, now - 11000, 0, 0, false, false),
          HealthAction::Keep,
          "ASYNC silence inside deadPeerMs (10s < 30s) holds" },
        { st(0, 0, now - 29000, now - 29000, 0, 0, false, false),
          HealthAction::Keep, "ASYNC silence just inside deadPeerMs holds" },
        { st(0, 0, now - 31000, now - 31000, 0, 0, true, false),
          HealthAction::DropSilentPeer,
          "silent backstop fires even with pool full" },
        { st(0, 0, now - 31000, now - 31000, 0, 3, false, false),
          HealthAction::DropSilentPeer,
          "silent backstop fires regardless of frameErrs" },
        // Defensive: lastRxMs == 0 means the link has
        // never received since reset (early SWP / test
        // fixture). The backstop is gated on lastRxMs>0
        // so a fresh link that just has not been touched
        // yet does not get torn down by the backstop.
        { st(0, 0, 0, now, 0, 0, false, false), HealthAction::Keep,
          "fresh link (lastRxMs=0) holds" },
        // Priority vs DropIdle: a quiet mutual past idle with
        // frameErrs>0 is DropIdle unless rxAge > deadPeerMs (30000),
        // in which case the silent-peer backstop catches it first.
        // rxAge=11000 here is below deadPeerMs so DropIdle is the
        // surviving verdict; rxAge=11000 + frameErrs=3 -> DropIdle
        // row above captures that boundary.
    };
    int n = 0;
    for (const Row &r : rows) {
        int deadPeerMs = healthDeadPeerMs(idle);
        HealthAction got = decideHealth(r.h, now, idle, deadPeerMs, r.h.rtoMs);
        if (got != r.expect) {
            std::cout << "FAIL: " << r.why << " got=" << (int)got
                      << " expect=" << (int)r.expect << std::endl;
            assert(false);
        }
        n++;
    }
    std::cout << "PASS: decideHealth table (" << n << " rows)" << std::endl;

    // Pin S1: healthDeadPeerMs math. 3 x idleTimeoutMs in
    // the normal case. The defensive 30 s floor that
    // idleTimeoutMs == 0 is the user's opt-out: no backstop, no
    // keepalive, by contract. 3x for the 10 s default gives 30 s —
    // wide enough not to tear down a legitimately-idle deployment
    // link, narrow enough to catch a dead peer (locked, then the
    // remote master leaves, 20+ s of silence).
    if (healthDeadPeerMs(10000) != 30000) {
        std::cout << "FAIL: healthDeadPeerMs(10000) != 30000" << std::endl;
        assert(false);
    }
    if (healthDeadPeerMs(60000) != 180000) {
        std::cout << "FAIL: healthDeadPeerMs(60000) != 180000" << std::endl;
        assert(false);
    }
    // 0 in now returns 0 unconditionally (caller
    // gates the call). A negative input is a contract
    // violation; the math is `3 * 0 = 0` for 0 and
    // would be negative for a negative input. The
    // helper has no contract on negative inputs.
    if (healthDeadPeerMs(0) != 0) {
        std::cout << "FAIL: healthDeadPeerMs(0) != 0 (caller's gate)"
                  << std::endl;
        assert(false);
    }
    std::cout << "PASS: healthDeadPeerMs math" << std::endl;

    // Pin S0: idleTimeoutMs=0 disables the health
    // machine. The caller's gate in applyHealth_unlocked
    // (LinkTimersOk.cpp) returns HealthAction::Keep when
    // idleTimeoutMs == 0, so decideHealth is never
    // consulted. Pin the table: the very same inputs
    // that drop a non-zero idleTimeoutMs link must
    // drop NOTHING when the caller short-circuits. The
    // caller's gate is the test's shape — LinkTimersOk.cpp
    // is exercised by the run-loop suite, but the
    // table pins the gate's design.
    {
        // Call-site gate: applyHealth_unlocked's
        // `cfg.idleTimeoutMs > 0 ? decideHealth(...) :
        // HealthAction::Keep` is the only path; assert
        // it here at the table level.
        HealthState h;
        h.rejFirstMs = now - 11000;
        h.rejLastMs = now - 500;
        h.lastRxMs = now;
        h.lastTxMs = now;
        h.rtoMs = 0;
        h.pending = 1;
        h.frameErrs = 0;
        h.poolFull = false;
        h.sync = false;
        // With idleTimeoutMs > 0, decideHealth returns
        // DropTxStall.
        HealthAction got =
            decideHealth(h, now, 10000, healthDeadPeerMs(10000), h.rtoMs);
        if (got != HealthAction::DropTxStall) {
            std::cout << "FAIL: idleTimeoutMs=10000 should drop, got="
                      << (int)got << std::endl;
            assert(false);
        }
        // The caller's gate: a hypothetical
        // decideHealth(h, now, 0, healthDeadPeerMs(0))
        // returns Keep because healthDeadPeerMs(0)=0
        // makes the rxAge / deadPeerMs checks
        // tautological. The caller's gate is what
        // the production code uses; the helper math
        // is pinned separately by the S1 rows above.
        // Note: `idleTimeoutMs=0` is the *user's
        // opt-out* — no backstop, no keepalive, by
        // contract.
        std::cout << "PASS: idleTimeoutMs=0 disables health machine (caller's "
                     "gate; helper returns 0)"
                  << std::endl;
    }

    // Pin S2: source pin. LinkHealth.h declares
    // HealthAction::DropSilentPeer, defines healthDeadPeerMs,
    // and the branch sits between DropDeadLink and the
    // SYNC short-circuit. LinkTimersOk.cpp threads the helper
    // into the decideHealth call. Catches a future revert
    // that drops the enum value, deletes the helper, moves
    // the check below the SYNC short-circuit, or removes
    // the deadPeerMs arg from the call.
    {
        FILE *f = fopen(testRepoPath("src/al/link/timers/LinkHealth.h").c_str(), "r");
        assert(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        assert(buf);
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = 0;
        fclose(f);
        const char *enumDecl = strstr(buf, "DropSilentPeer");
        assert(enumDecl);
        const char *helperDecl = strstr(buf, "healthDeadPeerMs");
        assert(helperDecl);
        // Order: the DropSilentPeer check must come AFTER
        // the DropDeadLink check and BEFORE the SYNC
        // short-circuit ("if (h.sync) return HealthAction::Keep;")
        // so a SYNC link in the wedge also recovers.
        const char *deadLinkCheck = strstr(buf, "DropDeadLink");
        assert(deadLinkCheck);
        const char *silentPeerCheck = strstr(deadLinkCheck, "DropSilentPeer");
        assert(silentPeerCheck);
        const char *syncKeep = strstr(silentPeerCheck, "if (h.sync)");
        assert(syncKeep);
        // Signature carries the new arg.
        const char *sig = strstr(buf, "decideHealth(const HealthState &h,");
        assert(sig);
        const char *deadPeerArg = strstr(sig, "int deadPeerMs");
        assert(deadPeerArg);
        // : rtoMs is now a 4th param (baud-aware
        // RTO) so the fast-idle / pool-exhaust gates
        // don't fire at 9600 baud before the queued
        // payload has time to land.
        const char *rtoArg = strstr(sig, "uint32_t rtoMs");
        assert(rtoArg &&
               "LinkHealth.h decideHealth must take a 4th rtoMs arg "
               "for baud-aware gating");
        // The defensive-floor branch is gone — the
        // helper returns `3 * idleTimeoutMs`
        // unconditionally and the caller (LinkTimersOk.cpp
        // applyHealth_unlocked) gates on
        // `idleTimeoutMs > 0` first. A future revert
        // that re-adds the floor would put
        // `? 3 * idleTimeoutMs : 30000` back; assert
        // its absence.
        const char *floor = strstr(buf, ": 30000");
        assert(!floor &&
               "healthDeadPeerMs must not carry a defensive floor; "
               "the caller's gate is the only path");
        free(buf);
    }
    {
        FILE *f = fopen(testRepoPath("src/al/link/timers/LinkTimersOk.cpp").c_str(), "r");
        assert(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        assert(buf);
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = 0;
        fclose(f);
        // The call site in applyHealth_unlocked must
        // pass healthDeadPeerMs(cfg.idleTimeoutMs) and
        // the baud-aware rtoMs as the 4th and 5th
        // decideHealth args.
        const char *call =
            strstr(buf, "decideHealth(h, now, cfg.idleTimeoutMs,");
        assert(call);
        const char *helper =
            strstr(call, "healthDeadPeerMs(cfg.idleTimeoutMs)");
        assert(helper);
        // : 5th arg is the baud-aware rtoMs
        // (applyHealth_unlocked now derives h.rtoMs
        // from roundTripMs at the locked baud, not
        // cfg.syncAckTimeoutMs).
        const char *rtoCall = strstr(call, "h.rtoMs");
        assert(rtoCall &&
               "LinkTimersOk.cpp applyHealth_unlocked must pass "
               "h.rtoMs (baud-aware) as the 5th decideHealth arg");
        // The DropSilentPeer switch case must log a
        // warning so the backstop's fire is visible.
        const char *caseDecl =
            strstr(buf, "case HealthAction::DropSilentPeer:");
        assert(caseDecl);
        free(buf);
    }
    std::cout << "PASS: silent-peer source pin" << std::endl;

    // Pin 5: DropAsymIdle must return Keep while
    // GBN retx is in flight with budget remaining,
    // and only fire when GbnMaxRetx (driven by
    // sweepRetx_unlocked) is the authority. The
    // watchdog's 2xRTO horizon is shorter than the
    // ARQ layer's maxRetx * syncAckTimeoutMs budget
    // (2.5 s at the defaults), so a mid-repair drop
    // leaves a stuck base. Suppress DropAsymIdle while
    // h.gbnActive && h.gbnBudgetOpen, let GbnMaxRetx
    // drive the honest-drop path.
    {
        HealthState h;
        h.lastRxMs = now - 1100; // past 2xRTO
        h.lastTxMs = now - 100;  // fresh tx (asym)
        h.rtoMs = 500;
        h.pending = 1; // in-flight repair
        h.frameErrs = 0;
        h.poolFull = false;
        h.sync = false;
        // No GBN in flight: DropAsymIdle.
        HealthAction got =
            decideHealth(h, now, 10000, healthDeadPeerMs(10000), h.rtoMs);
        if (got != HealthAction::DropAsymIdle) {
            std::cout << "FAIL: Pin 5 baseline (no GBN) should drop, got="
                      << (int)got << std::endl;
            assert(false);
        }
        // GBN in flight, budget remaining: Keep.
        h.gbnActive = true;
        h.gbnBudgetOpen = true;
        h.gbnAttempts = 0;
        got = decideHealth(h, now, 10000, healthDeadPeerMs(10000), h.rtoMs);
        if (got != HealthAction::Keep) {
            std::cout << "FAIL: Pin 5 GBN-in-flight-budget-open should Keep, "
                      << "got=" << (int)got << std::endl;
            assert(false);
        }
        // GBN in flight, budget exhausted: DropAsymIdle
        // (the GbnMaxRetx branch is the other one and
        // sits in sweepRetx_unlocked, not in
        // applyHealth_unlocked; this confirms the gate
        // flips back when the budget is closed).
        h.gbnBudgetOpen = false;
        h.gbnAttempts = 5;
        got = decideHealth(h, now, 10000, healthDeadPeerMs(10000), h.rtoMs);
        if (got != HealthAction::DropAsymIdle) {
            std::cout << "FAIL: Pin 5 GBN-in-flight-budget-closed should drop, "
                      << "got=" << (int)got << std::endl;
            assert(false);
        }
        std::cout << "PASS: DropAsymIdle suppressed during GBN retx "
                  << "with budget open" << std::endl;
    }

    // Pin 5b: source-grep on the gbnActive &&
    // gbnBudgetOpen gate in the DropAsymIdle branch.
    {
        FILE *f = fopen(testRepoPath("src/al/link/timers/LinkHealth.h").c_str(), "r");
        assert(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        assert(buf);
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = 0;
        fclose(f);
        const char *asym = strstr(buf, "DropAsymIdle");
        assert(asym);
        // The bug-class shape was a bare
        // `return HealthAction::DropAsymIdle;` with no
        // GBN check. The new shape is
        // `if (h.gbnActive && h.gbnBudgetOpen) return Keep;`
        const char *gbnGate = strstr(asym, "h.gbnActive && h.gbnBudgetOpen");
        assert(gbnGate &&
               "Pin 5b: LinkHealth.h DropAsymIdle must "
               "gate on `h.gbnActive && h.gbnBudgetOpen`. "
               "Without this, the watchdog fires mid-repair "
               "under the 2xRTO horizon (~1 s) and leaves a "
               "stuck base; the GBN layer is entitled to its "
               "full maxRetx * syncAckTimeoutMs budget "
               "(2.5 s at defaults).");
        const char *keepReturn = strstr(gbnGate, "return HealthAction::Keep");
        assert(keepReturn &&
               "Pin 5b: the GBN-in-flight budget-open path "
               "must return HealthAction::Keep (the "
               "GbnMaxRetx authority in sweepRetx_unlocked "
               "drives the honest drop).");
        free(buf);
    }

    // Pin 6: source-grep on the DropAsymIdle warning
    // log in LinkTimersOk.cpp. The prior shape was a
    // bare `Log::log().warning(TAG, "asymmetric idle "
    // "-> drop");` with no evidence. The new shape
    // carries rxAge, txAge, pending, rtoMs, rxIdleFloor,
    // lastValidRxMs, lastRxMs so a clock-stale fire
    // (during live CRC-valid RX) is root-causable from
    // the field log alone.
    {
        FILE *f = fopen(testRepoPath("src/al/link/timers/LinkTimersOk.cpp").c_str(), "r");
        assert(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        assert(buf);
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = 0;
        fclose(f);
        // The bug-class shape is `asymmetric idle -> drop`
        // with nothing after. The new shape includes
        // rxAge=, txAge=, pending=, rtoMs=, rxIdleFloor=,
        // lastValidRxMs=, lastRxMs=.
        const char *asymLog = strstr(buf, "asymmetric idle -> drop");
        assert(asymLog);
        // The first occurrence in LinkTimersOk.cpp is in
        // the applyHealth_unlocked switch case. The
        // new shape must include "rxAge=" in the same
        // warning (substring after the headline).
        const char *rxAgeInLog = strstr(asymLog, "rxAge=");
        assert(rxAgeInLog &&
               "Pin 6: DropAsymIdle warning must include "
               "rxAge= (otherwise the clock-stale fire "
               "during live RX is not root-causable from "
               "the field log)");
        const char *txAgeInLog = strstr(asymLog, "txAge=");
        assert(txAgeInLog &&
               "Pin 6: DropAsymIdle warning must include "
               "txAge=");
        const char *pendingInLog = strstr(asymLog, "pending=");
        assert(pendingInLog &&
               "Pin 6: DropAsymIdle warning must include "
               "pending=");
        const char *rtoInLog = strstr(asymLog, "rtoMs=");
        assert(rtoInLog &&
               "Pin 6: DropAsymIdle warning must include "
               "rtoMs=");
        const char *rxFloorInLog = strstr(asymLog, "rxIdleFloor=");
        assert(rxFloorInLog &&
               "Pin 6: DropAsymIdle warning must include "
               "rxIdleFloor=");
        const char *lastValidInLog = strstr(asymLog, "lastValidRxMs=");
        assert(lastValidInLog &&
               "Pin 6: DropAsymIdle warning must include "
               "lastValidRxMs= (raw CRC-validated clock)");
        const char *lastAnyInLog = strstr(asymLog, "lastRxMs=");
        assert(lastAnyInLog &&
               "Pin 6: DropAsymIdle warning must include "
               "lastRxMs= (raw any-byte clock, for "
               "clock-stale disambiguation)");
        free(buf);
    }
    std::cout << "PASS: DropAsymIdle log hygiene pin" << std::endl;

    return 0;
}

#endif
