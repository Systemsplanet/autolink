# 📡 AutoLink Web Monitor

`AutoLinkWeb` adds a self-contained WiFi dashboard to any AutoLink sketch. Include `AutoLinkWeb.h`, construct a monitor object that wraps your `AutoLink`, and call `begin()` once in `setup()` after `comm.begin()`. If WiFi connects, a mobile-friendly page is served — by default on port **8765** (pass a third argument to `begin()` to change it). If WiFi fails, `isUp()` returns `false` and the UART link is completely unaffected.

The `UtilPing` / `UtilPong` helpers wire this up for you automatically when you pass WiFi credentials to their constructor; the snippet below is the manual equivalent for a custom sketch.

```cpp
#include <AutoLink.h>
#include <AutoLinkWeb.h>
using namespace autolink;

AutoLink    comm(UART_NUM_2, 16, 17, true);
AutoLinkWeb mon(comm);

void setup() {
    comm.begin();
    mon.begin("YourSSID", "password");       // default port 8765
    // mon.begin("YourSSID", "password", 80); // custom port
}
// Nothing changes in loop() — no handleClient() or polling needed.
```

## Dashboard

The dashboard updates once per second and shows:

| Widget | What it shows |
|---|---|
| **State pill** (header) | Green `OK` / amber `LCK` / red `SWP` — animates on transition |
| **TX Rate** | Bytes/s + cumulative total since power-on |
| **RX Rate** | Bytes/s + cumulative total since power-on |
| **Errors (lifetime)** | Cumulative frame-error count + lifetime disconnect count |
| **WiFi RSSI** | Signal strength in dBm + free heap + current baud |
| **Live Log** | Scrolling `[E]`/`[I]`/`[D]` log panel, color-coded by severity |

## Errors vs. Disconnects

These two numbers measure different things:

- **Errors (lifetime)** is the cumulative count of every frame-level error — bad CRC, malformed COBS, buffer overflow — since the last reset. On a clean link this stays at 0. It is *not* the internal rolling counter (which resets to 0 after every good frame and so almost never appears to move); it is a true running total.
- **Disconnects** counts how many times the link dropped from `OK` back into a re-sweep (`OK → SWP`). One bumped wire or one noise burst that trips the error threshold equals one disconnect.

A link can accumulate errors without disconnecting (occasional noise that stays under the threshold), and the two counters are independent.

## Controls

- **Pause / Resume** stops the 1 Hz polling without closing the page.
- **Clear** wipes the log panel while keeping the sequence counter, so stale entries are never re-fetched.
- **Copy** copies the entire visible log to the clipboard.
- **↺ Reset** calls `resetStats()` + `resetErrors()` on the device, zeroing throughput, error, and disconnect counters.
- **⏏ Reboot** restarts the device after a confirmation prompt. The page then polls until the device answers again and reloads automatically.

## Endpoints

Open, no authentication:

| Endpoint | Description |
|---|---|
| `GET /` | Mobile HTML dashboard |
| `GET /stats` | JSON snapshot: state, B/s rates, totals, errors, RSSI, heap, uptime, baud |
| `GET /logs?since=N` | JSON log entries with seq ≥ N (used for incremental polling) |
| `POST /reset` | Calls `resetStats()` + `resetErrors()`; returns `"ok"` |
| `POST /reboot` | Restarts the device via `esp_restart()` after sending its reply |

> **Note on `resetStats()`:** `AutoLinkWeb` samples the cumulative counters on its own 1 Hz timer and never calls `resetStats()` on its own. If your sketch calls `resetStats()`, the B/s display will read 0 for one interval and then recover automatically.

## Reliability notes

The dashboard is designed to survive flaky WiFi. Each `fetch()` has a 2.5 s `AbortController` timeout, a `busy` flag prevents overlapping polls from stacking up when the device is slow, and every HTTP response sets `Connection: close` so stale keep-alive sockets cannot exhaust the server's socket pool after a reconnect.
