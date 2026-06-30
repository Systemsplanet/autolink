// Auto-generated split of the original
// HandleRootChunkedTest.cpp. Each TU in this
// split covers a single concern (handleRoot
// chunked send / begin lifecycle / httpd retry
// budget / Link kickoff defer) and includes
// the shared helpers via
// HandleRootChunkedTestCommon.h.
#include "HandleRootChunkedTestCommon.h"
#include "al/util/Log.h"

using namespace autolink;

void test_handle_root_uses_chunked_send() {
    std::cout
        << "\n=== handleRoot routes the dashboard through httpd_resp_send_chunk ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWebHandlers.cpp");
    assert(!src.empty());

    std::string body = extractHandleRootBody(src);
    assert(!body.empty());

    // Pin 1: the buggy single-shot path is gone. The old
    // call site was
    //   httpd_resp_send(req, DASHBOARD_HTML, sizeof(DASHBOARD_HTML) - 1);
    // which fires once with the full payload and silently
    // truncates on a 4096-byte send buffer. Any reversion
    // to that exact shape (or to a new
    // `httpd_resp_send(req, DASHBOARD_HTML, ...)` form)
    // must trip this assertion.
    auto badCall = body.find("httpd_resp_send(req, DASHBOARD_HTML");
    if (badCall != std::string::npos) {
        std::cout << "  FAIL: handleRoot still contains "
                     "`httpd_resp_send(req, DASHBOARD_HTML, ...)` — "
                     "re-introduces the silent-truncation bug.\n";
    }
    assert(badCall == std::string::npos);

    // Pin 2: the chunked path is in place. Must call
    // `httpd_resp_send_chunk(req, p, ...)` (the data path)
    // and a terminating `httpd_resp_send_chunk(req,
    // nullptr, 0)` (closes the chunked-encoded body so the
    // browser flushes its parser).
    auto dataChunk = body.find("httpd_resp_send_chunk(req, p,");
    auto termChunk = body.find("httpd_resp_send_chunk(req, nullptr, 0)");
    if (dataChunk == std::string::npos) {
        std::cout << "  FAIL: handleRoot never calls "
                     "`httpd_resp_send_chunk(req, p, ...)` — chunked "
                     "loop missing.\n";
    }
    assert(dataChunk != std::string::npos);
    if (termChunk == std::string::npos) {
        std::cout << "  FAIL: handleRoot does not terminate with "
                     "`httpd_resp_send_chunk(req, nullptr, 0)` — "
                     "chunked body never closes.\n";
    }
    assert(termChunk != std::string::npos);

    // Pin 3: the chunk size is bounded. ESP-IDF's httpd
    // send buffer tops out around 4096 bytes; chunks above
    // that re-introduce the truncation. The contract is
    // `const size_t CHUNK = 4096;` somewhere in the
    // function. (We don't pin the loop variable's name or
    // the comparison style — only the cap.)
    auto chunkCap = body.find("CHUNK = 4096");
    if (chunkCap == std::string::npos) {
        std::cout << "  FAIL: handleRoot does not declare "
                     "`const size_t CHUNK = 4096;` — chunk size "
                     "unbounded / mismatched.\n";
    }
    assert(chunkCap != std::string::npos);

    std::cout << "  PASS (chunked loop, terminator, and 4096-byte cap "
                 "all present)\n";
}


void test_handle_root_sets_required_headers() {
    std::cout << "\n=== handleRoot still sets content-type + cache headers ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWebHandlers.cpp");
    assert(!src.empty());

    std::string body = extractHandleRootBody(src);
    assert(!body.empty());

    // Pin: the response headers (text/html + no-store +
    // Connection: close) must remain. The chunked rewrite
    // touches the body path only — silently dropping a
    // header would change how the browser caches the page
    // or how it negotiates connection lifecycle.
    assert(body.find("text/html; charset=utf-8") != std::string::npos);
    assert(body.find("Cache-Control") != std::string::npos);
    assert(body.find("no-store") != std::string::npos);
    assert(body.find("Connection") != std::string::npos);

    std::cout << "  PASS (text/html + Cache-Control: no-store + "
                 "Connection: close all present)\n";
}


void test_httpd_stack_size_is_at_least_16384() {
    std::cout << "\n=== httpd task stack size bumped for chunked 28 KB send ==="
              << std::endl;
    std::string root = projectRoot();
    // cfg.stack_size = 16384 lives in the httpd config
    // block in AutoLinkWeb.cpp's setupHttpAndLogging_,
    // not in the handler TU.
    std::string src = readFile(root + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    // Scope to the httpd config block (same shape as
    // UriHandlerAlignmentTest) so a stray `stack_size = 16384`
    // mention in a comment elsewhere doesn't satisfy this
    // pin.
    std::string block = extractHttpConfigBlock(src);
    assert(!block.empty());

    auto pos = block.find("cfg.stack_size");
    assert(pos != std::string::npos);
    auto eq = block.find('=', pos);
    assert(eq != std::string::npos);
    auto semi = block.find(';', eq);
    assert(semi != std::string::npos);
    std::string value = block.substr(eq + 1, semi - eq - 1);
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t");
        size_t b = s.find_last_not_of(" \t");
        if (a == std::string::npos)
            return std::string();
        return s.substr(a, b - a + 1);
    };
    value = trim(value);

    int stackSize = std::atoi(value.c_str());
    // 6144 / 8192 (the old values) are enough for the small-
    // response path but the httpd worker stack still
    // exhausts under sustained chunked sends on a 28 KB
    // payload — the chunked loop's local state plus the
    // httpd internal send buffer frame can push the task
    // over its stack at runtime even though the build is
    // clean. 12288 might also work but 16384 gives
    // headroom; pin the lower bound at exactly 16384 so a
    // future regression to 6144 or 8192 trips here.
    if (stackSize < 16384) {
        std::cout << "  FAIL: cfg.stack_size = " << stackSize
                  << " (need >= 16384 for chunked 28 KB send).\n";
    }
    assert(stackSize >= 16384);
    assert(value == "16384");

    std::cout << "  PASS (cfg.stack_size = " << value
              << " in httpd config block)\n";
}

// Pin: setSink() must be called from begin(), not from
// setupHttpAndLogging_(). The web GUI must start capturing
// log messages before any meaningful work happens, so the
// app version line is the first entry in the live log.

int main() {
    std::cout << "=== Running HandleRootChunkedTest ===" << std::endl;

    Log::log().setLevel(Log::DEBUG);
    test_handle_root_uses_chunked_send();
    test_handle_root_sets_required_headers();
    test_httpd_stack_size_is_at_least_16384();

    std::cout << "\n=== HandleRootChunkedTest Completed ===" << std::endl;
    return 0;
}
