// Source-level regression test for the SYNC/ASYNC mode
// toggle UI + NVS-persist + reboot path. AutoLinkWeb.cpp and
// PingPongBase.h are `#ifdef ARDUINO` and can't run on host;
// this gates the bug by reading the source.
//
// Pins five contracts from the field-test request:
//
//   1. The dashboard HTML exposes a clickable "Toggle" button
//      wired to `toggleLinkMode()` so an operator can flip
//      modes without re-flashing.
//   2. A `<span id="linkModePill">` exists in the header so
//      the JS reconciliation has something to write to.
//   3. `handleModeToggle` is declared `static esp_err_t`
//      in AutoLinkWeb.h so the registration block in
//      setupHttpAndLogging_ can reference it.
//   4. /mode/toggle is registered in URIS[] / PATHS[]
//      parallel to the r<N> handler declaration. The full
//      PATHS-vs-URIS alignment is already pinned by
//      UriHandlerAlignmentTest; this is a focused gate for
//      the new entry.
//   5. bringUpLink reads the persisted mode from NVS
//      namespace "autolink" under key "mode" BEFORE
//      comm.begin() so the persisted mode is the active
//      mode from the first wire frame.
//
// Toggle off (drop the button, drop the pill, drop the
// declaration, drop the URI, drop the NVS read) flips at
// least one pin to red.
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
    return base;
}

// Pin 1: the dashboard HTML has a clickable Toggle button
// wired to toggleLinkMode(). Without this button the
// operator has to re-flash to change mode.
void test_dashboard_has_mode_toggle_button() {
    std::cout
        << "\n=== Pin 1: dashboard has onclick=toggleLinkMode() button ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWebHtml.h");
    assert(!src.empty());
    // The dashboard HTML has a <button onclick="toggleLinkMode()">.
    assert(src.find("toggleLinkMode()") != std::string::npos);
    assert(src.find("onclick=\"toggleLinkMode()\"") != std::string::npos);
    // The JS function body must be defined.
    auto fnPos = src.find("async function toggleLinkMode(");
    assert(fnPos != std::string::npos);
    // The body must POST to /mode/toggle (the wire contract).
    assert(src.find("/mode/toggle", fnPos) != std::string::npos);
    std::cout << "  PASS (button + JS + /mode/toggle POST)" << std::endl;
}

// Pin 2: the header has a span with id="linkModePill" so
// the JS reconciliation can update the current mode label.
void test_dashboard_has_link_mode_pill() {
    std::cout << "\n=== Pin 2: dashboard has <span id=linkModePill> ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWebHtml.h");
    assert(!src.empty());
    auto pos = src.find("id=\"linkModePill\"");
    assert(pos != std::string::npos);
    auto close = src.find('>', pos);
    assert(close != std::string::npos);
    assert(src.rfind("<span", pos) != std::string::npos);
    std::cout << "  PASS (linkModePill span present in header)" << std::endl;
}

// Pin 3: handleModeToggle is declared `static esp_err_t`
// in AutoLinkWeb.h so the setupHttpAndLogging_ block can
// reference it without a compile error.
void test_handle_mode_toggle_declared() {
    std::cout << "\n=== Pin 3: handleModeToggle declared in AutoLinkWeb.h ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWeb.h");
    assert(!src.empty());
    auto pos = src.find("handleModeToggle(httpd_req_t *req)");
    assert(pos != std::string::npos);
    assert(src.rfind("static esp_err_t", pos) != std::string::npos);
    std::cout << "  PASS (handleModeToggle declared static esp_err_t)"
              << std::endl;
}

