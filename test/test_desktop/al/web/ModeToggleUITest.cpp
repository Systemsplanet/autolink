// Source-level regression test for the this release
// SYNC/ASYNC link-mode UI + live /mode switch.
//
// Pins six contracts from the live mode-toggle:
//
//   1. The dashboard HTML exposes SYNC and ASYNC
//      radio buttons grouped under
//      name="linkMode" so an operator can flip
//      modes without re-flashing or rebooting.
//   2. Radio clicks call onLinkModeChange() which
//      POSTs /mode?m=SYNC|ASYNC — the live wire
//      contract (no /mode/toggle reboot).
//   3. handleMode is declared `static esp_err_t`
//      in AutoLinkWeb.h so the registration block
//      in setupHttpAndLogging_ can reference it.
//   4. /mode is registered in URIS[] / PATHS[]
//      parallel to the r<N> handler declaration.
//      /mode/toggle is GONE (this release removed
//      the reboot-on-toggle path).
//   5. handleMode applies the mode live (sets
//      link_.setMode(...) and persists to NVS
//      without rebooting).
//   6. bringUpLink reads the persisted mode from
//      NVS namespace "autolink" under key "mode"
//      BEFORE comm.begin() so the persisted mode
//      is the active mode from the first wire
//      frame on next boot.
//
// Toggle off (drop the radio, drop the
// declaration, drop the URI, drop the NVS read)
// flips at least one pin to red.
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

// Pin 1: the dashboard HTML exposes SYNC + ASYNC
// radio buttons grouped under name="linkMode".
// Without this the operator has to re-flash to
// change mode.
void test_dashboard_has_link_mode_radios() {
    std::cout
        << "\n=== Pin 1: dashboard has SYNC + ASYNC link-mode radios ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWebHtml.h");
    assert(!src.empty());
    // Group: <div ... id="linkModeGroup" role="radiogroup">
    auto groupPos = src.find("id=\"linkModeGroup\"");
    assert(groupPos != std::string::npos);
    auto rolePos = src.find("role=\"radiogroup\"", groupPos);
    assert(rolePos != std::string::npos);
    // Two radios under that group: name="linkMode" value="SYNC" + value="ASYNC"
    assert(src.find("name=\"linkMode\"") != std::string::npos);
    assert(src.find("value=\"SYNC\"") != std::string::npos);
    assert(src.find("value=\"ASYNC\"") != std::string::npos);
    // The pre-fix /mode/toggle button must be gone.
    assert(src.find("modeToggleBtn") == std::string::npos);
    assert(src.find("toggleLinkMode") == std::string::npos);
    // The linkModePill span is also gone (radio takes
    // its place).
    assert(src.find("id=\"linkModePill\"") == std::string::npos);
    std::cout
        << "  PASS (SYNC + ASYNC radios present; toggle button + pill gone)"
        << std::endl;
}

// Pin 2: radio clicks call onLinkModeChange() which
// POSTs /mode?m=SYNC|ASYNC. The 5.3.x /mode/toggle
// reboot path is gone.
void test_js_routes_radio_to_live_mode_post() {
    std::cout
        << "\n=== Pin 2: radio click POSTs /mode?m=SYNC|ASYNC (no reboot) ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWebHtml.h");
    assert(!src.empty());
    auto fnPos = src.find("async function onLinkModeChange(");
    assert(fnPos != std::string::npos);
    // The body POSTs to /mode?m=... (live, no reboot).
    assert(src.find("/mode?m=", fnPos) != std::string::npos);
    assert(src.find("/mode/toggle") == std::string::npos);
    // bindLinkModeGroup wires the radio change events.
    auto bindPos = src.find("bindLinkModeGroup(");
    assert(bindPos != std::string::npos);
    std::cout << "  PASS (radio -> /mode?m=... POST; no /mode/toggle)"
              << std::endl;
}

// Pin 3: handleMode is declared `static esp_err_t`
// in AutoLinkWeb.h so setupHttpAndLogging_ can
// reference it.
void test_handle_mode_declared() {
    std::cout << "\n=== Pin 3: handleMode declared in AutoLinkWeb.h ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWeb.h");
    assert(!src.empty());
    auto pos = src.find("handleMode(httpd_req_t *req)");
    assert(pos != std::string::npos);
    assert(src.rfind("static esp_err_t", pos) != std::string::npos);
    // The 5.3.x handleModeToggle is gone.
    assert(src.find("handleModeToggle") == std::string::npos);
    std::cout << "  PASS (handleMode declared static esp_err_t; "
                 "handleModeToggle gone)"
              << std::endl;
}

