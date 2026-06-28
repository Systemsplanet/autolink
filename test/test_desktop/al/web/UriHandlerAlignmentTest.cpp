// Source-level regression test for AutoLinkWeb's HTTP URI
// handler registration. AutoLinkWeb.cpp is `#ifdef ARDUINO`
// so it can't run on host; the host suite only covers the
// dashboard core / log ring / level parsing path. The two
// startup bugs this test pins are otherwise invisible
// until the user flashes the firmware and the web
// interface "never starts":
//
//   1. HTTPD_DEFAULT_CONFIG() sets max_uri_handlers = 8.
//      AutoLinkWeb registers 9 handlers (r0..r8). The
//      last one silently fails with HANDLERS_FULL and
//      the route 404s at runtime. Symptom: the dashboard
//      loads, then any page that hits /reboot first
//      looks completely broken.
//   2. The PATHS[] array must be parallel to URIS[]. When
//      a registration fails, the log.error(...) line
//      prints PATHS[i] to identify the failing path. If
//      the two arrays drift out of order (e.g. /reboot
//      and /level get swapped), the log names the wrong
//      path and the operator chases a ghost.
//
// The fix is mechanical: cfg.max_uri_handlers = 9 right
// after cfg.lru_purge_enable, and PATHS[] laid out in the
// same order as URIS[]. This test reads the source and
// asserts both invariants hold. Toggling either off (e.g.
// dropping max_uri_handlers, or reordering PATHS[] to
// match the original /level /reboot swap) makes the test
// fail loudly under `make test_uri_handler_alignment`.
#ifndef ARDUINO

#    include <algorithm>
#    include <cassert>
#    include <fstream>
#    include <iostream>
#    include <iterator>
#    include <set>
#    include <sstream>
#    include <string>
#    include <vector>
#    include "NullArqCache.h"

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

// Returns the brace-balanced block that ENCLOSES `pos`.
// Scans backwards from `pos` to find the most recent
// unmatched `{`, then scans forward to its matching `}`.
// Used here so we can scope our assertions to the httpd
// config / registration block in AutoLinkWeb::begin,
// without depending on the function name being a true
// definition (httpd_start is a *call*, not a definition).
std::string extractEnclosingBracedBlock(const std::string &src,
                                        std::size_t pos) {
    if (pos == std::string::npos)
        return "";
    int back = 0;
    std::size_t openPos = std::string::npos;
    for (std::size_t i = pos; i > 0; i--) {
        if (src[i] == '}') {
            back++;
        } else if (src[i] == '{') {
            if (back == 0) {
                openPos = i;
                break;
            }
            back--;
        }
    }
    if (openPos == std::string::npos)
        return "";
    int depth = 1;
    std::size_t closePos = std::string::npos;
    for (std::size_t i = openPos + 1; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0) {
                closePos = i;
                break;
            }
        }
    }
    if (closePos == std::string::npos)
        return "";
    return src.substr(openPos, closePos + 1 - openPos);
}

// Counts the URI handler declarations of the form
// `const httpd_uri_t r<N>`. AutoLinkWeb registers 9 of
// these (r0..r8). Drift here is the root cause of the
// HANDLERS_FULL failure mode.
int countUriHandlerDecls(const std::string &src) {
    int n = 0;
    std::string needle = "const httpd_uri_t r";
    std::size_t pos = 0;
    while ((pos = src.find(needle, pos)) != std::string::npos) {
        n++;
        pos += needle.size();
    }
    return n;
}

