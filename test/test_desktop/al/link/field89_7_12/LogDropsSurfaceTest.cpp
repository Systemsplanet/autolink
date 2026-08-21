// AL89 pin 11 / LogDropsSurfaceTest. AL-D1: converted from four
// source-grep string checks (QUEUE_CAP literal, droppedLines()
// identifier, logDrops identifier, retx-log-level substring search)
// into real behavioral tests: an actual ring overflow counted
// through droppedLines() and Stats.logDrops, and an actual resend's
// log severity captured via Log's own sink API rather than grepped
// out of source text.
#include "FieldWedgeFixes89Common.h"
#include "al/link/arq/ArqCache.h"

using namespace autolink;
using namespace autolink::field89;

namespace {
struct SinkCapture {
    std::string wantSubstr;
    char sevFound = 0;
    int hits = 0;
};
SinkCapture *g_capture = nullptr;
void captureSink(char sev, const char *, const char *msg, void *) {
    if (g_capture && g_capture->wantSubstr.size() &&
        std::string(msg).find(g_capture->wantSubstr) != std::string::npos) {
        g_capture->sevFound = sev;
        g_capture->hits++;
    }
}
} // namespace

// Pin 11 (AL89-11): the log ring is sized to 128 usable entries (up
// from the 64 that lost 42.8 s of master log on the field capture's
// failure path — see docs/Version.md's AL94-1 entry for the later
// 256-overflowed-the-real-device correction), droppedLines() and
// Stats.logDrops make an overflow countable rather than only
// inferable from a gap, and the per-frame "ARQ retx" line runs at
// debug level (not warning) so it doesn't flood the ring at
// pipeline rate. Toggle off any of the four -> red: this test
// forces a real overflow and counts real drops, and captures a
// real resend's log severity via Log's sink API.
void test_LogDropsSurfaceTest() {
    std::cout << "\n=== Pin 11: Log ring QUEUE_CAP=128 + "
                 "droppedLines() surface + retx demote ==="
              << std::endl;
    Log::log().clearDroppedLines();
    Log::log().drainPending();
    Log::Level prevLevel = Log::log().getLevel();
    Log::log().setLevel(Log::VERBOSE);

    // --- QUEUE_CAP / droppedLines(): overflow the pending queue by
    // exactly 2 without draining, confirm droppedLines() reads
    // back exactly 2. This is only possible to observe cleanly if
    // the cap is exactly 128 — a regression to 64 would already
    // have dropped lines well before the 130th call, and this
    // test's own byte-for-byte count would catch either direction
    // (cap too small: droppedLines() > 2; cap too large: this
    // call sequence wouldn't overflow at all and droppedLines()
    // would read 0).
    for (int i = 0; i < 130; i++)
        Log::log().debug("LogDropsSurfaceTest", "filler line %d", i);
    uint64_t drops = Log::log().droppedLines();
    std::cout << "  130 lines enqueued without draining: droppedLines()="
              << drops << std::endl;
    if (drops != 2) {
        std::cerr << "\nFAIL: droppedLines()=" << drops
                  << " after 130 lines with no drain — expected exactly 2 "
                     "(130 - QUEUE_CAP=128). Either QUEUE_CAP isn't 128, "
                     "or droppedLines() isn't counting the pending-queue "
                     "overflow path correctly."
                  << std::endl;
        assert(false);
    }
    Log::log().drainPending();

    // --- Stats.logDrops wiring: a real Link's getStats() must
    // surface the same counter, not a stale copy or a different
    // one.
    {
        NullArqCache cache;
        AutoLinkConfig cfg;
        cfg.streamBufferSize = 8192;
        cfg.txBufferSize = 8192;
        MockHal hal;
        Link link(hal, cache, true, cfg);
        link.begin();
        Stats s;
        link.getStats(s);
        std::cout << "  Stats.logDrops=" << s.logDrops
                  << " (Log::log().droppedLines()="
                  << Log::log().droppedLines() << ")" << std::endl;
        if (s.logDrops != Log::log().droppedLines()) {
            std::cerr << "\nFAIL: Stats.logDrops (" << s.logDrops
                      << ") doesn't match Log::log().droppedLines() ("
                      << Log::log().droppedLines()
                      << ") — the wiring from the log ring's own counter "
                         "into the periodic stats line is broken."
                      << std::endl;
            assert(false);
        }
    }
    Log::log().clearDroppedLines();

    // --- retx log severity: drive a real resend (ASYNC, a genuine
    // ArqCache holding a real chunk, a NAK for the pending base)
    // and capture the actual severity of the "ARQ retx" line via
    // Log's sink API — not a grep for a literal ".debug(" call.
    {
        ArqCache pingArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
        ArqCache pongArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
        AutoLinkConfig cfg;
        testBaseCfg(cfg);
        cfg.mode = AutoLinkConfig::Mode::ASYNC;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link ping(mHal, pingArq, true, cfg);
        Link pong(sHal, pongArq, false, cfg);
        ping.begin();
        pong.begin();
        lockPair(ping, pong, mHal, sHal);
        assert(ping.getState() == State::OK);

        uint8_t payload[64];
        for (int i = 0; i < 64; i++)
            payload[i] = (uint8_t)i;
        bool queued = ping.sendMsg(payload, sizeof(payload));
        assert(queued);
        mHal.peer = nullptr;
        LinkTestAccessor pa(ping);
        uint8_t baseSeq = pa.gbnBase();

        SinkCapture cap;
        cap.wantSubstr = "ARQ retx cobsSeq=";
        g_capture = &cap;
        Log::log().setSink(captureSink, nullptr);
        pa.onNakForTest(baseSeq);
        Log::log().setSink(nullptr, nullptr);
        g_capture = nullptr;

        std::cout << "  retx log hits=" << cap.hits << " severity='"
                  << (cap.sevFound ? cap.sevFound : '?') << "'" << std::endl;
        if (cap.hits == 0) {
            std::cerr << "\nFAIL: the NAK-driven resend produced no "
                         "\"ARQ retx cobsSeq=\" log line at all — this "
                         "test isn't reaching retxSeq_unlocked, so it "
                         "isn't exercising the path it claims to."
                      << std::endl;
            assert(false);
        }
        if (cap.sevFound != 'D') {
            std::cerr << "\nFAIL: the \"ARQ retx\" line fired at severity '"
                      << cap.sevFound
                      << "', not 'D' (debug). This line fires once per "
                         "resend — 400+ chunks/s on a saturated ASYNC "
                         "link — and a warning-level line at that rate "
                         "floods the log ring inside one second."
                      << std::endl;
            assert(false);
        }
    }

    std::cout << "  PASS (QUEUE_CAP=128 behaviorally confirmed via "
                 "overflow, Stats.logDrops wired to droppedLines(), "
                 "retx log fires at debug severity)"
              << std::endl;
}
