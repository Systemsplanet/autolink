# 📡 AutoLink Web Monitor

`AutoLinkWeb` adds a self-contained WiFi dashboard to any AutoLink sketch. Include `AutoLinkWeb.h`, construct a monitor object that wraps your `AutoLink`, and call `begin()` once in `setup()` after `comm.begin()`. If WiFi connects, a mobile-friendly page is served — by default on port **8765** (pass a third argument to `begin()` to change it). If WiFi fails, `isUp()` returns `false` and the UART link is completely unaffected.

The `PingPong` wrapper wire this up for you automatically when you pass WiFi credentials to their constructor; the snippet below is the manual equivalent for a custom sketch.

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
| **State pill** (header) | Green `OK` / red `SWP` — animates on transition |
| **TX Rate** | Bytes/s + cumulative total since power-on |
| **RX Rate** | Bytes/s + cumulative total since power-on |
| **Errors** | Big red number = lifetime disconnect count (OK→SWP transitions). Hint lines underneath show `N lost msgs` (wire loss) and `N frame errors` (bad CRC / malformed COBS / overflow). |
| **WiFi RSSI** | Signal strength in dBm + free heap + current baud |
| **Live Log** | Scrolling `[E]`/`[I]`/`[D]` log panel, color-coded by severity |

## Errors vs. Disconnects

The **Errors** card now leads with the big number: the lifetime disconnect count (cumulative `OK → SWP` transitions since the last reset). One bumped wire or one noise burst that trips the error threshold equals one disconnect. Two smaller hint lines underneath break out the related but distinct counters:

- **`N lost msgs`** — total messages physically lost on the wire. Derived from out-of-order sequence gaps; a single 4-seq burst loss is one disconnect with three lost messages.
- **`N frame errors`** — cumulative count of every frame-level error (bad CRC, malformed COBS, buffer overflow). On a clean link this stays at 0. It is *not* the internal rolling counter (which resets to 0 after every good frame and so almost never appears to move); it is a true running total.

A link can accumulate frame errors and lost messages without disconnecting (occasional noise that stays under the threshold), and the three counters are independent. Disconnects is the most important number — it's the headline — and frame errors is the diagnostic one sitting quietly below.

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
| `POST /ota/fw` | Firmware upload: streams the raw `.bin` body to the inactive OTA app slot, sets it as the boot partition, replies, then reboots. Errors (no OTA slot, oversized, invalid image) drain the body and return 4xx/5xx. |
| `POST /ota/gui` | Dashboard upload: a **STORED** zip (`zip -0 gui.zip index.html ...`) unpacked into LittleFS under `/web/`. Once `/web/index.html` exists, `GET /` serves it instead of the baked-in dashboard. Compressed or data-descriptor zip entries are rejected. |

### OTA partition requirements

Firmware OTA needs a partition table with **two app slots** (`ota_0`/`ota_1`); GUI OTA needs a **LittleFS data partition**. On Arduino-ESP32 pick a scheme like *"Minimal SPIFFS (Large APPS with OTA)"*, or supply a custom CSV with `ota_0`, `ota_1`, and a `spiffs`/`littlefs` data partition. Without a second app slot `POST /ota/fw` returns 500; without a mountable data partition `POST /ota/gui` returns 507 and `GET /` keeps serving the baked-in dashboard. After a firmware OTA, the next boot marks the new image valid (rollback-cancel) once the web monitor comes up.

Upload examples:

```
curl --data-binary @firmware.bin http://<ip>:8765/ota/fw
zip -0 gui.zip index.html && curl --data-binary @gui.zip http://<ip>:8765/ota/gui
```

> **Note on `resetStats()`:** `AutoLinkWeb` samples the cumulative counters on its own 1 Hz timer and never calls `resetStats()` on its own. If your sketch calls `resetStats()`, the B/s display will read 0 for one interval and then recover automatically.

## Reliability notes

The dashboard is designed to survive flaky WiFi. Each `fetch()` has a 2.5 s `AbortController` timeout, a `busy` flag prevents overlapping polls from stacking up when the device is slow, and every HTTP response sets `Connection: close` so stale keep-alive sockets cannot exhaust the server's socket pool after a reconnect.