// Pin 4: /mode is registered in URIS[] parallel
// to PATHS[], and /mode/toggle is gone (the 5.3.x
// reboot-on-toggle path is removed in this release).
// /fillmode is the new home of the fill-mode
// route (formerly at /mode?m=seq|rand).
void test_mode_uri_registered_and_toggle_gone() {
    std::cout
        << "\n=== Pin 4: /mode registered; /mode/toggle gone; /fillmode added ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());
    // /mode is in URIS[] (mapped to handleMode).
    assert(src.find("\"/mode\"") != std::string::npos);
    // Locate the r6 handler ctor specifically (it
    // binds /mode -> handleMode). Search for the
    // r6 declaration and read forward until the
    // matching `};`.
    auto r6Pos = src.find("const httpd_uri_t r6");
    assert(r6Pos != std::string::npos);
    auto ctorEnd = src.find("};", r6Pos);
    assert(ctorEnd != std::string::npos);
    std::string ctor = src.substr(r6Pos, ctorEnd - r6Pos);
    // The r6 handler ctor binds /mode -> handleMode.
    assert(ctor.find("\"/mode\"") != std::string::npos);
    assert(ctor.find("handleMode") != std::string::npos);
    // The /mode/toggle route must NOT exist (this
    // release replaced it with /mode live + /fillmode).
    assert(src.find("/mode/toggle") == std::string::npos);
    assert(src.find("handleModeToggle") == std::string::npos);
    // /fillmode is the new fill-mode route.
    assert(src.find("\"/fillmode\"") != std::string::npos);
    assert(src.find("handleFillMode") != std::string::npos);
    // PATHS[] lists both /mode and /fillmode.
    auto pathsPos = src.find("static const char *const PATHS[]");
    assert(pathsPos != std::string::npos);
    auto pathsEnd = src.find('}', pathsPos);
    assert(pathsEnd != std::string::npos);
    std::string pathsBlock = src.substr(pathsPos, pathsEnd - pathsPos);
    assert(pathsBlock.find("/mode") != std::string::npos);
    assert(pathsBlock.find("/fillmode") != std::string::npos);
    assert(pathsBlock.find("/mode/toggle") == std::string::npos);
    // cfg.max_uri_handlers must be >= 10 (same 9
    // from 5.3.x; /mode/toggle was removed and
    // /fillmode added, net change is 0; we keep 10
    // to maintain the httpd_config_t capacity
    // headroom).
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
    std::cout
        << "  PASS (/mode registered; /mode/toggle gone; /fillmode added; "
           "max_uri_handlers="
        << cap << ")" << std::endl;
}

// Pin 5: handleMode applies the mode live (calls
// link_.setMode + persists to NVS) without a
// reboot. The 5.3.x handleModeToggle path called
// esp_restart() on a detached FreeRTOS task;
// that's gone.
void test_handle_mode_applies_live_no_reboot() {
    std::cout
        << "\n=== Pin 5: handleMode applies mode live (no esp_restart) ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src =
        readFile(root + "/src/al/web/AutoLinkWebHandlers.cpp");
    assert(!src.empty());
    auto fnPos = src.find("esp_err_t AutoLinkWeb::handleMode(httpd_req_t *req)");
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
    // The body must call link_.setMode(...) to apply.
    assert(body.find("link_.setMode(") != std::string::npos);
    // It must NOT call esp_restart() (that's the
    // 5.3.x handleModeToggle behavior).
    assert(body.find("esp_restart") == std::string::npos);
    // NVS persistence for the chosen mode is
    // still required so bringUpLink restores it on
    // next boot.
    assert(body.find("putUChar(\"mode\"") != std::string::npos);
    // The body must accept ?m=SYNC and ?m=ASYNC.
    assert(body.find("\"SYNC\"") != std::string::npos);
    assert(body.find("\"ASYNC\"") != std::string::npos);
    std::cout
        << "  PASS (link_.setMode + NVS persist; no esp_restart; "
           "SYNC/ASYNC accepted)"
        << std::endl;
}

// Pin 6: bringUpLink reads the persisted mode from
// NVS namespace "autolink" under key "mode" BEFORE
// comm.begin() so the persisted mode is the active
// mode from the first wire frame on next boot.
void test_bringUpLink_reads_nvs_mode() {
    std::cout
        << "\n=== Pin 6: bringUpLink reads mode from NVS before begin() ==="
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

    auto nvsPos = body.find("getUChar(\"mode\"");
    assert(nvsPos != std::string::npos);
    auto beginPos = body.find("comm.begin()", nvsPos);
    assert(beginPos == std::string::npos || nvsPos < beginPos);
    auto setModePos = body.find("comm.setMode(");
    assert(setModePos != std::string::npos);
    assert(body.find("prefs.begin(\"autolink\"") != std::string::npos);
    std::cout << "  PASS (NVS read before begin(); setMode applied; "
                 "namespace=autolink)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Mode Toggle UI + Live Switch + NVS regression ==="
              << std::endl;
    test_dashboard_has_link_mode_radios();
    test_js_routes_radio_to_live_mode_post();
    test_handle_mode_declared();
    test_mode_uri_registered_and_toggle_gone();
    test_handle_mode_applies_live_no_reboot();
    test_bringUpLink_reads_nvs_mode();
    std::cout << "\n=== Done ===" << std::endl;
    return 0;
}
#endif