// Parses the URIS[] initializer into an ordered vector of
// "&r0", "&r1", ..., "&rN" pointer names. The PATHS[]
// alignment is checked against this order: PATHS[i] must
// match the path that URIS[i]'s pointed-to handler
// declares in its r<N> initializer.
std::vector<std::string> parseUrisOrder(const std::string &src) {
    std::vector<std::string> out;
    auto pos = src.find("static const httpd_uri_t *const URIS[]");
    if (pos == std::string::npos)
        return out;
    auto brace = src.find('{', pos);
    if (brace == std::string::npos)
        return out;
    auto end = src.find('}', brace);
    if (end == std::string::npos)
        return out;
    std::string body = src.substr(brace, end - brace);
    auto cursor = body.begin();
    while (cursor != body.end()) {
        auto amp = std::find(cursor, body.end(), '&');
        if (amp == body.end())
            break;
        auto comma = std::find(amp, body.end(), ',');
        out.emplace_back(&*amp, std::distance(amp, comma));
        cursor = (comma == body.end()) ? comma : comma + 1;
    }
    return out;
}

// Same shape as parseUrisOrder but pulls the string
// literals out of PATHS[]. Order is the contract.
std::vector<std::string> parsePathsOrder(const std::string &src) {
    std::vector<std::string> out;
    auto pos = src.find("static const char *const PATHS[]");
    if (pos == std::string::npos)
        return out;
    auto brace = src.find('{', pos);
    if (brace == std::string::npos)
        return out;
    auto end = src.find('}', brace);
    if (end == std::string::npos)
        return out;
    std::string body = src.substr(brace, end - brace);
    auto cursor = body.begin();
    while (cursor != body.end()) {
        auto q1 = std::find(cursor, body.end(), '"');
        if (q1 == body.end())
            break;
        auto q2 = std::find(q1 + 1, body.end(), '"');
        if (q2 == body.end())
            break;
        out.emplace_back(q1 + 1, q2);
        cursor = q2 + 1;
    }
    return out;
}

// Looks up the path string declared inside the r<N>
// initializer. AutoLinkWeb uses the literal form
// `const httpd_uri_t r<N> = { "<path>", HTTP_*, ... }`,
// so we just pluck the first quoted string after `r<N>`.
std::string findPathForHandler(const std::string &src,
                               const std::string &pointerName) {
    // pointerName is like "&r4" — strip the leading '&'.
    std::string name = pointerName.substr(1);
    // Search for the DECLARATION, not any later reference.
    // The declaration is preceded by `const httpd_uri_t r<N>`
    // and followed by `= { "<path>". Anchoring on the
    // declaration prefix avoids a stray `&r4` hit inside
    // URIS[] satisfying this lookup.
    std::string declPrefix = "const httpd_uri_t " + name;
    auto pos = src.find(declPrefix);
    if (pos == std::string::npos)
        return "";
    auto q1 = src.find('"', pos);
    if (q1 == std::string::npos)
        return "";
    auto q2 = src.find('"', q1 + 1);
    if (q2 == std::string::npos)
        return "";
    return src.substr(q1 + 1, q2 - q1 - 1);
}

void test_max_uri_handlers_is_10() {
    std::cout
        << "\n=== AutoLinkWeb sets cfg.max_uri_handlers = 10 (HANDLERS_FULL guard) ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    // Scope to the httpd config / registration block —
    // a stray comment elsewhere that mentions the literal
    // 8 must not satisfy this gate. Anchor on
    // `cfg.lru_purge_enable` and walk back to its
    // enclosing brace-balanced block (the
    // `httpd_config_t cfg = HTTPD_DEFAULT_CONFIG(); ...`
    // arm). Walking forward from any line in the block
    // finds the *next* `{`, which is the inner
    // `if (hs != ESP_OK) { ... }` and not what we want.
    auto anchor = src.find("cfg.lru_purge_enable");
    assert(anchor != std::string::npos);
    std::string body = extractEnclosingBracedBlock(src, anchor);
    assert(!body.empty());

    // Pin: cfg.max_uri_handlers = 10;
    // Drop this line and 10 handlers will fight for 9
    // slots; the last handler (currently /mode/toggle)
    // silently fails with HANDLERS_FULL and 404s at runtime.
    auto pos = body.find("cfg.max_uri_handlers");
    assert(pos != std::string::npos);
    auto eq = body.find('=', pos);
    assert(eq != std::string::npos);
    auto semi = body.find(';', eq);
    assert(semi != std::string::npos);
    std::string value = body.substr(eq + 1, semi - eq - 1);
    // Trim whitespace.
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t");
        size_t b = s.find_last_not_of(" \t");
        if (a == std::string::npos)
            return std::string();
        return s.substr(a, b - a + 1);
    };
    value = trim(value);
    assert(value == "10");

    std::cout << "  PASS (cfg.max_uri_handlers = 10 in httpd config block)\n";
}