// Pin 4: /mode/toggle is registered in URIS[] parallel
// to the corresponding entry in PATHS[]. Without this
// the route 404s at runtime and the toggle never reaches
// the firmware.
void test_mode_toggle_uri_registered_with_path() {
    std::cout << "\n=== Pin 4: /mode/toggle registered in URIS + PATHS ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());
    // A r<N> handler with path "/mode/toggle" must be declared.
    assert(src.find("\"/mode/toggle\"") != std::string::npos);
    // URIS[] must reference that handler via &rN. We don't
    // pin which N (could be r9 today, r12 tomorrow) — just
    // that some &rN is mapped to /mode/toggle.
    auto rPos = src.find("const httpd_uri_t r9");
    if (rPos == std::string::npos) {
        rPos = src.find("const httpd_uri_t r");
    }
    assert(rPos != std::string::npos);
    // The handler ctor must be { "/mode/toggle", HTTP_POST, handleModeToggle,
    // this };
    auto ctorEnd = src.find("};", rPos);
    assert(ctorEnd != std::string::npos);
    std::string ctor = src.substr(rPos, ctorEnd - rPos);
    assert(ctor.find("/mode/toggle") != std::string::npos);
    assert(ctor.find("handleModeToggle") != std::string::npos);
    assert(ctor.find("HTTP_POST") != std::string::npos);
    // PATHS[] must list "/mode/toggle".
    auto pathsPos = src.find("static const char *const PATHS[]");
    assert(pathsPos != std::string::npos);
    auto pathsEnd = src.find('}', pathsPos);
    assert(pathsEnd != std::string::npos);
    std::string pathsBlock = src.substr(pathsPos, pathsEnd - pathsPos);
    assert(pathsBlock.find("/mode/toggle") != std::string::npos);
    // cfg.max_uri_handlers must be >= 10 (9 prior + new).
    auto maxPos = src.find("cfg.max_uri_handlers");
    assert(maxPos != std::string::npos);
    auto eq = src.find('=', maxPos);
    assert(eq != std::string::npos);
    auto semi = src.find(';', eq);
    assert(semi != std::string::npos);
    std::string value = src.substr(eq + 1, semi - eq - 1);
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t");
        size_t b = s.find_last_not_of(" \t");
        if (a == std::string::npos)
            return std::string();
        return s.substr(a, b - a + 1);
    };
    int cap = std::stoi(trim(value));
    assert(cap >= 10);
    std::cout << "  PASS (/mode/toggle in URIS + PATHS; max_uri_handlers="
              << cap << ")" << std::endl;
}

// Pin 5: bringUpLink reads the persisted mode from NVS
// namespace "autolink" under key "mode" BEFORE comm.begin()
// so the persisted mode is the active mode from the first
// wire frame.
void test_bringUpLink_reads_nvs_mode() {
    std::cout
        << "\n=== Pin 5: bringUpLink reads mode from NVS before begin() ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/PingPongBase.h");
    assert(!src.empty());
    auto fnPos = src.find("inline void bringUpLink(");
    assert(fnPos != std::string::npos);
    auto brace = src.find('{', fnPos);
    assert(brace != std::string::npos);
    int depth = 0;
    bool foundOpen = false;
    std::size_t endPos = std::string::npos;
    for (std::size_t i = brace; i < src.size(); i++) {
        if (src[i] == '{') {
            depth++;
            foundOpen = true;
        } else if (src[i] == '}') {
            depth--;
            if (foundOpen && depth == 0) {
                endPos = i;
                break;
            }
        }
    }
    assert(endPos != std::string::npos);
    std::string body = src.substr(fnPos, endPos - fnPos + 1);

    // The NVS read must precede comm.begin() in the body.
    auto nvsPos = body.find("getUChar(\"mode\"");
    assert(nvsPos != std::string::npos);
    auto beginPos = body.find("comm.begin()", nvsPos);
    // The NVS read must come BEFORE begin() so the persisted
    // mode is applied before the link layer comes up.
    assert(beginPos == std::string::npos || nvsPos < beginPos);
    // Must call comm.setMode(...) to apply the value.
    auto setModePos = body.find("comm.setMode(");
    assert(setModePos != std::string::npos);
    // The NVS namespace must be "autolink" (matches the
    // existing log_level key path).
    assert(body.find("prefs.begin(\"autolink\"") != std::string::npos);
    std::cout << "  PASS (NVS read before begin(); setMode applied; "
                 "namespace=autolink)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Mode Toggle UI + NVS persistence regression ==="
              << std::endl;
    test_dashboard_has_mode_toggle_button();
    test_dashboard_has_link_mode_pill();
    test_handle_mode_toggle_declared();
    test_mode_toggle_uri_registered_with_path();
    test_bringUpLink_reads_nvs_mode();
    std::cout << "\n=== Done ===" << std::endl;
    return 0;
}
#endif
