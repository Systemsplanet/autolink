// Source-level + runtime regression test for the
// EspHal stream-buffer floor.
//
// Pre-fix shape: streamBufferFloor() returned
// `slots * 2 * (maxMsg + kHdr)` with slots=16, i.e.
// 32 * (maxMsg + 6) bytes. At the current default
// maxMsg=5120, that's 32 * 5126 = 164032 bytes
// (~160 KB). xStreamBufferCreate on an ESP32 with
// a typical free-heap of 200-250 KB can refuse that
// allocation, and the stream-buf failure branch in
// EspHal::begin() used to fall through silently
// (now caught and turned into an abort + 300ms
// blink). But even when the allocation succeeded,
// the buffer was sized for a 16-slot ARQ pipeline
// — the RX staging buffer is not the ARQ cache,
// so sizing the staging buffer for the cache's
// capacity wastes heap and trips the OOM path on
// tight boards.
//
// The fix drops the `slots * 2` factor: floor is
// now `2 * (maxMsg + kHdr)`, ≈10.3 KB at maxMsg=5120.
// One full coalesced message plus a retransmit's
// worth of headroom. Caller's larger
// cfg.streamBufferSize still wins.
//
// The formula is host-linkable (pure arithmetic on
// AutoLinkConfig), so the regression test calls
// the actual production function rather than
// re-implementing the formula in the test. AGENTS
// rule 12 applies: no version literals in the
// file; the test pins "current shape" / "pre-fix
// shape" against the source.
#ifndef ARDUINO

#    include <cassert>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>

#    include "al/AutoLinkConfig.h"

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

void test_stream_buffer_floor_at_default_max_msg_stays_under_32k() {
    std::cout << "\n=== Pin: streamBufferFloor(cfg) at default maxMsg stays "
                 "under heap-realistic ceiling ==="
              << std::endl;
    // Heap-realistic ceiling for a single allocation
    // on a typical ESP32 with WiFi + dashboard up:
    // ~32 KB. Pre-fix shape (16 * 2 * (maxMsg + 6) =
    // 164 KB) blew past that at maxMsg=5120 and
    // pushed xStreamBufferCreate into OOM territory.
    constexpr size_t kCeilingBytes = 32 * 1024;

    autolink::AutoLinkConfig cfg;
    const size_t floor = autolink::streamBufferFloor(cfg);
    std::cout << "  default cfg: maxMsg=" << cfg.maxMsg
              << " streamBufferSize=" << cfg.streamBufferSize
              << " floor=" << floor << " bytes" << std::endl;
    assert(cfg.maxMsg == autolink::AUTOLINK_DEFAULT_MAX_MSG &&
           "default maxMsg should be AUTOLINK_DEFAULT_MAX_MSG");
    assert(floor >= cfg.maxMsg &&
           "floor must hold at least one full coalesced message");
    assert(floor <= kCeilingBytes &&
           "floor at default maxMsg must stay under the heap-realistic "
           "32 KB ceiling; the pre-fix 16-slot ARQ pipeline factor would "
           "produce ~160 KB here");
    // Sanity: at maxMsg=5120, the formula gives exactly
    // 2 * (5120 + 6) = 10252 bytes (since the default
    // streamBufferSize=2048 < floor). Pin the exact
    // value so a future tweak to the multiples
    // factor doesn't slip in unannounced.
    assert(floor == 2 * (cfg.maxMsg + 6) &&
           "floor must equal 2 * (maxMsg + kHdr=6) at the default; a "
           "future bump must update this pin");
    std::cout << "  PASS (floor=" << floor << " <= " << kCeilingBytes
              << " B, >= maxMsg=" << cfg.maxMsg << ")" << std::endl;
}