void test_uri_handler_count_matches_capacity() {
    std::cout
        << "\n=== AutoLinkWeb declares exactly as many handlers as max_uri_handlers allows ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    int n = countUriHandlerDecls(src);
    // 10 handler slots, 10 declarations. Drift either side
    // (drop one r<N>, or add an eleventh without bumping
    // max_uri_handlers) re-introduces the silent
    // HANDLERS_FULL failure mode.
    assert(n == 10);
    std::cout << "  PASS (10 r<N> handler declarations; matches "
              << "max_uri_handlers = 10)\n";
}

void test_paths_matches_uris_order() {
    std::cout
        << "\n=== PATHS[] is parallel to URIS[] (log.error names the right path on failure) ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    auto uris = parseUrisOrder(src);
    auto paths = parsePathsOrder(src);
    assert(uris.size() == 10);
    assert(paths.size() == 10);
    assert(uris.size() == paths.size());

    // For each i: PATHS[i] must equal the path declared
    // in the r<N> that URIS[i] points at. The original
    // bug swapped /reboot and /level in PATHS[] — every
    // registration worked, but the log line on the last
    // silent failure pointed at the wrong path.
    for (size_t i = 0; i < uris.size(); i++) {
        std::string declared = findPathForHandler(src, uris[i]);
        assert(!declared.empty());
        if (declared != paths[i]) {
            std::cout << "  FAIL at URIS[" << i << "] (" << uris[i] << "): "
                      << "PATHS[" << i << "] is \"" << paths[i]
                      << "\" but handler declares \"" << declared << "\"\n";
            assert(false);
        }
    }
    std::cout << "  PASS (PATHS[" << uris.size() << "] parallel to URIS["
              << uris.size() << "], all entries match)\n";
}

void test_paths_array_contains_all_ten_routes() {
    std::cout
        << "\n=== PATHS[] contains every route registered (no missing / extra) ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    auto paths = parsePathsOrder(src);
    // 10 unique routes. A duplicate in PATHS[] (e.g.
    // listed twice after a refactor) would make
    // sizeof(PATHS) != sizeof(URIS) and silently mis-name
    // every registration after the first duplicate.
    std::set<std::string> seen;
    for (const auto &p : paths) {
        assert(seen.insert(p).second);
    }
    assert(seen.size() == 10);

    // Sanity: the must-have routes are all there. If a
    // future refactor drops /reboot from PATHS[] (leaving
    // it in URIS[]), this catches it.
    static const char *const mustHave[] = {
        "/",     "/stats",    "/logs",     "/reset", "/reboot", "/level",
        "/mode", "/fillmode", "/pausemsg", "/delay", nullptr,
    };
    for (int i = 0; mustHave[i]; i++) {
        assert(seen.count(mustHave[i]) == 1);
    }
    std::cout << "  PASS (10 unique routes, every required route present)\n";
}

} // namespace

int main() {
    std::cout << "=== Running AutoLinkWeb URI Handler Alignment Tests ==="
              << std::endl;
    test_max_uri_handlers_is_10();
    test_uri_handler_count_matches_capacity();
    test_paths_matches_uris_order();
    test_paths_array_contains_all_ten_routes();
    std::cout << "\n=== URI Handler Alignment Tests Completed ===" << std::endl;
    return 0;
}

#endif
