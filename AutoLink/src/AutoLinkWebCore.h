// AutoLinkWebCore.h — host-testable dashboard core.
//
// Splits AutoLinkWeb into two parts:
//
//   * AutoLinkWebCore (this file + AutoLinkWebCore.cpp): the testable
//     state machine — Snapshot, LogEntry ring, format functions, level
//     validation, role-conditional UI decisions. Pure C/C++ with no
//     Arduino, FreeRTOS, or esp_http_server dependencies. Compiles
//     cleanly on host (Linux) and on Arduino.
//
//   * AutoLinkWeb (AutoLinkWeb.h + AutoLinkWeb.cpp): the Arduino-only
//     glue — WiFi connection, NVS persistence, esp_http_server, esp_timer,
//     1 Hz snapshot timer. Wraps the core and exposes the same
//     user-facing API as before.
//
// The split exists so that the dashboard's correctness (JSON format,
// log ring invariants, level validation, role-conditional UI class
// names, version string) can be verified by the host test suite
// without an ESP32. Every change to AutoLinkWeb.cpp that affects
// observable behavior should land in the core and have a host test.
#pragma once
#include "Log.h"
#include <stdint.h>
#include <stddef.h>

namespace autolink {

// ---- tunables (must match AutoLinkWeb.h) --------------------------------
static constexpr int  WEB_RING_CAP  = 200;   // log ring capacity
static constexpr int  WEB_LINE_CAP  = 180;   // max bytes per log line (incl. NUL)

// Log entry as stored in the ring. 32 bytes per entry + LINE_CAP
// string. 200 entries ≈ 36 KB at default LINE_CAP.
struct WebLogEntry {
    uint32_t seq;
    char     sev;            // 'E', 'I', 'D', 'W', 'V'
    char     line[WEB_LINE_CAP];
};

// Snapshot built by the 1 Hz timer, served by /stats. The 1 Hz timer
// (in AutoLinkWeb.cpp) writes directly to a WebSnapshot, and the
// handler passes it to formatStatsJson() — no conversion needed.
// AutoLinkWeb.h aliases this as `Snapshot` via `using` so the timer's
// `self->snap_` is the same type.
struct WebSnapshot {
    char     state[4];
    uint32_t errCount;
    uint32_t txBps, rxBps;
    uint64_t txTotal, rxTotal, errTotal;
    uint64_t lostMsgs;
    int32_t  rssi;
    uint32_t freeHeap;
    uint32_t uptimeS;
    uint32_t baudRate;
    uint8_t  fillMode;
    char     role[8];
};

// Format a WebSnapshot into a JSON byte buffer. Returns the number
// of bytes written (not counting NUL). The buffer must be at least
// 512 bytes. On overflow, the result is truncated but the JSON
// remains well-formed (the last field is `version` and is always
// last). The output is a single object — no streaming, no chunking.
//
// The format is consumed by the dashboard JS — see
// AutoLinkWeb.cpp::handleStats for the field list. If you change
// a field name or remove a field, update the JS too.
int formatStatsJson(const WebSnapshot* s,
                    int logLevel,
                    const char* version,
                    char* out, int outLen);

// Format the log ring into a JSON byte buffer. Returns the number
// of bytes written. Includes the current `head` so the client can
// skip the boot-time backlog on first poll (see the JS poll handler
// in AutoLinkWeb.cpp).
//
// `since` is the last seq the client has already seen. Entries with
// seq < since are skipped. Entries older than the ring capacity are
// also skipped (the ring holds at most RING_CAP entries).
//
// The buffer must be large enough to hold the full response — at
// 200 entries × ~250 bytes per JSON-escaped line, ~50 KB. Caller
// typically allocates on the stack for the chunked response or
// streams via httpd_resp_send_chunk.
int formatLogsJson(const WebLogEntry* ring,
                   uint32_t head,
                   uint32_t since,
                   char* out, int outLen);

// Parse a "?lv=N" query value. Returns:
//   0  if N is a valid log level (1..5) and not Log::NONE
//  -1  if missing or malformed
//  -2  if lv == 0 (NONE — rejected because it silences the logger
//      and is unrecoverable without reflash)
//  -3  if lv > Log::VERBOSE
int parseLevelQuery(const char* val);

// Parse a "?m=seq|rand" mode value. Returns 0 (seq) or 1 (rand), or
// -1 if invalid.
int parseModeQuery(const char* val);

// Return true if a role string ("Ping", "Pong", etc.) is non-empty
// and short enough to fit in the WebSnapshot.role field.
bool validRoleString(const char* role);

// Apply a level change to the Log singleton and return the new
// level on success, or -1 if lv is invalid (none / out of range).
// Does NOT persist to NVS — that's the caller's job (NVS is
// Arduino-only).
int applyLogLevel(int lv);

// ---- HTML template accessors --------------------------------------------
//
// The dashboard HTML is a single big string. These accessors expose
// it (and a small bit of metadata) so host tests can verify the
// presence of role-conditional UI elements without parsing HTML.

// The full HTML for the dashboard, including the embedded CSS + JS.
// Length excludes NUL. The string is owned by the .cpp file.
const char* dashboardHtml();
int         dashboardHtmlLen();

// True if the dashboard HTML contains a given substring. Used by
// host tests to verify class names, IDs, and conditional UI
// elements without parsing HTML.
bool dashboardContains(const char* needle);

} // namespace autolink
