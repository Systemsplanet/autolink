// Source-level regression test for the begin() OOM path,
// the AutoLinkConfig tx buffer default, and the
// bringUpLink isHealthy() gate. Three fixes, three pins:
//
//   - EspHal::begin() must NOT retry uart_driver_install on
//     ESP_FAIL. The previous retry on transient DMA-release
//     races silently retried on OOM too, producing
//     "UART driver malloc error" twice in a row when
//     clampBuffers() inflated txBufferSize to ~21 KB. The
//     current contract: OOM (ESP_ERR_NO_MEM) → log free
//     heap, cleanup, return. No retry. Any other error →
//     log + cleanup + return. Toggle off (re-add the
//     ESP_FAIL retry / vTaskDelay / second uart_driver_install)
//     -> this test fails.
//   - AutoLinkConfig::txBufferSize default must be 256
//     (non-zero, sized for a couple of COBS frames at
//     default maxMsg). Toggle off (set back to 0) -> this
//     test fails.
//   - bringUpLink must call comm.isHealthy() after
//     comm.begin() and enter a halt loop on false. Toggle
//     off (delete the if (!comm.isHealthy()) block) -> this
//     test fails.
//
// EspHal.cpp / PingPongBase.h are `#ifdef ARDUINO` so they
// can't run on host; this gates the bug-fix contracts by
// reading the source, matching the pattern used by
// HttpdStartupTest.cpp.
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

void test_esphal_begin_does_not_retry_on_esp_fail_and_logs_oom() {
    std::cout << "\n=== EspHal::begin() does NOT retry uart_driver_install"
                 " and logs OOM with free heap ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/hal/EspHal.h");
    assert(!src.empty());

    // Locate the body of begin().
    auto beginPos = src.find("void begin(");
    assert(beginPos != std::string::npos);

    size_t scan = beginPos;
    int depth = 0;
    bool foundOpen = false;
    size_t endPos = std::string::npos;
    for (; scan < src.size(); scan++) {
        if (src[scan] == '{') {
            depth++;
            foundOpen = true;
        } else if (src[scan] == '}') {
            depth--;
            if (foundOpen && depth == 0) {
                endPos = scan;
                break;
            }
        }
    }
    assert(endPos != std::string::npos);
    std::string body = src.substr(beginPos, endPos - beginPos + 1);

    // Pin #1: the begin() body must NOT contain a
    // retry on ESP_FAIL. The previous shape was
    //     if (e == ESP_FAIL) { vTaskDelay(...); e = uart_driver_install(...); }
    // which silently retried on OOM. Either phrasing
    // is wrong: a literal `if (e == ESP_FAIL)` guarded
    // retry, or a `vTaskDelay` followed by a second
    // uart_driver_install. Reject both.
    assert(body.find("e == ESP_FAIL") == std::string::npos);

    // The body must contain exactly ONE
    // uart_driver_install call. Count occurrences.
    size_t searchFrom = 0;
    int installCount = 0;
    size_t firstInstall = std::string::npos;
    while (true) {
        auto p = body.find("uart_driver_install(", searchFrom);
        if (p == std::string::npos)
            break;
        if (firstInstall == std::string::npos)
            firstInstall = p;
        installCount++;
        searchFrom = p + 1;
    }
    assert(installCount == 1);
    assert(firstInstall != std::string::npos);

    // Pin #2: the OOM path. ESP_ERR_NO_MEM must be
    // checked and logged with the free heap value,
    // then `cleanup()` and `return` — no retry.
    auto noMemPos = body.find("ESP_ERR_NO_MEM", firstInstall);
    assert(noMemPos != std::string::npos);
    // The OOM log must actually CALL esp_get_free_heap_size()
    // so the operator sees the post-failure heap. Reject
    // comment-only or string-literal mentions.
    std::string oomSlice = body.substr(noMemPos, 400);
    assert(oomSlice.find("esp_get_free_heap_size()") != std::string::npos);
    // cleanup() + return must follow the OOM branch.
    assert(oomSlice.find("cleanup") != std::string::npos);
    assert(oomSlice.find("return") != std::string::npos);

    // Pin #3: free heap is logged BEFORE the
    // uart_driver_install call. The pre-install log
    // line lets operators see the heap delta that
    // the install caused.
    auto heapLogPos = body.find("free heap=", firstInstall);
    // The pre-install log must sit before the install
    // call site. (If the only "free heap=" mention is
    // inside the OOM branch, that fires AFTER the
    // failed install — too late for the operator to
    // see the pre-failure baseline.)
    assert(heapLogPos != std::string::npos);
    // Allow pre-install info logs that mention
    // "free heap=" or other heap info. We require the
    // Log::log().info(...) call to reference heap.
    auto infoLog = body.rfind("Log::log().info", firstInstall);
    assert(infoLog != std::string::npos);
    std::string infoSlice = body.substr(infoLog, firstInstall - infoLog);
    assert(infoSlice.find("free heap") != std::string::npos);

    std::cout << "  PASS (single uart_driver_install, "
                 "ESP_ERR_NO_MEM path with heap log + cleanup + return, "
                 "no ESP_FAIL retry, free heap logged before install)\n";
}

