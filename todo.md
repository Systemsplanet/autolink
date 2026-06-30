# todo.md — v6.0.23

Release history: `docs/Version.md`.

## Open

1. **OTA firmware + GUI upload.** Slots `/ota/fw` and `/ota/gui` are reserved
   (r10/r11, `max_uri_handlers = 12`) and return 503 stubs as of 6.0.22.
   Buffer sizing depends on bench items 2 and 3.
   - `POST /ota/fw`: drain the request body via `httpd_req_recv` (the stub 503s
     without consuming it → half-read socket the httpd layer must time out),
     stream to the inactive slot via `esp_ota_*`, `esp_ota_set_boot_partition`,
     reboot; rollback via `esp_ota_mark_app_valid_cancel_rollback`.
   - `POST /ota/gui`: receive dashboard zip → LittleFS; serve from LittleFS when
     present, else `AutoLinkWebHtml.h`.
   - Partition table: dual app slots (`ota_0`/`ota_1`) + LittleFS data partition.
   - Host-pin zip parse + slot selection; `esp_ota_*` / LittleFS are
     cross-compile-only.

## Hardware bench (FireBeetle pair)

2. **ASYNC heap headroom.** Capture free-heap at ASYNC boot; confirm
   `uart_driver_install` + `xStreamBufferCreate` succeed with margin (rx floor
   10160 + streamBuf 10252 + WiFi/httpd). If tight, cap the ASYNC rx floor by
   available heap. Blocks OTA buffer sizing.

3. **ASYNC flood, frameErrs/disc = 0.** Flood (txDelay=0) at 115200 and at
   512000 over a short cable; `frameErrs` and `disc` must hold at 0, single loss
   recovers within reorderHoldMs. If 512000 spikes, make the rx floor / loopTask
   cadence baud-aware (couples to item 2). WireSim can't catch this.

4. **Sweep walk-down.** Degrade the link; sweep must fall 512000 → 115200 → …
   and lock on the first baud returning PONG_ACK, with no stall at the top entry.

## Verify
`cd test && make test && make itest && ./build/verify_build.sh`
(62/62 unit, 3/3 itest; cross-compile gates `AutoLinkWeb.cpp` /
`AutoLinkWebHandlers.cpp`.)