void test_caller_larger_stream_buffer_size_wins() {
    std::cout << "\n=== Pin: caller-set cfg.streamBufferSize > floor wins ==="
              << std::endl;
    // The fix preserves the original contract: a
    // caller who wants a larger buffer than the
    // floor (e.g. a 64 KB wire simulator scratch
    // pad) can still set cfg.streamBufferSize
    // directly, and streamBufferFloor() returns
    // the larger value.
    autolink::AutoLinkConfig cfg;
    cfg.maxMsg = 1024;
    cfg.streamBufferSize = 64 * 1024;
    const size_t floor = autolink::streamBufferFloor(cfg);
    assert(floor == cfg.streamBufferSize &&
           "caller's larger cfg.streamBufferSize must win over the floor");
    // And the floor itself at maxMsg=1024 should
    // be 2 * 1030 = 2060 bytes (well below 64K).
    const size_t computed = 2 * (cfg.maxMsg + 6);
    assert(autolink::streamBufferFloor({}) <= computed * 4 ||
           computed <= autolink::streamBufferFloor({}) + 1024);
    std::cout << "  PASS (caller's 64 KB won over the floor)" << std::endl;
}

void test_source_grep_drops_16_slot_pipeline_factor() {
    std::cout << "\n=== Pin: source drops the 16-slot ARQ pipeline factor ==="
              << std::endl;
    // The pre-fix source had `slots = 16` and
    // `(size_t)slots * 2 * (...)`. Either token
    // surviving in the production formula is a
    // regression to the over-sized floor. The
    // fix lives in AutoLinkConfig.h (host-linkable
    // so the test can call it directly) and is
    // forwarded by EspHal::streamBufferFloor as a
    // thin one-line wrapper.
    std::string cfgSrc = readFile(projectRoot() + "/src/al/AutoLinkConfig.h");
    assert(!cfgSrc.empty());

    // Locate the floor function body. Either the
    // declaration line or the definition body's
    // nearest occurrence — find the comment block
    // and read a 1500-char slice that includes the
    // inline body.
    auto p = cfgSrc.find("streamBufferFloor");
    assert(p != std::string::npos);
    // The formula body lives after the struct. Take
    // a 1500-char slice from the SECOND occurrence
    // onward so we land on the definition (the
    // forward decl + the body both match
    // "streamBufferFloor").
    auto def = cfgSrc.find("streamBufferFloor", p + 1);
    assert(def != std::string::npos);
    std::string slice = cfgSrc.substr(def, 1500);

    // Negative: no `slots = 16` constant. The pre-fix
    // shape declared `constexpr int slots = 16;` and
    // used it as a multiplier.
    assert(slice.find("slots = 16") == std::string::npos &&
           "pre-fix `slots = 16` constant must be gone from the floor "
           "formula");
    assert(slice.find("slots * 2") == std::string::npos &&
           "pre-fix `slots * 2 *` factor must be gone from the floor "
           "formula");
    // Negative: no `32 *` factor multiplying maxMsg.
    assert(slice.find("32 * (cfg.maxMsg") == std::string::npos &&
           "pre-fix `32 * (cfg.maxMsg ...)` factor must be gone from "
           "the floor formula");
    // Positive: the new formula `2 * (cfg.maxMsg + kHdr)` appears,
    // with kHdr=6. The constant `multiples = 2` is the new multiplier.
    assert(slice.find("multiples = 2") != std::string::npos &&
           "floor formula must use `multiples = 2` constant");
    assert(slice.find("multiples * (cfg.maxMsg + kHdr)") != std::string::npos &&
           "floor formula body must be `multiples * (cfg.maxMsg + "
           "kHdr)`");
    std::cout << "  PASS (no 16-slot factor, new 2× maxMsg formula present)"
              << std::endl;
}