void test_esphal_derives_stream_buffer_size_from_maxmsg() {
    std::cout << "\n=== EspHal::begin() reserves the 2x-maxMsg floor via "
                 "the AutoLinkConfig helper (single source of truth) ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/hal/EspHal.h");
    assert(!src.empty());

    // The HAL must derive the stream buffer floor
    // from cfg.maxMsg internally. The pre-fix facade
    // mutator (clampBuffers) computed this in
    // src/AutoLink.cpp; the current contract is that
    // the facade does NOT touch cfg.streamBufferSize
    // and EspHal::begin() chooses its own size.

    // Pin: EspHal::streamBufferFloor is a thin
    // forwarder to ::autolink::streamBufferFloor (the
    // host-linkable pure-arithmetic helper in
    // AutoLinkConfig.h). Single source of truth: the
    // formula lives in one place and the HAL body
    // must not redefine kHdr / multiples / slots /
    // 32 *.
    auto floorPos = src.find("static size_t streamBufferFloor");
    assert(floorPos != std::string::npos);
    std::string floorWindow = src.substr(floorPos, 400);
    assert(floorWindow.find("::autolink::streamBufferFloor(cfg)") !=
               std::string::npos &&
           "EspHal::streamBufferFloor must forward to "
           "::autolink::streamBufferFloor (single source of truth in "
           "AutoLinkConfig.h)");
    // The HAL body must NOT redefine the formula.
    assert(floorWindow.find("kHdr") == std::string::npos &&
           "EspHal::streamBufferFloor must not redefine kHdr");
    assert(floorWindow.find("multiples") == std::string::npos &&
           "EspHal::streamBufferFloor must not redefine multiples");
    assert(floorWindow.find("slots = 16") == std::string::npos &&
           "pre-fix `slots = 16` constant must be gone from the HAL");
    assert(floorWindow.find("32 * (cfg.maxMsg") == std::string::npos &&
           "pre-fix 32x factor must be gone from the HAL");
    // No mode-based ternary anywhere in the HAL's
    // streamBufferFloor slice (the floor formula is
    // mode-agnostic; ASYNC's many-in-flight budget
    // comes from the ARQ cache, not the RX staging
    // buffer).
    assert(floorWindow.find("(cfg.mode == ") == std::string::npos);
    assert(floorWindow.find("ASYNC) ? 16 : 2") == std::string::npos);

    // Pin: the helper's formula in AutoLinkConfig.h
    // uses `2 * (maxMsg + kHdr)`, not a 16-slot
    // pipeline. The 32x factor at default maxMsg
    // produces ~160 KB which trips xStreamBufferCreate
    // on tight heaps.
    std::string cfgSrc = readFile(projectRoot() + "/src/al/AutoLinkConfig.h");
    assert(!cfgSrc.empty());
    auto cfgFloorPos = cfgSrc.find("inline size_t streamBufferFloor");
    assert(cfgFloorPos != std::string::npos);
    // Take a slice starting at the SECOND occurrence
    // (the definition body, not the forward decl).
    auto cfgFloorDef =
        cfgSrc.find("inline size_t streamBufferFloor", cfgFloorPos + 1);
    assert(cfgFloorDef != std::string::npos);
    std::string cfgFloorSlice = cfgSrc.substr(cfgFloorDef, 600);
    assert(cfgFloorSlice.find("multiples = 2") != std::string::npos &&
           "AutoLinkConfig::streamBufferFloor must use `multiples = 2` "
           "constant (2x maxMsg + hdr padding, not the pre-fix 32x "
           "pipeline factor)");
    assert(cfgFloorSlice.find("multiples * (cfg.maxMsg + kHdr)") !=
               std::string::npos &&
           "AutoLinkConfig::streamBufferFloor formula must be "
           "`multiples * (cfg.maxMsg + kHdr)`");
    // Negative pin: the pre-fix 16-slot factor must NOT
    // appear in the formula body.
    assert(cfgFloorSlice.find("slots = 16") == std::string::npos &&
           "pre-fix `slots = 16` must be gone from "
           "AutoLinkConfig::streamBufferFloor");
    assert(cfgFloorSlice.find("32 * (cfg.maxMsg") == std::string::npos &&
           "pre-fix 32x factor must be gone from "
           "AutoLinkConfig::streamBufferFloor");

    // The facade must NOT define or use clampBuffers
    // anymore. Pin that the duplication was removed.
    std::string facadeSrc = readFile(projectRoot() + "/src/AutoLink.cpp");
    assert(!facadeSrc.empty());
    assert(facadeSrc.find("clampBuffers") == std::string::npos);
    std::string facadeHdr = readFile(projectRoot() + "/include/AutoLink.h");
    assert(!facadeHdr.empty());
    assert(facadeHdr.find("clampBuffers") == std::string::npos);

    std::cout << "  PASS (EspHal forwards to AutoLinkConfig::streamBufferFloor "
                 "with 2x-maxMsg formula; no clampBuffers in facade)\n";
}

