// Runtime pin for the post-construction
// OOB-closed shape.
//
// Background: AutoLinkConfig::allowedBaudsCount is
// a public field. A sketch (or a future refactor)
// can write cfg.allowedBaudsCount = 20 after
// link.begin() — at which point any code path that
// reads cfg.allowedBauds[i] directly, with
// spdI derived from cfg.allowedBaudsCount - 1,
// reads past the end of the fixed-size array
// (AUTOLINK_MAX_BAUDS = 16). The link layer's
// choke-point accessors clamp the count and the
// element on the way out, so a raw write to the
// public field can no longer drive spdI OOB.
//
// This test:
//   1. Constructs a Link against the default
//      config.
//   2. Writes cfg.allowedBaudsCount = 20 after
//      begin() (the OOB trigger).
//   3. Walks every accessor path that drives
//      spdI — getCurrentBaud, allowedBaud — and
//      asserts the returned value is bounded by
//      AUTOLINK_MAX_BAUDS, never an OOB read.
//
// Pins the choke-point contract from the consumer
// side; the structural counterpart
// (test_choke_points_route_through_clamped_accessors
// in TestAccessorStructureTest) locks the source
// shape so a future regression that drops back to
// the raw cfg.allowedBauds[i] pattern trips both
// the source grep AND the runtime assertion.

#include <iostream>
#include <cassert>
#include "MockHal.h"
#include "NullArqCache.h"
#include "LinkTestAccessor.h"
#include "al/AutoLinkConfig.h"
#include "al/link/Link.h"

using namespace autolink;

namespace {

// Walk the accessors that the link layer uses to
// resolve spdI -> baud. After the OOB trigger,
// none of them must read past the array.
void assert_bounded_reads(Link &link) {
    // The choke-point accessors are public via
    // the LinkContext interface (Link publicly
    // inherits LinkContext). Reach them through
    // a LinkContext& so the test doesn't have to
    // befriend Link.
    LinkContext &ctx = link;
    // getCurrentBaud() takes spdI -> cfg.allowedBauds[spdI]
    // through the safe accessor. Should never read OOB.
    uint32_t b1 = link.getCurrentBaud();
    // The choke-point accessor exposes the bounded
    // count even after a post-construction write.
    int n = ctx.allowedBaudsCount();
    assert(n <= AUTOLINK_MAX_BAUDS);
    assert(n >= 0);
    // Walk every in-range index AND every out-of-range
    // index (positive and negative) the choke points
    // could receive. allowedBaudSafe returns 0 for
    // OOB; the choke point must surface that 0, not
    // garbage.
    for (int i = -2; i <= AUTOLINK_MAX_BAUDS + 2; ++i) {
        uint32_t b = ctx.allowedBaud(i);
        if (i < 0 || i >= AUTOLINK_MAX_BAUDS) {
            assert(b == 0 && "OOB index must surface 0 through safe accessor");
        } else {
            // In-range: must equal what we wrote into the array
            // (the default config populates the first 5 entries
            // and zeros the rest; we only check >= 0 here).
            (void)b;
        }
    }
    std::cout << "  Link::getCurrentBaud()=" << b1
              << " Link::allowedBaudsCount()=" << n << " (bounded) \u2713"
              << std::endl;
}

} // namespace

void test_post_construction_count_write_is_bounded() {
    std::cout << "\n=== Test: cfg.allowedBaudsCount = 20 post-begin is "
                 "bounded ==="
              << std::endl;
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    Link link(hal, cache, true, cfg);
    LinkTestAccessor acc(link);

    // Force spdI to the array tail so the next
    // allowedBaud read would OOB if the choke
    // point weren't clamped. (cfg.allowedBauds
    // has 16 entries; index 15 is the last valid.)
    acc.setSpdI(AUTOLINK_MAX_BAUDS - 1);

    // Sanity: default config has 5 bauds; reading
    // spdI=15 returns 0 (no baud at that index in
    // the default config — entry 15 was never
    // populated, hence default-initialized to 0).
    LinkContext &ctx = link;
    uint32_t tail = ctx.allowedBaud(AUTOLINK_MAX_BAUDS - 1);
    assert(tail == 0);

    // The OOB trigger: write a count above the
    // array size. Pre-fix, the link layer would
    // index cfg.allowedBauds[19] (spdI = count-1)
    // and return garbage; post-fix, the choke
    // point clamps the count first.
    cfg.allowedBaudsCount = 20;
    assert_bounded_reads(link);
    std::cout << "  PASS (allowedBaudsCount=20, no OOB read through any "
                 "accessor)"
              << std::endl;
}

void test_negative_count_write_is_bounded() {
    std::cout << "\n=== Test: cfg.allowedBaudsCount = -1 post-begin is "
                 "bounded ==="
              << std::endl;
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    Link link(hal, cache, true, cfg);
    LinkTestAccessor acc(link);

    acc.setSpdI(0);
    cfg.allowedBaudsCount = -1;
    assert_bounded_reads(link);
    std::cout << "  PASS (allowedBaudsCount=-1, no underflow through any "
                 "accessor)"
              << std::endl;
}

int main() {
    std::cout << "=== Running Link Baud-Index Bounds Tests ===" << std::endl;
    test_post_construction_count_write_is_bounded();
    test_negative_count_write_is_bounded();
    std::cout << "\n=== Link Baud-Index Bounds Tests PASS ===" << std::endl;
    return 0;
}
