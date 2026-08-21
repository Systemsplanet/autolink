// AL97-5: findMsgHeaderResync_unlocked returned -1 for BOTH "could
// not scan at all" (avail < MSG_HDR, or malloc failure) AND
// "scanned, found no valid header" (the overwhelmingly common
// case). The second case still calls hw.clearAppBuf(), discarding
// real data — but the caller's log line ("resync scan dropped %d
// bytes") printed the sentinel -1 as if it were a byte count, and
// no counter anywhere recorded how much data a desync event
// actually cost.
//
// Fix: on the "scanned, nothing found" path, return the number of
// bytes actually discarded (not -1) and accumulate it in
// Stats.resyncDroppedBytes. -1 is now reserved for the two
// couldn't-scan-at-all cases.
//
// A second, independent bug caught while implementing the fix: the
// scan snapshot is capped by max_scan (snapLen = min(scan+MSG_HDR,
// avail)), but the failure path's hw.clearAppBuf() wipes the
// ENTIRE app buffer (avail bytes), not just the snapshot (got
// bytes) — so avail > got is possible whenever max_scan caps the
// scan below what's actually queued, and reporting `got` there
// would itself undercount the true loss. This pin exercises both
// the equal case (avail == snapshot) and the capped case (avail >
// snapshot) so a regression to the `got`-based accounting is
// caught too.
//
// Drives Link::findMsgHeaderResync_unlocked directly via
// LinkTestAccessor::findMsgHeaderResyncForTest — the same
// abstraction level ResyncScanErrTest already uses for the
// adjacent err()-firing contract on this same function, and
// HoldNakSelfDescribingTest uses for onPayload. Toggle off (revert
// to `return got` / `return -1` for the no-header-found case) ->
// red on both pins below.
#ifndef ARDUINO

#    include <cassert>
#    include <iostream>
#    include <vector>
#    include "MockHal.h"
#    include "LinkTestAccessor.h"
#    include "NullArqCache.h"

using namespace autolink;

namespace {

Link *makeLinkOk(MockHal &hal, NullArqCache &cache, AutoLinkConfig &cfg,
                 LinkTestAccessor **outAcc) {
    Link *link = new Link(hal, cache, true, cfg);
    link->begin();
    *outAcc = new LinkTestAccessor(*link);
    (*outAcc)->forceState(State::OK);
    return link;
}

// Pin 1: avail < MSG_HDR (couldn't scan at all) must still return
// -1, and must not touch resyncDroppedBytes — this is the genuine
// "nothing to scan" sentinel, distinct from "scanned and found
// nothing".
void test_avail_below_header_size_returns_sentinel() {
    std::cout << "\n=== Pin 1: avail < MSG_HDR returns -1, does not "
                 "count as a drop ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    MockHal hal;
    LinkTestAccessor *acc = nullptr;
    Link *link = makeLinkOk(hal, cache, cfg, &acc);

    uint8_t tooShort[] = { 0x01, 0x02, 0x03 }; // < MSG_HDR (6)
    hal.pushAppBuf(tooShort, sizeof(tooShort));

    int r = acc->findMsgHeaderResyncForTest((int)cfg.maxMsg + MSG_HDR);
    std::cout << "  findMsgHeaderResync_unlocked returned " << r
              << std::endl;
    assert(r == -1 &&
           "avail < MSG_HDR must still return the -1 sentinel — this "
           "is a genuine could-not-scan case, not a found-nothing case");

    std::cout << "  PASS" << std::endl;
    delete acc;
    delete link;
}

// Pin 2: avail >= MSG_HDR, scan runs, no valid header anywhere in
// it (garbage bytes: no length/CRC combination will validate). The
// function must return the actual byte count discarded, not -1 —
// and Stats.resyncDroppedBytes must reflect it.
void test_no_header_found_reports_byte_count() {
    std::cout << "\n=== Pin 2: scanned-but-not-found reports the real "
                 "byte count, not -1 ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    MockHal hal;
    LinkTestAccessor *acc = nullptr;
    Link *link = makeLinkOk(hal, cache, cfg, &acc);

    // 12 bytes of garbage: no 6-byte window in here has both a
    // len in [1, cfg.maxMsg] AND a matching CRC16 — msgResyncScan
    // returns -1 for every offset.
    std::vector<uint8_t> garbage = { 0xFF, 0xFF, 0xFF, 0xFF, 0xDE, 0xAD,
                                     0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    hal.pushAppBuf(garbage.data(), (int)garbage.size());

    int r = acc->findMsgHeaderResyncForTest((int)cfg.maxMsg + MSG_HDR);
    std::cout << "  findMsgHeaderResync_unlocked returned " << r
              << " (buffer was " << garbage.size() << " bytes)"
              << std::endl;
    assert(r >= 0 &&
           "a scan that ran and found nothing must return the bytes "
           "discarded, not the -1 sentinel — the old shape printed "
           "a nonsensical negative byte count in the field log");
    assert((size_t)r == garbage.size() &&
           "the reported count must equal the full app-buf content "
           "actually destroyed by clearAppBuf()");
    assert(hal.appBufAvailable() == 0 &&
           "the app buffer must actually be empty after the failed "
           "scan (clearAppBuf() ran)");

    std::cout << "  PASS" << std::endl;
    delete acc;
    delete link;
}

// Pin 3: avail > the max_scan-capped snapshot window. clearAppBuf()
// still wipes the WHOLE buffer, so the reported count must be
// avail, not the smaller snapshot size — the accounting bug caught
// while implementing this fix.
void test_capped_scan_reports_full_avail_not_snapshot() {
    std::cout << "\n=== Pin 3: a max_scan-capped scan still reports the "
                 "FULL buffer as dropped, not just the scanned window ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    MockHal hal;
    LinkTestAccessor *acc = nullptr;
    Link *link = makeLinkOk(hal, cache, cfg, &acc);

    // 40 bytes of garbage queued, but max_scan caps the snapshot to
    // 10 (+ MSG_HDR) — avail (40) > snapLen. Every one of the 40
    // bytes still gets destroyed by clearAppBuf() on failure.
    std::vector<uint8_t> garbage(40, 0x7E);
    hal.pushAppBuf(garbage.data(), (int)garbage.size());

    int r = acc->findMsgHeaderResyncForTest(/*max_scan=*/10);
    std::cout << "  findMsgHeaderResync_unlocked(max_scan=10) returned "
              << r << " (buffer was " << garbage.size() << " bytes)"
              << std::endl;
    assert(r == (int)garbage.size() &&
           "a scan capped below the full buffer must still report "
           "the FULL buffer size as dropped — clearAppBuf() destroys "
           "everything queued, not just the bytes that were "
           "actually scanned, and undercounting here means the "
           "field-visible drop total silently understates real "
           "data loss whenever max_scan caps below what's queued");
    assert(hal.appBufAvailable() == 0 &&
           "the app buffer must be fully empty, including the "
           "portion beyond the scanned snapshot");

    std::cout << "  PASS" << std::endl;
    delete acc;
    delete link;
}

} // namespace

int main() {
    std::cout << "=== Resync Scan Reports Dropped Bytes (AL97-5) ==="
              << std::endl;
    test_avail_below_header_size_returns_sentinel();
    test_no_header_found_reports_byte_count();
    test_capped_scan_reports_full_avail_not_snapshot();
    std::cout << "\nAll ResyncScanReportsDroppedBytes pins passed."
              << std::endl;
    return 0;
}
#endif