void test_esphal_no_legacy_single_byte_peek_buf() {
    std::cout << "\n=== EspHal::peek paths use only the array, not the "
                 "legacy single-byte peek_buf ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/hal/EspHal.h");
    assert(!src.empty());

    // The pre-fix code carried two parallel peek
    // buffers: a single-byte `peek_buf` and a 16-byte
    // array `peek_buf_[]`. The single-byte one was
    // effectively dead and the two paths were
    // entangled. The current contract: only the array
    // exists. Pin that:
    //   - the symbol `peek_buf ` (with a space — i.e.
    //     NOT `peek_buf_` / `peek_buf_len_` /
    //     `peek_buf_pos_` / `peek_buf_cap`) is gone
    //   - the array `peek_buf_[]` and its cursor
    //     fields remain.
    // The simplest rejection: any standalone `peek_buf`
    // token that is NOT followed by `_`.
    size_t searchFrom = 0;
    while (true) {
        auto p = src.find("peek_buf", searchFrom);
        if (p == std::string::npos)
            break;
        // Skip the underscore-suffixed variants.
        char next = (p + 8 < src.size()) ? src[p + 8] : '\0';
        bool isSuffix = (next == '_');
        assert(isSuffix);
        searchFrom = p + 8;
    }

    // The array and cursor fields must still be there.
    assert(src.find("peek_buf_") != std::string::npos);
    assert(src.find("peek_buf_len_") != std::string::npos);
    assert(src.find("peek_buf_pos_") != std::string::npos);

    std::cout << "  PASS (legacy peek_buf removed; only peek_buf_[]"
                 " array + cursors remain)\n";
}

void test_autolink_test_ctor_takes_ihal_by_reference() {
    std::cout << "\n=== AutoLink host-test ctor takes IHal by reference "
                 "(no raw pointer, no ownership ambiguity) ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/include/AutoLink.h");
    assert(!src.empty());

    // The previous host-test ctor was
    //     AutoLink(IHal *hal_in, bool isMasterNode, ...)
    // which silently took ownership via a no-op deleter.
    // The current contract: ctor takes IHal& so the
    // compiler rejects null and ownership is
    // unambiguous.
    auto ctorPos = src.find("AutoLink(IHal");
    assert(ctorPos != std::string::npos);
    // The token right after `AutoLink(IHal` must be
    // `&` (reference) — reject `*` (pointer) and any
    // other whitespace-prefixed shape.
    size_t p = ctorPos + std::string("AutoLink(IHal").size();
    while (p < src.size() && (src[p] == ' ' || src[p] == '\t'))
        p++;
    assert(p < src.size() && src[p] == '&');

    std::cout << "  PASS (host-test ctor takes IHal by reference)\n";
}