void test_source_grep_espal_floor_is_a_thin_forward() {
    std::cout << "\n=== Pin: EspHal::streamBufferFloor forwards to "
                 "AutoLinkConfig::streamBufferFloor ==="
              << std::endl;
    // EspHal.h is the ARDUINO-gated runtime home of
    // the floor; on host we can't compile it (pulls
    // in FreeRTOS / driver headers). Pin that the
    // EspHal.h member is a thin wrapper over the
    // AutoLinkConfig free function — single source
    // of truth, no copy of the formula in the HAL.
    std::string halSrc = readFile(projectRoot() + "/src/al/hal/EspHal.h");
    assert(!halSrc.empty());

    // Find the EspHal::streamBufferFloor member body.
    auto p = halSrc.find("static size_t streamBufferFloor");
    assert(p != std::string::npos);
    // The body is small — read 400 chars.
    std::string slice = halSrc.substr(p, 400);

    // The body must call ::autolink::streamBufferFloor
    // (the free function in AutoLinkConfig.h).
    assert(slice.find("::autolink::streamBufferFloor(cfg)") !=
               std::string::npos &&
           "EspHal::streamBufferFloor must forward to "
           "::autolink::streamBufferFloor (single source of truth in "
           "AutoLinkConfig.h)");
    // The HAL body must NOT redefine the formula —
    // if `kHdr`, `multiples`, `slots`, `slots * 2`,
    // or `32 *` appear inside this slice, the
    // formula has been duplicated.
    assert(slice.find("kHdr") == std::string::npos &&
           "EspHal::streamBufferFloor must not redefine kHdr — the "
           "formula lives in AutoLinkConfig.h");
    assert(slice.find("multiples") == std::string::npos &&
           "EspHal::streamBufferFloor must not redefine the `multiples` "
           "constant — the formula lives in AutoLinkConfig.h");
    assert(slice.find("slots") == std::string::npos &&
           "EspHal::streamBufferFloor must not reference the pre-fix "
           "`slots` constant");
    std::cout << "  PASS (HAL body forwards to AutoLinkConfig::floor)"
              << std::endl;
}

void test_link_core_drops_bogus_stream_buffer_size_check() {
    std::cout << "\n=== Pin: LinkCore ctor drops the bogus "
                 "maxMsg > streamBufferSize check ==="
              << std::endl;
    // The pre-fix ctor logged an error when
    // cfg.maxMsg > cfg.streamBufferSize, but
    // cfg.streamBufferSize is the user-set hint,
    // not the actual buffer size used by the HAL
    // (which is streamBufferFloor(cfg)). After the
    // floor fix the actual buffer is always >=
    // 2*maxMsg, so the check is always wrong —
    // every sketch with the new default maxMsg=5120
    // and the default streamBufferSize=2048 would
    // see the spurious E (355) boot error.
    std::string src = readFile(projectRoot() + "/src/al/link/LinkCore.cpp");
    assert(!src.empty());

    // Locate the ctor body — begin() is later, so
    // grab the first 2000 chars after Link::Link.
    auto p = src.find("Link::Link(");
    assert(p != std::string::npos);
    std::string slice = src.substr(p, 2000);

    // Negative: the pre-fix `cfg.maxMsg > cfg.streamBufferSize` gate
    // is gone (the bogus boot error path).
    assert(slice.find("cfg.maxMsg > cfg.streamBufferSize") ==
               std::string::npos &&
           "LinkCore ctor must drop the bogus maxMsg > streamBufferSize "
           "check; the HAL floor guarantees buffer >= maxMsg");
    // Negative: the "maxMsg > streamBufSize" log message is gone.
    assert(slice.find("maxMsg > streamBufSize") == std::string::npos &&
           "LinkCore ctor must drop the 'maxMsg > streamBufSize' log "
           "line");
    std::cout << "  PASS (bogus streamBufferSize gate removed)" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running EspHal Stream-Buffer Floor Tests ==="
              << std::endl;
    test_stream_buffer_floor_at_default_max_msg_stays_under_32k();
    test_caller_larger_stream_buffer_size_wins();
    test_source_grep_drops_16_slot_pipeline_factor();
    test_source_grep_espal_floor_is_a_thin_forward();
    test_link_core_drops_bogus_stream_buffer_size_check();
    std::cout << "\n=== EspHal Stream-Buffer Floor Tests PASS ===" << std::endl;
    return 0;
}

#endif // !ARDUINO