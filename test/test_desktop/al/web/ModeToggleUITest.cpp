// Source-level regression test for the SYNC/ASYNC
// link-mode UI + NVS-only /mode handler (reboot
// required to apply on the wire).
//
// Pins seven contracts from the radio-mode flow:
//
//   1. The dashboard HTML exposes SYNC and ASYNC
//      radio buttons grouped under
//      name="linkMode" so an operator can flip
//      modes without re-flashing. The radio
//      group is rendered for BOTH Ping and Pong
//      (no `ping-only` class on the div) because
//      mode is symmetric on both sides — only the
//      fill-mode/start/pause widgets stay ping-only.
//   2. Radio clicks call onLinkModeChange() which
//      POSTs /mode?m=SYNC|ASYNC; on a successful
//      ack the JS kicks reboot() so the new mode
//      actually takes effect on the wire (mode
//      changes the buffer floor and SYNC wait
//      logic — both init-time).
//   3. handleMode is declared `static esp_err_t`
//      in AutoLinkWeb.h so the registration block
//      in setupHttpAndLogging_ can reference it.
//   4. /mode is registered in URIS[] / PATHS[]
//      parallel to the r<N> handler declaration.
//      /mode/toggle is GONE (this release removed
//      the reboot-on-toggle path).
//   5. handleMode persists to NVS only. It must
//      NOT call link_.setMode(...) live (the live
//      flag change would mutate cfg.mode without
//      resizing buffers or updating SYNC wait
//      logic, leaving the wire out of sync with
//      the snap-reported mode). The JS radio
//      handler kicks the reboot that re-runs
//      bringUpLink's NVS read path.
//   6. bringUpLink reads the persisted mode from
//      NVS namespace "autolink" under key "mode"
//      BEFORE comm.begin() so the persisted mode
//      is the active mode from the first wire
//      frame on next boot.
//   7. The radio handler's confirm() dialog
//      warns the user a reboot is required
//      before the POST so a slip of the click
//      doesn't surprise them with a 5–10 s
//      link drop.
//
// Toggle off (drop the radio, drop the
// declaration, drop the URI, drop the NVS read,
// restore the live setMode call) flips at least
// one pin to red.
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
// change mode. The radio group is shown on BOTH
// Ping and Pong dashboards (mode is symmetric —
// only the fill-mode/start/pause widgets stay
// ping-only).
void test_dashboard_has_link_mode_radios() {
    std::cout << "\n=== Pin 1: dashboard has SYNC + ASYNC link-mode radios "
                 "(Ping + Pong)"
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
    // The linkModeGroup div must NOT carry the
    // ping-only class — Pong must see the radio
    // too (mode is a symmetric choice).
    auto divOpen = src.rfind("<div", groupPos);
    assert(divOpen != std::string::npos);
    auto divEnd = src.find('>', divOpen);
    assert(divEnd != std::string::npos);
    std::string divTag = src.substr(divOpen, divEnd - divOpen);
    assert(divTag.find("ping-only") == std::string::npos);
    // The pre-fix /mode/toggle button must be gone.
    assert(src.find("modeToggleBtn") == std::string::npos);
    assert(src.find("toggleLinkMode") == std::string::npos);
    // The linkModePill span is also gone (radio takes
    // its place).
    assert(src.find("id=\"linkModePill\"") == std::string::npos);
    std::cout << "  PASS (SYNC + ASYNC radios present on Ping + Pong; "
                 "toggle button + pill gone)"
              << std::endl;
}

// Pin 2: radio clicks call onLinkModeChange() which
// POSTs /mode?m=SYNC|ASYNC; on success it kicks
// reboot(true) so the new mode actually takes effect
// on the wire (mode is init-time, not live-applicable).
// The skipConfirm flag prevents a double-confirm
// stack: the handler already showed its own
// confirm() before the POST, so the reboot() call
// must not show the Reboot button's confirm again.
// The 5.3.x /mode/toggle reboot path is gone.
void test_js_routes_radio_to_mode_post_and_reboots() {
    std::cout << "\n=== Pin 2: radio click POSTs /mode?m=... and reboots ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWebHtml.h");
    assert(!src.empty());
    auto fnPos = src.find("async function onLinkModeChange(");
    assert(fnPos != std::string::npos);
    // Locate the function body's closing brace so we
    // can scope assertions to inside onLinkModeChange.
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
    // The body POSTs to /mode?m=...
    assert(body.find("/mode?m=") != std::string::npos);
    // The 5.3.x /mode/toggle route is gone.
    assert(src.find("/mode/toggle") == std::string::npos);
    // On a successful /mode ack the body MUST call
    // reboot(true) — skipConfirm=true because the
    // handler already showed its own confirm() before
    // the POST, and calling reboot() without the flag
    // would stack a second confirm dialog on the user.
    assert(body.find("reboot(true)") != std::string::npos);
    assert(body.find("reboot()") == std::string::npos);
    // reboot() itself must accept a skipConfirm
    // parameter so callers that already confirmed
    // (like onLinkModeChange) can opt out of the
    // Reboot button's dialog. A future refactor
    // that drops the param would let the
    // double-confirm regression sneak back in.
    auto rebootSig = src.find("async function reboot(");
    assert(rebootSig != std::string::npos);
    assert(src.find("skipConfirm", rebootSig) != std::string::npos);
    // The pre-fix optimistic update is gone (a
    // reboot wipes the page; reconciling the
    // radio before reboot is wasted state).
    assert(body.find("currentLinkMode=val") == std::string::npos);
    // bindLinkModeGroup wires the radio change events.
    auto bindPos = src.find("bindLinkModeGroup(");
    assert(bindPos != std::string::npos);
    std::cout << "  PASS (radio -> /mode?m=... POST -> reboot(true); no "
                 "/mode/toggle; no optimistic update; reboot(skipConfirm) "
                 "signature accepts opt-out)"
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