void test_autolink_config_default_tx_buffer_size_is_256() {
    std::cout << "\n=== AutoLinkConfig default txBufferSize == 256 ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/AutoLinkConfig.h");
    assert(!src.empty());

    // The field is a struct member — find it and read the
    // initializer. Tolerate any amount of whitespace between
    // the identifier and the literal.
    auto pos = src.find("txBufferSize");
    assert(pos != std::string::npos);

    auto eq = src.find('=', pos);
    assert(eq != std::string::npos);
    auto semi = src.find(';', eq);
    assert(semi != std::string::npos);
    std::string init = src.substr(eq + 1, semi - eq - 1);
    // Trim whitespace.
    size_t a = init.find_first_not_of(" \t\n\r");
    size_t b = init.find_last_not_of(" \t\n\r");
    if (a == std::string::npos)
        init = "";
    else
        init = init.substr(a, b - a + 1);
    assert(init == "256");

    // Sanity: the field must NOT be zero (the pre-fix default).
    assert(init != "0");

    std::cout << "  PASS (default txBufferSize = " << init << ")\n";
}

void test_bringUpLink_halts_on_isHealthy_false() {
    std::cout << "\n=== bringUpLink checks comm.isHealthy() after begin() "
                 "and halts on false ==="
              << std::endl;
    std::string src =
        readFile(projectRoot() + "/src/al/pingpong/PingPongBase.h");
    assert(!src.empty());

    // Find the bringUpLink free function body. It sits inside
    // the autolink namespace, gated on #ifdef ARDUINO.
    auto fnPos = src.find("inline void bringUpLink(");
    assert(fnPos != std::string::npos);

    size_t scan = fnPos;
    int depth = 0;
    bool foundOpen = false;
    size_t endPos = std::string::npos;
    for (; scan < src.size(); scan++) {
        if (src[scan] == '{') {
            depth++;
            foundOpen = true;
        } else if (src[scan] == '}') {
            depth--;
            if (foundOpen && depth == 0) {
                endPos = scan;
                break;
            }
        }
    }
    assert(endPos != std::string::npos);
    std::string body = src.substr(fnPos, endPos - fnPos + 1);

    // The isHealthy() call must happen AFTER comm.begin().
    auto beginCall = body.find("comm.begin()");
    assert(beginCall != std::string::npos);
    auto healthyCall = body.find("comm.isHealthy()", beginCall);
    assert(healthyCall != std::string::npos);

    // The branch must include a log call and a halt-style
    // infinite loop. Either `while (true)` with a delay or
    // an ESP-style `for (;;) delay(...)` would satisfy the
    // pin; we require the warning log + a while-true halt
    // because that's what the production shape uses.
    std::string branch = body.substr(healthyCall);
    assert(branch.find("log.error") != std::string::npos);
    assert(branch.find("while (true)") != std::string::npos);

    // And the "link layer up" log must be unreachable when
    // the branch fires — it must sit AFTER the isHealthy()
    // call, NOT before.
    auto linkUp = body.find("link layer up");
    assert(linkUp != std::string::npos);
    assert(linkUp > healthyCall);

    std::cout << "  PASS (isHealthy() check + log.error + halt come "
                 "after comm.begin(); link-up log is gated behind them)\n";
}

} // namespace

int main() {
    std::cout << "=== Running EspHal Begin + Health Gate Tests ==="
              << std::endl;
    test_esphal_begin_does_not_retry_on_esp_fail_and_logs_oom();
    test_esphal_derives_stream_buffer_size_from_maxmsg();
    test_esphal_no_legacy_single_byte_peek_buf();
    test_autolink_test_ctor_takes_ihal_by_reference();
    test_autolink_config_default_tx_buffer_size_is_256();
    test_bringUpLink_halts_on_isHealthy_false();
    std::cout << "\n=== EspHal Begin + Health Gate Tests Completed ==="
              << std::endl;
    return 0;
}

#endif