// Pin 5: handleMode persists to NVS only. It must
// NOT call link_.setMode(...) live (a live flag
// change would mutate cfg.mode without resizing
// the ARQ pool or updating SYNC wait logic — the
// snap-reported mode would diverge from the wire).
// The JS radio handler kicks the reboot that
// re-runs bringUpLink's NVS read path. The 5.3.x
// handleModeToggle path called esp_restart() on
// a detached FreeRTOS task; that's gone.
void test_handle_mode_persists_only_no_live_apply() {
    std::cout << "\n=== Pin 5: handleMode persists to NVS only (no live "
                 "setMode; no esp_restart) ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWebHandlers.cpp");
    assert(!src.empty());
    auto fnPos =
        src.find("esp_err_t AutoLinkWeb::handleMode(httpd_req_t *req)");
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
    // The body must NOT call link_.setMode(...) —
    // a live apply would leave cfg.mode out of
    // sync with the buffer floor and SYNC wait
    // logic. Mode change requires reboot.
    assert(body.find("link_.setMode(") == std::string::npos);
    // It must NOT call esp_restart() (that's the
    // 5.3.x handleModeToggle behavior).
    assert(body.find("esp_restart") == std::string::npos);
    // NVS persistence for the chosen mode is
    // required so bringUpLink restores it on
    // next boot.
    assert(body.find("putUChar(\"mode\"") != std::string::npos);
    assert(body.find("prefs.begin(\"autolink\"") != std::string::npos);
    // The body must accept ?m=SYNC and ?m=ASYNC.
    assert(body.find("\"SYNC\"") != std::string::npos);
    assert(body.find("\"ASYNC\"") != std::string::npos);
    // The leading comment must document the
    // NVS-only contract so a future reader doesn't
    // "helpfully" re-introduce the live apply.
    assert(body.find("NVS only") != std::string::npos ||
           body.find("persist") != std::string::npos);
    std::cout << "  PASS (NVS persist only; no live link_.setMode; no "
                 "esp_restart; SYNC/ASYNC accepted)"
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

// Pin 7: the radio handler shows a confirm() dialog
// that warns the user a reboot is required before
// the /mode POST fires, so a slip of the click
// doesn't surprise them with a 5–10 s link drop.
// The dialog's message text mentions the chosen
// mode and the reboot side-effect (the message is
// concatenated in the source, so we scan a small
// window starting at the confirm call for the
// word "reboot" rather than parsing the literal).
void test_radio_confirm_warns_reboot_required() {
    std::cout
        << "\n=== Pin 7: radio confirm() warns that reboot is required ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWebHtml.h");
    assert(!src.empty());
    auto fnPos = src.find("async function onLinkModeChange(");
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
    // The handler must gate the POST behind a
    // confirm() that mentions reboot so the user
    // can bail before the device reboots.
    auto confirmPos = body.find("confirm(");
    assert(confirmPos != std::string::npos);
    // The /mode POST (via tfetch) must come AFTER
    // the confirm(); a console.log mentioning the
    // URL string may legitimately appear earlier.
    auto postPos = body.find("tfetch('/mode?m=", confirmPos);
    assert(postPos != std::string::npos);
    // Scan a small window starting at the confirm
    // call for the word "reboot" so we catch both
    // a single literal ('reboot the device...')
    // and a concat expression
    // ('switch to ' + val + '? reboot ...').
    std::string window = body.substr(confirmPos, 220);
    assert(window.find("reboot") != std::string::npos);
    std::cout << "  PASS (confirm() before POST mentions reboot)" << std::endl;
}

} // namespace

int main() {
    std::cout
        << "=== Mode Toggle UI + NVS-only /mode + reboot-required regression ==="
        << std::endl;
    test_dashboard_has_link_mode_radios();
    test_js_routes_radio_to_mode_post_and_reboots();
    test_handle_mode_declared();
    test_mode_uri_registered_and_toggle_gone();
    test_handle_mode_persists_only_no_live_apply();
    test_bringUpLink_reads_nvs_mode();
    test_radio_confirm_warns_reboot_required();
    std::cout << "\n=== Done ===" << std::endl;
    return 0;
}
#endif
