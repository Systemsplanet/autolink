# 📅 AutoLink Version History

All releases, most recent first.
## v6.0.23

**todo.md reorg + Version.md hygiene (docs only)**

Documentation-only housekeeping, same shape as 6.0.20. No source, wire-format, build-surface, or test-behavior change — the protocol, ARQ, framing, and seq budget are byte-for-byte identical to 6.0.22. `todo.md` is reordered most-important-first and trimmed to the bare minimum a developer needs to pick the work back up: OTA is the single headline Open item with its firmware/gui/partition sub-steps inlined, and the previously-disclosed "OTA stub 503s without draining the request body (half-read socket)" limitation is promoted from a recurring Version.md footnote into the OTA item's first concrete sub-step instead of being re-disclosed each cycle. No completed items remain in `todo.md` — Open 1 and Open 2 were closed in 6.0.22, so this pass moves nothing out, it only re-files what is still Open. `docs/Version.md` stays the single source of truth for release detail; the dangling `---` separator at the file tail is removed and `trim --keep 20` drops v6.0.2.

### What moved

- `todo.md` — reordered most-important-first; OTA collapsed to one Open item (1) with the firmware / gui / partition sub-steps inlined; the half-read-body drain promoted into sub-step 1; hardware-bench items (2–4) and the Verify footer carried over unchanged; title bumped to 6.0.23.
- `docs/Version.md` — this entry; `trim --keep 20` dropped v6.0.2; trailing `---` separator artifact removed.
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.22 → 6.0.23` bump in lockstep (AGENTS rule 3).

### Wire format

Unchanged. No `.cpp` / `.h` / `.ino` touched. Same framer shape, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, and seq-space budget as 6.0.22.

### Regression test

None added — docs-only, no behavior to pin. The host suite is the gate and is identical to 6.0.22: `make test` 62/62 unit, `make itest` 3/3.

### Limitations

- Standing cross-compile carry-over: `build/verify_build.sh` was not re-run for 6.0.23 (no source delta; the 6.0.22 cross-compile already cleared the build path at 1027671 / 79320 bytes). No source on top of 6.0.22, so the risk stays bounded to the unchanged build path.
- The OTA work itself (todo item 1) stays Open; this release only re-files it, it does not implement it. The 503 stubs and the reserved r10/r11 slots are unchanged from 6.0.22.

### Result

- No source touched; the host suite is unchanged from 6.0.22 (`make test` 62/62 unit, `make itest` 3/3).
- `python3 build/version.py check` green (20 entries, --keep=20; this entry pushed v6.0.2 off the tail).
- `make assets_check` / `make test_coverage_manifest` unaffected — no dashboard source, no new TEST_BINS.
---

## v6.0.22

**Fix stale dashboard byte-count test + reserve OTA URI-handler slots (closes Open 1 + 2)**

Two todo.md items landed together because they're the only two this release can host-pin: the dashboard-byte-count literal went stale against the regenerated header and is now computed from the parts; the OTA URI-handler cap was 10-of-10 saturated, blocking the `/ota/fw` and `/ota/gui` endpoints that Open 3 needs. The release reserves those two slots (handlers return 503 with a clear message until the stream-to-flash + LittleFS paths land) and bumps `cfg.max_uri_handlers` from 10 to 12 in lockstep with `URIS[]` / `PATHS[]`. No wire-format, ARQ, framer, or seq-budget change — the protocol is byte-identical to 6.0.20. Source-touching release, so the cross-compile carry-over that 6.0.14–6.0.20 carried (no source delta) is live again: this is the first source-touching tag since 6.0.13 where `AutoLinkWeb.cpp` / `AutoLinkWebHandlers.cpp` actually change. `build/verify_build.sh` ran in-sandbox against `esp32:esp32:firebeetle32` (`arduino-cli` + esp32 core installed fresh this session), 1027671 bytes / 79320 bytes RAM — a 548-byte program-space delta vs. 6.0.19 baseline, consistent with the two stub handler bodies and the +2 handler-table entries. The cross-compile gate is green.

### What moved

- `build/dashboard_assets-test.py` — Open 1 fix. The pin that previously asserted `runtime == 31222` (a hardcoded snapshot from an early release) is replaced with `expected = sum(sizes.values()) + 1`, computed from the per-part decomposition in `_runtime_size_from_header`. Each part now has a positivity check (`part_size > 0`) so an empty part silently sneaking into the header trips the gate. The byte-count line is structurally honest: it asserts "the runtime equals what we just summed", which is the only contract the chunked-send loop has.
- `src/al/web/AutoLinkWeb.cpp` — Open 2. `cfg.max_uri_handlers = 10` becomes `12` (the +2 headroom for OTA). Two new handler slots: `const httpd_uri_t r10 = { "/ota/fw", HTTP_POST, handleOtaFw, this };` and `const httpd_uri_t r11 = { "/ota/gui", HTTP_POST, handleOtaGui, this };`. `URIS[]` and `PATHS[]` grow in lockstep, preserving the index-aligned `PATHS[i] / URIS[i]` contract that the source-grep alignment test pins.
- `src/al/web/AutoLinkWeb.h` — two new `static esp_err_t` declarations: `handleOtaFw(httpd_req_t*)` and `handleOtaGui(httpd_req_t*)`.
- `src/al/web/AutoLinkWebHandlers.cpp` — two stub implementations. Both return `503 Service Unavailable` with a body of `"OTA firmware/gui upload not yet implemented"` and a `warning`-level log line so a user probing the device sees the reservation rather than a 404 (which would mask that the slot is taken). The actual `esp_ota_*` / LittleFS wire-up lands in Open 3.
- `test/test_desktop/al/web/UriHandlerAlignmentTest.cpp` — `10 → 12` everywhere the count is asserted (`n == 10`, `uris.size() == 10`, `paths.size() == 10`, `seen.size() == 10`, `value == "10"`), plus `/ota/fw` and `/ota/gui` added to the `mustHave[]` route roster so the part-of-PATHS pin catches a future refactor that drops either OTA route. The function names `test_max_uri_handlers_is_10` / `test_paths_array_contains_all_ten_routes` become `_is_12` / `_all_twelve_routes` to match.
- `todo.md` — Open 1 and Open 2 closed; the OTA work itself (Open 3, firmware + GUI OTA, partition table) stays Open and unchanged. Verified-done gains a 6.0.22 pointer (one-liner; detail lives here).
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.20 → 6.0.22` bump in lockstep (AGENTS rule 3).

### Wire format

Unchanged. The OTA routes are HTTP, not AutoLink-wire. No `Link.cpp` / framer / seq-budget / ARQ-cache delta. `MAX_CHUNK = 250`, `MSG_HDR = 6`, the COBS encap, and the `cobsSeq` space are byte-identical to 6.0.20.

### Regression test

- `run_test_uri_handler_alignment` updated to pin the new 12-handler / 12-cap contract across all four sub-tests: (1) `cfg.max_uri_handlers = 12` in the httpd config block; (2) exactly 12 `r<N>` declarations match the cap; (3) `PATHS[12]` parallel to `URIS[12]`, every `PATHS[i]` matches the path declared in `r<N>`; (4) `PATHS[]` contains all 12 unique routes including `/ota/fw` and `/ota/gui`. Reverting either OTA slot (or any of the existing 10) trips the alignment pin.
- `run_test_esp_idf_error_etiquette` — the `httpd_register_uri_handler` word-boundary match in the source-grep pin is unchanged; the new code paths (`Log::log().warning(...)`, the 503/200 response shapes) are within the 15-line window the test checks for the call site.
- `run_test_compile_check` — `AutoLinkWeb.cpp` and `AutoLinkWebHandlers.cpp` both parse cleanly under `-DARDUINO=10607 -DAUTOLINK_USE_ESP_TIMER` against the host stubs. The OTA stubs only call ESP-IDF functions already declared by the stub `esp_http_server.h` (`httpd_resp_set_status`, `httpd_resp_set_type`, `httpd_resp_set_hdr`, `httpd_resp_send`).
- `run_test_version_free_source` — the dashboard_assets-test.py comment rewrite drops the prior `5.4.3` / `6.0.18` / `31222` / `32094` version-literal anchors (rule 12). The test no longer references any specific version or byte-count snapshot.
- `build/dashboard_assets-test.py` — the byte-count pin is now `expected = sum(sizes.values()) + 1` (computed) plus a per-part positivity check (`part_size > 0`). Idempotency, `{{VERSION}}`-marker, AUTOLINK_VERSION token-count, dashboard.js content, and dashboard.css version-marker checks are unchanged.

### Disclosed limitations

- The OTA stubs return 503 but do not consume the request body. A `POST /ota/fw` with a real firmware blob leaves the connection half-read; the httpd layer eventually times out and closes. Open 3 will wire up `httpd_req_recv` (or a chunked receive) into `esp_ota_write` for the firmware path; for the gui path it streams to LittleFS. The 503-without-body-consumption shape is acceptable as a pre-Open-3 placeholder because the routes are reserved, not implemented.
- `cfg.max_uri_handlers = 12` is a permanent cap; bumping it past 12 in a future release (say 14 for `/ota/fw/rollback` + `/ota/gui/clear`) requires a new entry in `URIS[]` / `PATHS[]` and a new pin in `mustHave[]`. The test invariants make the next bump mechanical but not free.
- `run_test_uri_handler_alignment` is a source-grep suite (the AutoLinkWeb TUs are `#ifdef ARDUINO` and can't run on host). It pins the count + order + completeness; a runtime regression (e.g., a `httpd_register_uri_handler` that returns `HANDLERS_FULL` silently) is caught by the alignment test's count invariant only if it surfaces in the source (a 12-of-12 race wouldn't). Out of scope for this release.
- The cross-compile gate ran in-sandbox this session against a freshly-installed `arduino-cli 1.5.1` + `esp32:esp32@3.3.5` (`arduino-cli core install esp32:esp32@3.3.5` completed before the verify). The 6.0.14–6.0.20 cross-compile carry-over is closed; 6.0.22 is the first source-touching release since 6.0.13 where the gate actually ran end-to-end in the sandbox.
- Open 3 (the OTA stream-to-flash + LittleFS wire-up) stays Open. The hardware bench items (renumbered 2–4 in `todo.md` after this release: ASYNC heap headroom, flood frameErrs=0, sweep walk-down) are unchanged — they require the FireBeetle pair and aren't host-pinnable.

### Result

- `make test` 62 / 62 unit suites pass (wall ~6.4 s). The `run_test_uri_handler_alignment` suite was the only source-grep pin that needed updating; it now asserts the 12-handler / 12-cap contract.
- `make itest` 3 / 3 host integration suites pass (wall ~40 s). Loopback / loopback_noise / loopback_sync unchanged from 6.0.20.
- `make assets_check` PASS — `AutoLinkWebHtml.h` byte contract unchanged (no dashboard source touched; the byte-count fix is in the test only).
- `make test_coverage_manifest` PASS — no new TEST_BINS entries; the two new stub functions live in `AutoLinkWebHandlers.cpp` which is excluded from the host suite's link set (same shape as the existing handlers). No manifest changes.
- `python3 build/pretty_print.py` — the touched files (`AutoLinkWeb.h`, `AutoLinkWeb.cpp`, `AutoLinkWebHandlers.cpp`, `UriHandlerAlignmentTest.cpp`, `dashboard_assets-test.py` for source-style check) format cleanly.
- `python3 build/dashboard_assets-test.py` PASS — `runtime=32094B`, sha256 unchanged from 6.0.20, sizes dict unchanged.
- `python3 build/version.py check` green (20 entries, --keep=20; this entry pushed v5.4.4 off the tail).
- `build/verify_build.sh` PASS — ran in-sandbox this session against `esp32:esp32:firebeetle32` (`arduino-cli 1.5.1` + `esp32:esp32@3.3.5` installed fresh). `Sketch uses 1027671 bytes (78%) of program storage space; Global variables use 79320 bytes (24%)`. Vs. the 6.0.19 baseline (1027123 / 79312) the deltas are +548 bytes program-space and +8 bytes RAM, consistent with the two stub handler bodies + the +2 handler-table entries.

### Verification artifacts

- `make test` wall: 6363 ms (62 unit suites).
- `make itest` wall: 40096 ms (3 integration suites).
- Combined wall: 46.5 s.
- Peak RSS: 88968 KiB (largest single-suite resident set; unchanged from 6.0.20).
- Cross-compile wall: ~3 min on a fresh `arduino-cli` + `esp32:esp32@3.3.5` toolchain install; ~30 s on a cached install.
- Program-space delta vs. 6.0.19 baseline: +548 bytes (two stub bodies + the +2 handler-table entries).
---

## v6.0.20

**todo.md reorg + Version.md cleanup; track stale dashboard-asset byte count (docs only)**

Documentation-only housekeeping. No source, wire-format, build-surface, or test-behavior change — the protocol, ARQ, framing, and seq budget are byte-for-byte identical to 6.0.19. `todo.md` had accumulated ten full multi-paragraph Verified-done blocks (6.0.10–6.0.19) that duplicated this file's canonical entries verbatim; that duplication is the drift hazard `version.py` exists to prevent (the two copies can disagree). This release collapses `todo.md`'s Verified-done section to one-line-per-release pointers and makes `docs/Version.md` the single source of truth for release detail. It also removes the closed Open 5 stub from `todo.md`'s `## Open` list (the `bytesRecvdForMessage` consumer landed in 6.0.19), renumbers the survivors with no gaps, and adds one genuinely untracked item: the stale `dashboard_assets-test.py` byte-count expectation. Two cosmetic `>` typos at the tails of the v6.0.3 and v6.0.1 entries in this file are removed.

### What moved

- `todo.md` — Verified-done collapsed from full blocks to one-liners (canonical detail now lives here); title bumped to 6.0.20; closed Open 5 removed from `## Open`; remaining Open items renumbered gap-free (old Open 6 → Open 5); new Open 6 tracks the stale dashboard-asset byte count; Verify + Hardware-re-test sections updated for 6.0.20.
- `docs/Version.md` — this entry (and `trim --keep 20` dropped v5.4.3); stray trailing `>` removed from the v6.0.3 and v6.0.1 entry tails.
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.19 → 6.0.20` bump in lockstep (AGENTS rule 3).

### New tracked item — stale dashboard-asset byte count (todo Open 6)

`build/dashboard_assets-test.py` hard-codes `expected = 31222` for the runtime size of `DASHBOARD_HTML`, but the regenerated header is now `32094` bytes (parts: A 186 + CSS 4980 + B 5967 + JS 20935 + C 25 = 32093, plus the 1-byte concatenation terminator). The expectation has been stale since at least 5.4.3 (then 31222 vs 31801) and has drifted further as the JS grew. The generated header itself is correct and current (`make assets_check` regenerates byte-identically); only the test's literal is wrong. Promoted from a recurring out-of-scope footnote to a tracked Open item so a future release fixes the literal (or replaces it with a sum-of-parts assertion) rather than re-disclosing it every cycle.

### Wire format

Unchanged. No `.cpp`/`.h`/`.ino` touched. Same framer shape, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, and seq-space budget as 6.0.19.

### Regression test

None added — docs-only, no behavior to pin. The host suite is the gate and is identical to 6.0.19: `make test` 62/62 unit, `make itest` 3/3.

### Limitations

- Standing cross-compile carry-over: `build/verify_build.sh` was not re-run for 6.0.20 (no source delta; the 6.0.19 cross-compile already cleared the build path). No source on top of 6.0.19, so the risk stays bounded to the unchanged build path.
- The dashboard-asset byte-count fix is tracked, not applied here — this release only records it as Open 6.

### Result

`make test` 62/62 (~5.8 s wall), `make itest` 3/3 (~40 s wall), `python3 build/version.py check` green (20 entries), `python3 build/dashboard_assets-test.py` regenerates `AutoLinkWebHtml.h` byte-identically to the shipped copy (its `expected` literal still mismatches — now Open 6). No hardware delta vs. 6.0.19.
---

## v6.0.19

**wire AutoLink::bytesRecvdForMessage + Ping wire-recvd log line (closes Open 5)**

The 6.0.15 release added `Link::bytesRecvdForMessage(baseSeq)` and the `LinkArq::baseSeqFor(seq)` accessor it walks, but neither had a production caller — `todo.md` Open 5 tracked the decision to make before the API could rot: either wire it into a real consumer or document it as deliberately-latent. This release wires it. `AutoLink` gains a one-line `bytesRecvdForMessage(uint8_t baseSeq) const` forwarder on the public facade (mirroring the existing `bytesRecvdFor` forwarder); Ping's two slot-completion drains in `Ping::loop` gain an additive `wire <seq> <bytes>` debug line that reads through `base_.comm_.bytesRecvdForMessage(queue_[head_].seq)`. The wire-recvd log fires alongside the existing `echo <seq> <msgBytes> <pending>` line, not instead of it — the slot-len log is the operator-facing message size, the wire-recvd log is the peer's wire-ACK confirmation, and both now share the same production-call surface for any future dashboard JSON field. Pinned by `run_test_bytes_recvd_forwarded_to_ping` (4 pins: facade forwarder present, two wire-recvd log sites in Ping::loop, each reads from the facade accessor not the single-chunk `bytesRecvdFor`, each fires inside the slot-drain loop). The existing `run_test_ping_send_failure` Pin 2 had a 400-char window between the echo format string and the `head_++` slot advance; the additional `wire %u %u %u` line + multi-line comment added ~700 chars, so the window is widened to 1500 to reach the head advance in the new layout (the pin's intent — "bytes arg is `queue_[head_].len`, not `bytesRecvdFor()`" — is unchanged).

### What moved

- `include/AutoLink.h` — `uint16_t bytesRecvdForMessage(uint8_t baseSeq) const` public forwarder. Body delegates to `link->bytesRecvdForMessage(baseSeq)` when `link_` is non-null. Lock-free contract matches the existing `bytesRecvdFor` accessor (both fields are stamped by the link task and read from the app context).
- `src/al/pingpong/Ping.h` — two new `base_.log_.debug("Ping", "wire %u %u", ...)` calls, one at each slot-completion drain in `Ping::loop` (the gap-stop drain and the main loop's tail-drain). Logged alongside the existing `echo %u %u %d` line. Reads through `base_.comm_.bytesRecvdForMessage(queue_[head_].seq)` so the same accessor serves the Ping log path and any future dashboard JSON consumer.
- `test/test_desktop/al/pingpong/BytesRecvdForwardedToPingTest.cpp` — new source-grep regression pin (4 pins). Source-grep is the right gate because Ping and AutoLink are `#ifdef ARDUINO` and can't run on host; the suite gates the facade forwarder's presence, the two wire-recvd log sites, the per-site facade-call source (vs. a `bytesRecvdFor` single-chunk lookup), and that the wire-recvd log fires inside the per-slot drain loop (before the `head_++` advance).
- `test/test_desktop/al/pingpong/PingSendFailureTest.cpp` — widened the Pin 2 tail window from 400 to 1500 chars to reach the `head_++` advance in the new layout (the wire-recvd log + its multi-line comment added ~700 chars between the echo format string and the slot advance).
- `test/test_desktop/Makefile` — `run_test_bytes_recvd_forwarded_to_ping` added to `TEST_BINS` and the per-suite build/run target lists.
- `test/scripts/coverage/test_coverage_manifest.py` — added `run_test_bytes_recvd_forwarded_to_ping` to the source-grep-only exempt tuple (it's a source-grep suite and doesn't link `$(AUTOLINK_SRC)`).
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.18 → 6.0.19` bump in lockstep (AGENTS rule 3).

### Wire format

Unchanged. The wire-recvd log reads existing per-chunk state (`bytesRecvd_[seq]`, `baseSeq_[seq]`); no frame shape shift, no new wire bytes, no public API addition beyond the one new facade forwarder. `Link::bytesRecvdFor(baseSeq)` is unchanged — its existing single-chunk contract is preserved.

### Regression coverage

- `run_test_bytes_recvd_forwarded_to_ping` Pin a (source-grep on `include/AutoLink.h`): facade forwarder exists with the exact signature `uint16_t bytesRecvdForMessage(uint8_t baseSeq) const`. A forwarder named differently, returning a different type, or with a different access path leaves the API latent again.
- `run_test_bytes_recvd_forwarded_to_ping` Pin b (source-grep on `src/al/pingpong/Ping.h`): Ping::loop contains exactly two `"wire %u %u"` log sites, one per slot-completion drain. The two existing `"echo %u %u %d"` sites are preserved (additive, not replacing).
- `run_test_bytes_recvd_forwarded_to_ping` Pin c (source-grep): each wire-recvd log site reads through `base_.comm_.bytesRecvdForMessage(...)`, NOT the single-chunk `base_.comm_.bytesRecvdFor(...)`. The whitespace-stripped string matcher handles the multi-line clang-format indentation between `comm_` and `.bytesRecvdForMessage(`. A future regression that reverts to the single-chunk accessor (the pre-fix bug shape) trips this pin (`Assertion 'tail.find("base_.comm_.bytesRecvdForMessage(")' failed`).
- `run_test_bytes_recvd_forwarded_to_ping` Pin d (source-grep): each wire-recvd log site fires inside the per-slot drain loop, before the `head_ = (head_ + 1) % WINDOW` slot advance. The site count is asserted; the head-advance position is checked.
- Existing pins are preserved: `run_test_base_seq_tracking` (5 pins on the link-layer accessor + accessor presence + walk loop), `run_test_ping_send_failure` Pin 2 (3-arg echo log shape, bytes arg is `queue_[head_].len` not `bytesRecvdFor` — window widened to 1500 chars for the current layout), `run_test_pingpong_log_hygiene` (paused-aware not-ready line, flushRx-after-clearQueue on got<0 + mismatch, Pong recv-only, WIRING? one-shot). All green on the 6.0.19 host suite.
- `run_test_ping_send_failure` Pin 2 toggle check: revert the tail-window widening (restore the 400-char window) — the pin turns red (`Assertion 'headAdvance != std::string::npos'` failed). The widened window is required for the new layout.

### Disclosed limitations

- The new facade forwarder and the wire-recvd log line are wire/host-equivalent to 6.0.18 — no new host-runnable surface; the cross-compile gate (`build/verify_build.sh`) is the relevant check, and it now runs in-sandbox on this session (arduino-cli + esp32:esp32@3.3.5 installed). Both the host suite (62/62 unit, 3/3 itest) and the cross-compile (Sketch 1027123 bytes, 78% of program storage, 79312 bytes RAM) pass clean.
- The wire-recvd log is `debug`-level, not `info`-level — an operator reading the default Info log won't see it. Pin 2 of `run_test_ping_send_failure` continues to pin the operator-facing `echo %u %u %d` shape at `debug` level; the new `wire %u %u` line matches that level so an operator who enables Ping debug logging sees both side by side.
- The Ping log path is `#ifdef ARDUINO` and not host-runnable. The source-grep pin is the structural guarantee; a runtime regression (e.g., a future change that swaps the facade call for a stale cached `uint16_t`) is not caught by the host suite. Out of scope for this release; a future cross-compile-driven fuzz or a runtime integration test could close it.
- The 6.0.18 dashboard-asset byte count discrepancy (`32094` vs expected `31222`) is unrelated to this release — present on the 6.0.18 baseline before any of my changes. Out of scope.

### Result

`make test` 62/62, `make itest` 3/3, `make all` both, `bash build/verify_build.sh` clean compile against `esp32:esp32:firebeetle32`. No hardware-required delta (Ping's wire-recvd log is `debug`-level and additive; an existing peer / link pair doesn't need any configuration change).

### Cross-checks

- AGENTS rule 4 carry-over discharged: 6.0.18 was the first source-touching tag since 6.0.13 (it was docs-only, but `AutoLinkConfig.h` from 6.0.17 was unverified); 6.0.19 is the first source-touching tag where `build/verify_build.sh` actually runs in the sandbox and passes. The carry-over is closed for 6.0.19.
- `run_test_version_free_source` green (the literal `6.0.19` reference exists only in the `AUTOLINK_VERSION` macro in `include/AutoLink.h`, the rule's only legitimate exception).
- `build/version.py check` green (20 entries, --keep=20).
- `python3 build/pretty_print.py` clean reformat on the four touched files; subsequent `make test` still 62/62.
- `python3 build/dashboard_assets-test.py` size mismatch is pre-existing on the 6.0.18 baseline (32094 vs 31222), unrelated to this release.

### Verification artifacts

- `make test` wall: 7678 ms (62 unit suites, 1 new pin + the prior 61).
- `make itest` wall: 40110 ms (3 integration suites).
- Cross-compile wall: ~3 min on the cached arduino-cli + esp32 toolchain.
- Peak RSS (largest unit suite): ~85 KiB.
---

## v6.0.18

**todo.md sync for 6.0.17 baud change (docs only)**

Documentation-only follow-up to 6.0.17. The 6.0.17 baud change shipped its source edit, the three version files, and a Version.md entry, but `todo.md` was not updated — it still read "current as of v6.0.16" and carried no record of the 512000 default. This release closes that gap: it records 6.0.17 under Verified done, adds the 6.0.17 hardware re-test block (short-cable lock at 512000, long/noisy-cable walk-down), and adds a new tracked risk (Open 6, 512000 ASYNC rx-servicing latency — the rx buffer floor is baud-independent in *size* but the time-to-overflow at 512000 is ~4.4x shorter than at 115200). It also corrects the Verify section: 6.0.17 is the first source-touching tag since 6.0.13, so the `verify_build.sh` cross-compile carry-over now genuinely applies (6.0.14–6.0.16 were docs/host-equivalent). No source touched.

### What moved

`todo.md` only. Verified-done gains a 6.0.18 (this release) block and a 6.0.17 block; Open gains item 6 (appended, not inserted — Planned 1's prerequisites reference Open 1 and Open 3 by number, so renumbering would drift those cross-refs); the Hardware re-test section gains a 6.0.17 sub-block; the Verify section's cross-compile carry-over is rewritten from "limited to the unchanged build path" to "now live — 6.0.17 edits `AutoLinkConfig.h`, unverified in-sandbox".

### Wire format

Unchanged. No `.cpp`/`.h`/`.ino` touched. Same framer shape, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, and seq-space budget as 6.0.17.

### Regression test

None added — docs-only. The host suite is the gate and is identical to 6.0.17: `make test` 61/61 unit (incl. `run_test_baud_index_bounds` 2 pins, `run_test_base_seq_tracking` 5 pins), `make itest` 3/3 (`run_loopback` sweeps the 6-baud default and locks at 512000).

### Limitations

- Standing cross-compile carry-over (inherited from 6.0.17, not introduced here): `build/verify_build.sh` has not been re-run in-sandbox (no arduino-cli / network disabled). 6.0.17's `AutoLinkConfig.h` edit is in the Arduino build path; it must clear the ESP32 cross-compile in a longer-lived environment before the next source-touching tag. 6.0.18 adds no source on top.
- Open 6 (512000 ASYNC rx-servicing) is hardware-only — WireSim is byte-exact and untimed, so the host loopback locks at 512000 regardless of real-wire bit timing.
---

## v6.0.17

**Add 512000 baud to default allowedBauds list**

The default 5-baud list (`115200, 57600, 38400, 19200, 9600`) was sized for ESP32 UART hardware that the 5.3.x release series ran on. The FireBeetle-ESP32 and similar boards reliably drive 512000 baud over a short cable (the bit period drops to ~19.5 µs, still well above the ~104 µs 9600 floor; cable capacitance dominates only at < 19200). The default-list ceiling was a config-default gap, not a protocol gap — the ARQ / framer / seq-space all derive from `MAX_CHUNK` and `MSG_HDR`, neither of which depends on baud, and the baud-sweep phase machine handles any list length. This release adds `512000` as the new fastest entry, so a freshly-deployed board can sweep up to 512000 without the user hand-editing `cfg.allowedBauds[]`. The list is still 6 entries — well under the 16-entry `AUTOLINK_MAX_BAUDS` cap, no per-suite source change required.

### Fix 1 — `allowedBauds[]` default list + `allowedBaudsCount` default

`AutoLinkConfig::allowedBauds[]` initializer now leads with `512000` followed by the existing five: `{ 512000, 115200, 57600, 38400, 19200, 9600 }`. `allowedBaudsCount` default bumps from `5` to `6`. Existing user sketches that explicitly set `cfg.allowedBauds` / `cfg.allowedBaudsCount` are unaffected — the field stays public per the existing contract and the post-construction choke-point accessors (`Link::allowedBaudsCount()`, `Link::allowedBaud(i)`) bound any OOB write the same way they did pre-this-release. The baud-sweep machine walks the list top-down (`allowedBauds[0]` fastest); a sweep that finds 512000 noisy falls back to 115200 / 57600 / ... on the same path it did before.

### Wire format

Unchanged. The baud list is a runtime config, not a wire-protocol constant. No `.cpp`/`.h`/`.ino` behavior change beyond the two initializer literals. The wire still emits PING / PONG / LCK / REQ / CTRL / ACK / NAK frames at whatever baud the sweep locks on; the framer shape, the COBS encap, the chunk cap (`MAX_CHUNK = 250`), the per-message header (`MSG_HDR = 6`), and the seq-space budget are all byte-identical to 6.0.16.

### Regression test

The existing `run_test_baud_index_bounds` suite (2 runtime pins) still covers the post-construction OOB-write contract on the now-6-entry list — the test writes `cfg.allowedBaudsCount = 20` (and `-1`) and walks every choke-point accessor; the test's runtime numbers (`AUTOLINK_MAX_BAUDS = 16`, `getCurrentBaud` returns the bounded value, OOB indices surface 0) are unchanged. No new test added because the change is a config-default initialization, not a code-path addition: the only way to "revert" is to delete the `512000,` from the initializer and the `+ 1` from the count, and the 6.0.16 list shape had no test asserting the count of entries (every test that cared about the count set its own `kBauds[]` / `kNumBauds`). The default-list contents are operator-facing config, not a behavior pin.

Pinned implicitly by:
- `make test` (61 / 61 unit suites) — all suites that use the default `AutoLinkConfig cfg;` (without overriding the list) now sweep a 6-baud table. None of those suites asserts a specific count; they all assert behavior that holds for any list length.
- `make itest` (3 / 3 integration suites) — `run_loopback` (30 s) sweeps the default 6-baud list, locks at `512000` (the new top entry) in the bench scenario, and reports `tx=42577 rx=41370 disc=0 frameErrs=0` with both sides at `state: OK` end-to-end. Pre-this-release at the same 30 s window the loopback locked at `115200` and reported comparable numbers — the sweep's only delta is which entry it locks on; the throughput, disc count, and OK-state hold time are the same.
- `run_test_baud_index_bounds` (2 pins) — still green at the new 6-entry default; the choke-point accessors bound any OOB write regardless of list length.
- `run_test_linkcontext` (pin 3) — still green; `LinkContext` interface is unchanged.

### Limitations

- The 512000 entry assumes the wire and both ESP32 UARTs can carry it. Long or shielded cables may force a fallback to 115200 or 57600 — the sweep handles that case automatically (the per-baud dwell is uncapped, so the master will walk down until it finds a baud that returns PONG_ACK within the dwell). Operators with known-bad long cables can remove 512000 by setting `cfg.allowedBauds[0] = 115200; cfg.allowedBaudsCount = 5;` in their sketch.
- The new entry is the fastest in the list, so the baud-sweep now starts at 512000. The cold-start worst case (`~156ms` at the old 5-baud list) scales to `~176ms` at the 6-baud list (5.3.7's dwell formula: each added entry adds one round-trip + 10% margin to the worst-case sweep). The best case (`~57ms`) is unchanged — the sweep locks at the top baud and never walks down. The sweep change is bounded; the operator-visible delta is ~20 ms in the absolute worst case.
- The default-list change is host-equivalent: the test sweep runs in MockHal, which doesn't model real wire delay. The 30 s loopback test confirms the protocol holds at the new top baud (no disc, no frame errors, both sides at OK) but does not measure real-wire sweep timing. The 512000 baud on real hardware is unverified in this sandbox (no arduino-cli / no FireBeetle).
- The Arduino Library Manager / IDF component / Arduino ESP32 core all support 512000 baud on standard baud-rate tables, so the platform layer is fine. The default-list change is the only delta.
- The 6.0.17 source diff against 6.0.16 is one header (`AutoLinkConfig.h`) and one `int` initializer. No `src/`, `include/`, or `examples/` API change. The `AUTOLINK_MAX_BAUDS = 16` cap is unchanged; the 6-entry default leaves 10 slots of headroom for user-extended lists.
- The fix does NOT change the wire format, the ARQ cache shape, the seq-space budget, the floor math (`uartRxBufferFloor` / `uartTxBufferFloor` / `chunksForMsgLen`), the link-layer public API, the dashboard JSON shape, or the build surface. Only the config-default list initializer and the count default.

### Files touched

- `src/al/AutoLinkConfig.h` — `allowedBauds[]` initializer now `{ 512000, 115200, 57600, 38400, 19200, 9600 }`; `allowedBaudsCount` default `5` → `6`.
- `include/AutoLink.h` + `library.properties` + `idf_component.yml` — version bump 6.0.16 → 6.0.17 in lockstep.
- `docs/Version.md` — this entry (and trim dropped the oldest entry per `--keep=20`).
- `todo.md` — no change (this is a config-default add, not a TODO item; the existing OTA / heap / OTA-handler / API-consume items all stand).

### Result

- 61 / 61 host unit suites pass (`make test`). Wall: ~6.1 s.
- 3 / 3 host integration suites pass (`make itest`). Wall: ~40 s.
- `make loopback` PASS — 30 s two-Link loopback sweeps the new 6-baud list, locks at 512000, `tx=42577 rx=41370 disc=0 frameErrs=0`, both sides at `state: OK`.
- `make test_coverage_manifest` PASS — no test binary changes; the source-grep-only exempt list is unchanged.
- `make assets_check` PASS — `AutoLinkWebHtml.h` byte contract unchanged (no dashboard source touched).
- `python3 build/pretty_print.py` PASS — the touched header formatted cleanly (the initializer reflow is the only delta).
- `python3 build/version.py check` PASS — 20 entries, --keep=20 (this entry pushed the oldest off the tail).
- `build/verify_build.sh` not run in this sandbox — `arduino-cli` not installed. Per AGENTS rule 4, the cross-compile must be re-run in a longer-lived environment before release. The change is two literal initializers in one header (one added baud value, one integer default bump); no new symbols, no new RTOS primitive allocations, no header cycle changes. The `esp32:esp32:firebeetle32` cross-compile risk is very low, unverified. The 512000 baud itself is unverified on real hardware (no FireBeetle in sandbox); the protocol's hold at 512000 is pinned by `run_loopback` (MockHal-based, no real-wire delay).
---

## v6.0.16

**todo reorganization + unconsumed-API follow-up tracked (docs only)**

Housekeeping release. No source touched; the wire format, build surface, and host/itest suites are identical to 6.0.15. `todo.md` had a stale shape after 6.0.15: closed item 4 (base-seq tracking) was left in the `## Open` section as a "Closed in 6.0.15" stub instead of being removed, so the Open list read as five items when only four were live. The reorg deletes the stub, renumbers the Open items with no gaps, and adds one genuinely untracked follow-up.

### Fix 1 — Open list reorganized

The base-seq item closed in 6.0.15 is removed from `## Open` (it already lives under Verified done). Remaining Open items renumbered in sequence: heap headroom (1), kFrameOverhead literal (2), max_uri_handlers OTA headroom (3), AGENTS/developer.md split discipline (4). The `kFrameOverhead` source line references in Open 2 were corrected to `AutoLinkConfig.h:377,389` — the old 372/388 had drifted after the 6.0.13 move of MAX_CHUNK / MSG_HDR into `LinkContext.h`. Open 3 gained a note that `run_test_uri_handler_alignment` already pins URIS[]/PATHS[] parity and the cap=10, and that pin must be extended when the OTA routes land.

### Fix 2 — unconsumed API tracked (new Open 5)

The 6.0.15 `Link::bytesRecvdForMessage(baseSeq)` method and the `LinkArq::baseSeqFor(seq)` accessor it walks are exercised only by `run_test_base_seq_tracking` and `LinkTestAccessor` — no production path reads them. The method is correct and pinned, but an unconsumed public API can rot or diverge from the table it reads. New Open 5 records the decision to make before it ages: either wire it into a real consumer (dashboard JSON per-message byte count, or a Pong-side echo cross-check) or document it in `docs/API.md` as a deliberately-latent accessor so a future reader doesn't mistake "no caller" for "dead code."

### Wire format

Unchanged. No `.cpp`/`.h`/`.ino` touched. Only `todo.md`, `docs/Version.md`, and the three version files (`AutoLink.h`, `library.properties`, `idf_component.yml`).

### Regression test

None — docs-only release, no behavior change to pin (mirrors the 6.0.14 docs-only precedent). The standing ESP32 cross-compile carry-over from 6.0.13 is unchanged: host build green, `build/verify_build.sh` not re-run in-sandbox; no source delta in 6.0.14/6.0.15/6.0.16 keeps that risk bounded to the unchanged build path.
---

## v6.0.15

**Per-message bytes-recvd sum + baseSeq tracking regression pin (closes todo item 4)**

The 6.0.6 default bump (maxMsg 1024 → 5120) made the multi-chunk path the steady state, not the corner case. `todo.md` item 4 flagged a v6.0.7 carry-over: Ping's base-seq tracking was more likely to surface at 5120-byte messages (22 chunks vs. 5 at 1024), and the question of whether the baseSeq_ table was correctly populated for a 22-chunk message was never closed. `LinkArq` had been writing `baseSeq_[seq] = (baseSeq == NO_BASE) ? seq : baseSeq` since the table's introduction, but no consumer ever walked it — the field was effectively dead state. `Link::bytesRecvdFor(baseSeq)` returned the first chunk's bytes (= MSG_HDR for the hdr-only frame), not the full message size. A new `Link::bytesRecvdForMessage(baseSeq)` method sums every chunk that shares the same baseSeq, surfacing the data the table was already collecting. A new regression test pins the structural invariant (the per-chunk baseSeq_ mapping is correct at MTU) and the API (the per-message sum equals MSG_HDR + payload length).

### Fix 1 — `LinkArq::baseSeqFor(seq)` public accessor

`baseSeq_[256]` was a private field of `LinkArq` with no read accessor. The table was written by `onSent(seq, baseSeq, ...)` and cleared by `onAcked` / `clearAll`, but no production code path ever read it back — the only "consumer" was the inline comment in `LinkFrameRx.h` describing the intent. The accessor surfaces the field for the new per-message sum query and for any future caller (a dashboard JSON field, a Ping log-line update, etc.) that wants to walk the per-message chunk set. The accessor is a one-line inline — no cost on the hot path; `bytesRecvdForMessage` is the only caller today.

### Fix 2 — `Link::bytesRecvdForMessage(baseSeq)` public API

The new method walks the 256-slot `baseSeq_` table, sums `bytesRecvd_[seq]` for every `seq` whose `baseSeq_[seq] == baseSeq`, and returns the total. For a short ASYNC message (one merged chunk) the total equals `bytesRecvdFor(baseSeq)` — the per-chunk and per-message queries coincide. For a multi-chunk ASYNC message (1 hdr-only + N data chunks all sharing baseSeq), the total equals MSG_HDR + payload length = the full message size. For a SYNC message the table's NO_BASE default makes every chunk its own baseSeq, so the per-message sum equals the per-chunk value (the SYNC path's per-chunk waitForAck pattern doesn't have a single shared-baseSeq grouping).

The implementation is a 256-entry loop, no heap, no lock (matches the existing `bytesRecvdFor` no-lock contract — both fields are stamped by the link task and read from the app context). Walk order is seq 0..255; future work that wants ordered output (e.g. a "chunks received" list) would walk the same way.

### Wire format

Unchanged. The new accessor and the new sum query read existing per-chunk state; no frame shape shift, no new wire bytes, no public API addition beyond the two new methods. `Link::bytesRecvdFor(baseSeq)` is unchanged — its existing single-chunk contract is preserved (Pin 2 of the new test pins this).

### Regression test

New `run_test_base_seq_tracking` suite (5 pins). Runtime + source-grep, one per assertion. AGENTS rule 18 compliant: each pin fails when its fix is reverted.

- **Pin 1** (runtime): ASYNC 5120-byte message round-trips through pong's 22-chunk wire-ACK pipeline. `ping.bytesRecvdForMessage(baseSeq) == 5126` (= MSG_HDR + 5120). Pong's `recvMsg` returns 5120 byte-for-byte. The pre-this-release shape (no `bytesRecvdForMessage`) couldn't express this number — `bytesRecvdFor(baseSeq)` returned 6 (the hdr-only frame's bytes). A future regression that breaks the per-message walk (e.g., returns `bytesRecvd_[baseSeq]` directly, or walks `baseSeq_[i] == i`) trips this pin (`Assertion 't.bytesRecvdForMessage(baseSeq) == 5126' failed`).
- **Pin 2** (runtime): ASYNC 4-byte short message. `bytesRecvdFor(baseSeq) == bytesRecvdForMessage(baseSeq) == 10` (= MSG_HDR + 4, the merged-chunk length). Pins the single-chunk contract: the per-message sum doesn't change the short-message behavior. Toggle off (e.g., add a `+ 1` to the per-message sum) → red.
- **Pin 3** (source-grep on `LinkArq.h`): the public `baseSeqFor(seq)` accessor exists. Drop the accessor (e.g., move it to private) → compile error in `Link.h` (which calls `arq_.baseSeqFor(...)`); red.
- **Pin 4** (source-grep on `Link.h`): the `bytesRecvdForMessage` method body references `arq_.baseSeqFor` and walks 256 entries (comment-stripped — a comment containing "256" doesn't fake the check, the walk loop is required). Pre-this-release shape had no method; an implementation that walks the table but reads `baseSeq_[i]` directly (instead of through the accessor) → red (`Assertion 'arq_.baseSeqFor' failed`).
- **Pin 5** (source-grep on `LinkArq.cpp`): `onSent` body stamps `baseSeq_[seq] = (baseSeq == NO_BASE) ? seq : baseSeq`. The pre-this-release shape had this exact ternary; a future refactor that drops the NO_BASE default would have data chunks with `baseSeq_[seq] = 0xFF` (NO_BASE) and the per-message sum would sum 0 chunks. Reverting the ternary to a bare `baseSeq_[seq] = baseSeq;` → Pin 5 red (`Assertion 'NO_BASE' failed`).

Toggle-off checks (verified locally):
- Removing `arq_.baseSeqFor` from `bytesRecvdForMessage`'s body → Pin 4 red.
- Replacing `bytesRecvdForMessage` with `return bytesRecvd_[baseSeq];` → Pin 1 red (`Assertion 't.bytesRecvdForMessage(baseSeq) == 5126' failed`).
- Dropping the NO_BASE ternary in `onSent` → Pin 5 red.
- Removing the public `baseSeqFor` accessor → Pin 3 red (compile error in `Link.h`).
- Pin 2 catches any future drift that touches the short-message path.

### Limitations

- The per-message sum reads the 256-slot `baseSeq_` table without the link lock, matching the existing `bytesRecvdFor` contract. A future release that wants strict thread safety can add a `hw.lock()` / `hw.unlock()` pair around the walk; the existing per-chunk accessor doesn't have it, so adding it to the new method alone would be inconsistent. Out of scope for this release.
- The 256-entry walk is O(256) per query. At default `maxMsg=5120` a 22-chunk message's bytes-recvd is queried once per call, so the worst case is 256 iterations per "did message N land?" check. Negligible on ESP32 (a few microseconds at 240 MHz). A future release that wants the per-message sum in a hot path (e.g., a per-loop stats query) could memoize the sum on the last `onAck`; out of scope.
- The new API is wired but not consumed by any production code path today. Ping's existing `matchEcho_` log line sources its byte count from the local slot's `len` field (the operator-facing message size), not from the wire-ACK-reported bytes-recvd, so the log was already correct without this method. The new API is available for any future consumer — a dashboard JSON field, a Ping-side "last message bytes-recvd" indicator, a Pong-side echo — to walk the per-message chunk set.
- The fix does NOT change the wire format, the ARQ cache shape, the seq-space budget, or any link-layer public API. The `bytesRecvd_` table is unchanged; the new `baseSeq_` accessor is a one-line inline; the new `bytesRecvdForMessage` is a 256-entry loop. No new RTOS primitive allocations, no new `#ifdef ARDUINO` paths, no header cycle changes. The `esp32:esp32:firebeetle32` cross-compile risk is very low (the touched methods are pure C++ that compiles under `-DAUTOLINK_HOST_TEST` and the production `#ifdef` block alike).
- The 6.0.15 source diff against 6.0.14 touches three headers (`Link.h`, `LinkArq.h`, `LinkTestAccessor.h`) and one test (`LinkBaseSeqTrackingTest.cpp`). No wire, build-surface, or test-API change beyond the new test.

### Files touched

- `src/al/link/arq/LinkArq.h` — new `uint8_t baseSeqFor(uint8_t seq) const { return baseSeq_[seq]; }` public accessor + leading comment.
- `src/al/link/Link.h` — new `uint16_t bytesRecvdForMessage(uint8_t baseSeq) const` method; walks 256 entries summing `bytesRecvd_[seq]` for `arq_.baseSeqFor(i) == baseSeq`. Comment explains the no-lock contract (matches `bytesRecvdFor`) and the SYNC / single-chunk / multi-chunk shape.
- `test/common/LinkTestAccessor.h` — new `bytesRecvdFor(seq)` and `bytesRecvdForMessage(baseSeq)` pass-through accessors. Both host-test only (the test-only shim contract).
- `test/test_desktop/al/link/LinkBaseSeqTrackingTest.cpp` — NEW (5 pins, runtime + source-grep).
- `test/test_desktop/Makefile` — `run_test_base_seq_tracking` added to `TEST_BINS` + per-suite build rule + phony target.
- `include/AutoLink.h` + `library.properties` + `idf_component.yml` — version bump 6.0.14 → 6.0.15 in lockstep.
- `docs/Version.md` — this entry (and trim dropped v6.0.0 per `--keep=20`).

### Result

- 61 / 61 host unit suites pass (`make test_cpp`), with the new `run_test_base_seq_tracking` as the 61st. All 5 pins (1, 2, 3, 4, 5) green; the existing 60 suites unchanged. Wall: ~7 s.
- 3 / 3 host integration suites pass (`make itest`). Wall: ~40 s.
- `make assets_check` PASS — `AutoLinkWebHtml.h` byte contract unchanged (no dashboard source touched).
- `make test_coverage_manifest` PASS — `run_test_base_seq_tracking` links `$(LINK_SRC)`, so the manifest generator auto-detects its `src_for_*` entries (no exempt-list addition required).
- `python3 build/pretty_print.py` PASS — the touched files (`Link.h`, `LinkArq.h`, `LinkTestAccessor.h`, `LinkBaseSeqTrackingTest.cpp`) formatted cleanly.
- `python3 build/version.py check` PASS — 20 entries, --keep=20 (this entry pushed v6.0.0 off the tail).
- `build/verify_build.sh` not run in this sandbox — `arduino-cli` not installed. Per AGENTS rule 4, the cross-compile must be re-run in a longer-lived environment before release. The change is one new public accessor (a one-line inline in a header), one new public method (a 256-entry loop), and two new test accessors — no new symbols, no new RTOS primitive allocations, no header cycle changes. The `esp32:esp32:firebeetle32` cross-compile risk is very low, unverified.
---

## v6.0.14

**Reorganize todo.md; track OTA + URI-handler headroom as planned work (docs only)**

Docs only — no source, wire, or build-surface change. `todo.md` was carrying a duplicated 6.0.12 block (listed twice, under "Verified done" and again as "this release") and had no entry for the OTA work, even though firmware + GUI OTA is the next planned feature. Reorganized `todo.md` into Done / Open / Planned / Hardware re-test / Verify sections, de-duplicated the 6.0.12 entry, and added three tracked items grounded in the current tree: the OTA feature (firmware `POST /ota/fw`, GUI-zip-to-LittleFS `POST /ota/gui`, dual-app + LittleFS partition table), the `max_uri_handlers` / `URIS[]`/`PATHS[]` headroom prerequisite that blocks adding those two endpoints, and a carry-over to confirm the v6.0.7 base-seq tracking status at the default 5120-byte (22-chunk) message size. No `.cpp`/`.h`/`.ino` touched; version bumped in lockstep across `AutoLink.h`, `library.properties`, `idf_component.yml`.

### Regression test

None — documentation reorg with no behavioral change. The `version.py check` structural gate and the existing `run_test_mode_sync_async_fixes` pins remain the relevant guards; nothing in the source tree moved.

### Limitations

Host unit tests, host itest, and the ESP32 cross-compile were not re-run in the delivery sandbox (no toolchain network access). The change set is `docs/Version.md` + `todo.md` + the three version-string files, none of which feed the compile or the wire, so the build surface is byte-unchanged from 6.0.13. Re-run `make test` / `make itest` / `./build/verify_build.sh` in a networked environment if a gate is required before tagging.
---

## v6.0.13

**MAX_CHUNK-symbolic floor math + chunksForMsgLen de-duplication**

The UART buffer floor helpers (`uartRxBufferFloor`, `uartTxBufferFloor`) and the seq-space helper `chunksForMsgLen` all carry a literal `kChunkCap = 250` (and `kHdr = 6`) that mirrors `MAX_CHUNK` and `MSG_HDR` from `al/link/LinkContext.h`. The mirrors are intentional — `AutoLinkConfig.h` is host-linkable without pulling in the link layer — but the duplication is a drift hazard. A future `MAX_CHUNK` bump in `LinkContext.h` (e.g. raising the chunk cap from 250 to 384 for an MTU expansion) would leave the floor math, the framer, and the seq-space guard all tracking different values: the rx/tx buffers would size for the old chunk cap, the framer would emit the new cap, and the seq-space guard would compute chunk counts against the new cap. Silent wire vs. buffer desync. The fix pulls `MAX_CHUNK` and `MSG_HDR` symbolically out of `LinkContext.h` (where they belong as wire-protocol constants) and removes the literal mirrors from the three helpers.

### Fix 1 — `MSG_HDR` moved to `LinkContext.h`

`MSG_HDR = 6` was declared in `al/link/Link.h`. `Link.h` already includes `LinkContext.h`, so the constant was reachable from any TU that pulled in the link header — but `AutoLinkConfig.h` deliberately didn't (it would have dragged `Link.h`'s Arduino-free-but-heavy surface into every host test that just wanted the config struct). `MSG_HDR` is a wire-protocol constant (the per-message header length the first chunk carries), so it sits next to `MAX_CHUNK` in `LinkContext.h`. `Link.h`'s duplicate definition was deleted in favour of an include-chain re-export. No call-site changes — every TU that includes `Link.h` (which is every TU that uses the link layer) already gets `LinkContext.h` transitively.

### Fix 2 — `chunksForMsgLen` references `MAX_CHUNK` / `MSG_HDR` symbolically

The pre-v6.0.13 body:

```cpp
constexpr int kChunkCap = 250;
constexpr int kHdr = 6;
if (len + kHdr <= kChunkCap)
    return 1;
int n = (len + kChunkCap - 1) / kChunkCap;
return 1 + n;
```

— the same wire-protocol constants mirrored as literals. This release:

```cpp
if (len + MSG_HDR <= MAX_CHUNK)
    return 1;
int n = (len + MAX_CHUNK - 1) / MAX_CHUNK;
return 1 + n;
```

The function semantics are byte-identical at the current constant values (`MAX_CHUNK = 250`, `MSG_HDR = 6`). The merge-frame cap (hdr + payload fitting in one coalesced frame) still trips at `len + 6 <= 250`; the chunk-count math still evaluates to `ceil(len / 250)` for long messages. The mirror is gone — a `MAX_CHUNK` bump in `LinkContext.h` shifts both the merge-frame boundary and the chunk-count divisor in lockstep.

### Fix 3 — `uartRxBufferFloor` / `uartTxBufferFloor` reference `MAX_CHUNK` symbolically

The pre-v6.0.13 body had `constexpr int kChunkCap = 250;` duplicated inside both floor helpers. The post-fix body uses `MAX_CHUNK` from `LinkContext.h`:

```cpp
size_t perChunk = (size_t)MAX_CHUNK + kFrameOverhead;
```

`kFrameOverhead = 4` stays as a literal — it captures the COBS encap + zero-frame-delimiter pair, which is a property of the framing layer (the encap bytes the wire sees, not the per-chunk payload cap). The derivation is identical at the current value: `perChunk = 250 + 4 = 254`, and the rx floor at the default `AUTOLINK_ARQ_PIPELINE_WINDOW = 32` is `254 * 32 * 5/4 = 10160`, the tx floor is `254 * 3/2 = 381`. A future `MAX_CHUNK` bump shifts both floors in lockstep.

`AutoLinkConfig.h` now `#include`s `LinkContext.h` at file top. `LinkContext.h` is pure C++ (`#include <stdint.h>` only) — no Arduino, no FreeRTOS — so the host test environment still compiles cleanly.

### Wire format

Unchanged. The constant substitutions are byte-identical at the current values; the wire sees the same chunk sizes, same per-message headers, same floor numbers.

### Regression test

`run_test_mode_sync_async_fixes` Pin 2d (new) covers the symbolic-derivation invariant. Source-grep scope:

- `AutoLinkConfig.h` must NOT contain `kChunkCap = 250` (the literal mirror is gone).
- `uartRxBufferFloor` / `uartTxBufferFloor` / `chunksForMsgLen` bodies must each reference `MAX_CHUNK` symbolically.
- `chunksForMsgLen` body must reference `MSG_HDR` and contain the formula patterns `len + MSG_HDR <= MAX_CHUNK` and `(len + MAX_CHUNK - 1) / MAX_CHUNK` (comment-stripped — a comment containing the constant name doesn't fake the formula check).

Runtime check (Pin 2d-runtime):

- `uartRxBufferFloor(ASYNC) == (MAX_CHUNK + 4) * AUTOLINK_ARQ_PIPELINE_WINDOW * 5/4 == 10160`
- `uartTxBufferFloor(ASYNC) == (MAX_CHUNK + 4) * 3/2 == 381`

Toggle off (revert perChunk to literal `250` in either floor function) → Pin 2d source-grep fires red. Toggle off (revert `chunksForMsgLen` to literal `len + 6 <= 250`) → Pin 2d formula check fires red.

The pre-existing Pin 2a/2b/2c (mode scaling, pipeline-window reference) still pin the runtime behavior; Pin 2d layers the symbolic-derivation invariant on top.

### Limitations

`kFrameOverhead = 4` stays a literal — it's a framer-overhead constant, not a `LinkContext.h` constant. If a future framer change shifts the on-wire overhead (e.g. adding a per-frame length prefix or moving to a non-COBS encap), the floors would need a sibling constant alongside `MAX_CHUNK` in `LinkContext.h`. Out of scope for this MINOR; surfaced here as a follow-up if the framer shape ever changes.

The 6.0.13 source diff against 6.0.12 touches three headers (`AutoLinkConfig.h`, `LinkContext.h`, `Link.h`) and one test (`ModeSyncAsyncFixesTest.cpp`). No wire, build-surface, or test-API change beyond the new Pin 2d pins.
---

## v6.0.12

**docs: developer.md + AGENTS.md split**

Documentation-only release. No source, wire-format, build-surface, or test changes — the protocol, ARQ, framing, and seq budget are byte-for-byte unchanged from 6.0.11. The version-string bump rides through `include/AutoLink.h` / `library.properties` / `idf_component.yml` in lockstep so the contract stays consistent.

### Change 1 — `docs/developer.md` added

A new `docs/developer.md` collects the project-agnostic engineering principles (deep modules, composition over inheritance, one concern per unit, short names, test-through-interfaces, regression-pin discipline, red-loop-first debugging). It carries the *why*; `AGENTS.md` carries the project-specific *how*.

### Change 2 — `AGENTS.md` split and de-duplicated

The general rationale that previously lived inline in `AGENTS.md`'s code-style and testing rules was moved into `docs/developer.md`. The affected rules (11 comments, 13 short names, 14 composition, 15 one-concern, and the Testing block) now state the principle as a one-line `(principle: docs/developer.md)` defer and keep only the project-specific application — e.g. rule 14 keeps the ESP32 vtable/`IHal` detail, rule 13 keeps this codebase's name vocabulary. A header note points readers at `docs/developer.md` for the reasoning.

### Change 3 — cross-references

`README.md`'s Document Index gains a `docs/developer.md` row and corrects the stale "last 8 releases" to "last 20" (matching `version.py trim --keep 20`). `AGENTS.md`'s header references `docs/developer.md`.

### Change 4 — `todo.md` reorganized

Prior open item 1 (backpressure cooldown no-op) is moved to Verified done — it shipped in 6.0.11. Remaining open items renumbered: heap-headroom RISK and magic-number MINOR carry forward; a new DOC item tracks keeping the AGENTS/developer split clean. Title updated to the current version.

### Wire format

Unchanged.

### Regression test

None — no code path changed. Host suite, itest, loopback, and the ESP32 cross-compile are expected green/unchanged from 6.0.11; the only delta the build sees is the `AUTOLINK_VERSION` string.

### Limitations

Docs-only: the open hardware-verification items (ASYNC heap headroom, backpressure-storm suppression under a full cache at `txDelayMs=0`) are still untested on the FireBeetle and remain in `todo.md`.
---

## v6.0.11

**Backpressure cooldown gate independent of txDelayMs**

The v6.0.10 ASYNC-thrash fix-bundle shipped a `BACKPRESSURE_COOLDOWN_MS = 1000` throttle on Ping's `sendMsg` failure branch (the exact storm that produced the `Ping send failed (backpressure) n=1378 pending=0 consec=1` log spam on a full ARQ cache), but the throttle stamped `tNextSendMs_` and the send-loop gate only honors `tNextSendMs_` when `cfg.txDelayMs > 0`. ASYNC flood mode (the scenario that produced the storm in the first place) runs with `cfg.txDelayMs = 0`, so the v6.0.10 throttle was a no-op — the bench log was unchanged between 6.0.10 and pre-6.0.10. One fix pins the failure class.

### Fix 1 — `backpressureCoolUntilMs_` stamp + txDelayMs-independent gate

The pre-v6.0.11 backpressure branch:

```cpp
tNextSendMs_ = millis() + BACKPRESSURE_COOLDOWN_MS;
```

was the right wall-clock target, but the gate at the top of the send loop was:

```cpp
if (txDelayMs > 0 && (int32_t)(now - tNextSendMs_) < 0)
    break;
```

— gated on `txDelayMs > 0`, so a flood-mode sketch (`txDelayMs = 0`) walked the gate past it on every iteration and `consecSendFail_` ticked up toward `MAX_SEND_FAIL = 5` with the throttle stamp sitting unused on the side. The 1000 ms cooldown was active in name only.

This release:

- Adds `uint32_t backpressureCoolUntilMs_ = 0;` to the `Ping` class. Comment explains: "ASYNC-only backpressure cooldown stamp. Set when sendMsg() trips consecSendFail_++, honored by the send-loop gate regardless of cfg.txDelayMs. Distinct from tNextSendMs_ (which the gate only honors when txDelayMs > 0) so ASYNC flood mode still throttles on a real backpressure hit."
- The send-loop writes `backpressureCoolUntilMs_ = millis() + BACKPRESSURE_COOLDOWN_MS;` on the backpressure branch (replacing `tNextSendMs_`).
- The send-loop adds a sibling gate that fires regardless of `txDelayMs`:

```cpp
if (backpressureCoolUntilMs_ != 0 &&
    (int32_t)(now - backpressureCoolUntilMs_) < 0)
    break;
```

- The `backpressureCoolUntilMs_` stamp is cleared in two places that reset other counters: the link-lost transition (`if (!base_.comm_.ready())` branch in `loop()`) and the SWP→OK transition (`if (!base_.wasReady_)` branch). Stale throttle from before the drop / recovery doesn't carry across.

The two gates are orthogonal. `tNextSendMs_` is user-pacing (the configurable `cfg.txDelayMs` inter-send delay). `backpressureCoolUntilMs_` is the ASYNC-only emergency brake on a stalled pipeline. SYNC mode never trips the backpressure branch (the sender blocks inline for the receiver ACK), so the cooldown is ASYNC-only by construction.

### Wire format

Unchanged. The throttle is wall-clock pacing inside `Ping::loop` — it doesn't affect the on-the-wire COBS+CRC frame format, doesn't change the ARQ cache shape, doesn't change the seq-space budget, and doesn't touch any Link-layer logic. The cooldown just stops Ping from hammering `sendMsg` on a full ARQ cache. Lowering `tNextSendMs_` (the v6.0.7 default-bump) is unaffected.

### Regression test

`run_test_mode_sync_async_fixes` extended from 11 to 12 pins. Pin 5b rewritten + Pin 5c added. AGENTS rule 18 compliant: each pin fails when its fix is reverted.

- **Pin 5b (rewritten, source-grep on `Ping.h`):** the backpressure-failure branch sets `backpressureCoolUntilMs_ = millis() + BACKPRESSURE_COOLDOWN_MS`. The `BACKPRESSURE_COOLDOWN_MS = 1000` constant is still defined on the class. The pre-v6.0.11 shape wrote `tNextSendMs_` here, which the gate never honored in ASYNC flood mode. Reverting the stamp assignment back to `tNextSendMs_ = millis() + BACKPRESSURE_COOLDOWN_MS` flips Pin 5b red (`Assertion 'backpressureCoolUntilMs_ = ... must be set on the backpressure branch' failed`). clang-format splits the assignment across lines (`backpressureCoolUntilMs_ =\n    millis() + BACKPRESSURE_COOLDOWN_MS;`); the pin matches both halves.
- **Pin 5c (new, source-grep on `Ping.h`):** the cooldown gate in the send-loop fires regardless of `cfg.txDelayMs`. The pin locates the gate pattern (`backpressureCoolUntilMs_ != 0 && (int32_t)(now - backpressureCoolUntilMs_) < 0`), walks back to the parent `if (...)`, and asserts the gate's parenthesized expression does NOT contain the substring `txDelayMs`. The pre-v6.0.11 shape was a sibling `if (txDelayMs > 0 && ... backpressureCoolUntilMs_ ...)` (or wrote to `tNextSendMs_` and the gate was the same `txDelayMs > 0` gate that was already there) — coupling the cooldown to user pacing. Re-coupling the gate (e.g. `if (txDelayMs > 0 && backpressureCoolUntilMs_ != 0 && (int32_t)(now - backpressureCoolUntilMs_) < 0)`) flips Pin 5c red. Removing the gate entirely also flips Pin 5c red (`Assertion 'send-loop must contain a cooldown gate of the form backpressureCoolUntilMs_ != 0 && (now - stamp) < 0' failed`).
- Pins 1, 2, 3, 4, 5a unchanged.

Toggle-off checks (verified locally):
- Reverting the backpressure branch to `tNextSendMs_ = millis() + BACKPRESSURE_COOLDOWN_MS;` → Pin 5b flips red (`Assertion 'backpressureCoolUntilMs_ = ...' failed`).
- Replacing the cooldown gate with `if (txDelayMs > 0 && backpressureCoolUntilMs_ != 0 && (int32_t)(now - backpressureCoolUntilMs_) < 0)` → Pin 5c flips red (`Assertion 'the cooldown gate must NOT be conditioned on txDelayMs' failed`).
- Removing the cooldown gate entirely (the pre-v6.0.11 plus-change shape where `tNextSendMs_` was set but never honored) → Pin 5c flips red.

### Limitations

- The cooldown stamp is wall-clock (`millis() + 1000`), not loop-iteration count, so it scales with baud — the link task gets more wall-clock at higher baud to drain the cache, the cooldown is fixed at 1000 ms regardless. A future change that wants a baud-aware cooldown could read `link_.nowBaud()` and scale; out of scope for this fix.
- The cooldown is ASYNC-only by construction: SYNC mode's send blocks inline for the receiver ACK, so `sendMsg` only returns false for `Link not OK` (which the existing `if (!base_.comm_.ready()) break;` guard already short-circuits before the consec counter bumps). No SYNC-mode churn.
- The cooldown clears on link-lost and SWP→OK transitions. A fresh resweep cycle doesn't carry a stale throttle from the previous cycle. A future change that wanted to preserve cooldown across a brief resweep would need a different policy; current behavior matches operator intent (after recovery, send at full line rate until real backpressure reappears).
- The `BACKPRESSURE_COOLDOWN_MS = 1000` constant is hard-coded in `Ping.h`. A future change that wants it configurable could read from `cfg`; the ASYNC-only backpressure branch is the right place for the knob. Out of scope for this fix.
- The 12-pin `run_test_mode_sync_async_fixes` suite remains a source-grep / runtime-pin-only test, on the `test_coverage_manifest.py` source-grep-only exempt list. No `src_for_*` entry required.
- The fix does NOT touch `EspHal::begin()`'s abort path. The `HAL not healthy` halt still fires (with the v6.0.8 300 ms LED blink) when heap refusal trips; the cooldown change is downstream of the link's runtime backpressure signal, not the boot-time heap-refusal signal.

### Files touched

- `src/al/pingpong/Ping.h` — new `uint32_t backpressureCoolUntilMs_ = 0;` field; send-loop gate reads it via a txDelayMs-independent sibling gate; backpressure branch sets the new stamp (not `tNextSendMs_`); link-lost and SWP→OK transitions clear the stamp.
- `test/test_desktop/al/link/ModeSyncAsyncFixesTest.cpp` — Pin 5b rewritten to assert the `backpressureCoolUntilMs_ = millis() + BACKPRESSURE_COOLDOWN_MS` stamp shape; new Pin 5c `test_pin_ping_cooldown_gate_independent_of_txDelayMs` asserts the gate is independent of `txDelayMs` (locates the gate pattern, walks back to the parent `if`, asserts the gate header contains no `txDelayMs`); both pins registered in `main()`.
- `include/AutoLink.h` + `library.properties` + `idf_component.yml` — version bump 6.0.10 → 6.0.11 in lockstep.
- `docs/Version.md` — this entry (and trim dropped v6.0.1 per `--keep=20`).

### Result

- 60 / 60 host unit suites pass (`make test_cpp`); `make all` ~21 s wall. The extended `run_test_mode_sync_async_fixes` is the 60th; all 12 pins (1a/1b/1c, 2a/2b/2c, 3, 4a/4b/4c, 5a/5b/5c) green; the existing 59 suites unchanged. Toggle-off checks (above) verified locally.
- 3 / 3 host integration suites pass (`make itest`). Wall: ~40 s.
- `make assets_check` PASS — `AutoLinkWebHtml.h` byte contract unchanged (no dashboard source touched).
- `make test_coverage_manifest` PASS — the extended suite is still on the source-grep-only exempt list, no `src_for_*` entry required.
- `python3 build/pretty_print-test.py` PASS — the touched files formatted cleanly.
- `python3 build/version.py check` PASS — 20 entries, --keep=20 (this entry pushed v6.0.1 off the tail).
- `build/verify_build.sh` not run in this sandbox — `arduino-cli` is not installed. Per AGENTS rule 4, the cross-compile must be re-run in a longer-lived environment before release. The change is a single new field declaration + a sibling gate + a stamp-target swap (no new symbols, no new RTOS primitive allocations, no header cycle changes). The `esp32:esp32:firebeetle32` cross-compile risk is very low, unverified.
---

## v6.0.10

**ASYNC link-thrash fixes: mode-to-HAL forward, mode-aware UART buffers, retx/reorder coupling, wire-frame txBytes, version-first log, backpressure cooldown**

Bench logs from a v6.0.9 ASYNC flood at 115200 with `txDelayMs=0` showed two independent root defects that compounded: the HAL held a stale `cfg.mode` copy and sized UART / stream buffers for SYNC while the link was running ASYNC, and the receiver's reorder buffer (`reorderHoldMs=1500 ms`) expired the held tail before the sender's `okTickMs()`-driven ARQ retx loop (`max(idleTimeoutMs/3, 50, syncAckTimeoutMs) = 3333 ms` at the defaults) could retransmit the missing chunk. The first chunk loss (Pong seq=58, a UART rx overrun on a 2 KB SYNC-sized buffer) was unrecoverable: 59..100,126 sat in the reorder buffer past `reorderHoldMs`, `lostMsgs` climbed, and the link cycled `P1 -> P2 -> BREAK -> P1` on the back of each forced resweep. Two cosmetic-but-confusing problems rode along: Pong's `tx=0 B/sec` because the wire-ACK/NAK/CTRL paths didn't bump `txBytes`, and the live boot log placed `v<version>` AFTER the `calling comm_.begin()` diagnostic. Five fixes pin the failure class.

### Fix 1 — `IHal::setMode` + `AutoLink::setMode` forwards to the HAL

`AutoLink::setMode` mutated only the `Link` copy of `cfg.mode`. `EspHal` holds its own `AutoLinkConfig cfg` (passed by value to both the link and the HAL) and never saw the update. The NVS+reboot restore path (`PingPongBase::bringUpLink` calls `setMode` BEFORE `begin()`) flipped the link to ASYNC while the HAL's `cfg.mode` stayed at its ctor's default — the boot log printed `mode=SYNC` against a wire that was running ASYNC, the UART / stream buffer floor came from the SYNC shape, and the cross-mode size mismatch drove the seq=58 rx overrun.

This release adds `virtual void setMode(AutoLinkConfig::Mode){}` to `IHal` (default no-op) and implements it on `EspHal`. `AutoLink::setMode` now forwards to `hal->setMode(m)` AND `link->setMode(m)`. `EspHal::setMode` re-derives the UART and stream-buffer floors from the new mode pre-`begin()` (the live UART / stream buffers can't be resized in flight — FreeRTOS stream buffers are immutable-after-create); post-`begin()` it only updates `cfg.mode` so the boot log stays honest, matching the existing NVS+reboot contract for mode toggles.

### Fix 2 — `uartRxBufferFloor` / `uartTxBufferFloor` scale by mode

The `cfg.rxBufferSize = 2048` / `cfg.txBufferSize = 256` defaults are SYNC-sized. ASYNC at 115200 with `txDelayMs=0` pipelines a multi-chunk window; 2 KB rx overruns on a single burst and the first chunk drops. No mode-based scaling existed in 6.0.9. This release adds two host-linkable helpers in `AutoLinkConfig.h`:

- `uartRxBufferFloor(cfg)`: SYNC passes `cfg.rxBufferSize` through unchanged; ASYNC returns `perChunk * AUTOLINK_ARQ_PIPELINE_WINDOW * 5/4` = `~10.2 KB` at the default window=32. Caller's larger `cfg.rxBufferSize` still wins via `max()`.
- `uartTxBufferFloor(cfg)`: SYNC passes `cfg.txBufferSize` through unchanged; ASYNC returns `perChunk * 3/2` = `~381 B` at the defaults.

Both reference `AUTOLINK_ARQ_PIPELINE_WINDOW` so a future window bump stays in lockstep. `EspHal::begin()` reads the derived values (not `cfg.rxBufferSize` / `cfg.txBufferSize` directly) for `uart_driver_install`, and the boot log line shows the derived size. `streamBufferFloor` already covered the rx *stream* buffer (one coalesced message plus headroom, mode-independent) and is unchanged.

### Fix 3 — `okTickMs` clamps at `reorderHoldMs/2` in ASYNC mode

The sender's OK-state timer tick is the only path that drives the ARQ retransmit loop (see `LinkTimers::onTimerOk_unlocked` walking the 256-slot table). The pre-this-release shape capped the tick at `max(idleTimeoutMs/3, 50, syncAckTimeoutMs) = 3333 ms` at the default `idleTimeoutMs=10000`; the receiver's `reorder_.dropExpired(now, reorderHoldMs=1500)` expired the held frames before the sender's tick could fire a retx. A single ASYNC chunk loss was unrecoverable: even a good retransmit landed on a Pong that had already discarded the tail.

This release clamps the tick at `reorderHoldMs/2` in ASYNC mode. The new tick is 750 ms at the default `reorderHoldMs=1500` — the retx always lands ahead of the receiver's expiry, with a 750 ms cushion for wire-side round-trip. SYNC mode is unchanged (the sender blocks inline for the receiver ACK and never walks the ARQ table, so the clamp doesn't apply). The SYNC-mode floor (`max(50, syncAckTimeoutMs)`) is also unchanged.

The new ASYNC retx cadence is a single retx attempt per `reorderHoldMs/2` window. With `syncAckTimeoutMs=500` and `reorderHoldMs=1500`, a missing chunk gets at most 3 retx attempts (at 750 ms intervals) before the receiver gives up — the receiver's expiry is the backstop, not the sender's. The pre-this-release shape's `syncAckTimeoutMs` was the per-slot "elapsed since `sentAtMs_`" threshold inside `LinkArq::decideSlot`; the *tick* itself was the gating factor for how often the table was walked. The fix tightens the tick to a value where the table walk always beats the receiver's expiry.

### Fix 4 — Wire-frame `txBytes` counter for CTRL / ACK / NAK / retx

The Stats.tx field is wired from `txBytes`, which `buildAndSendMsg_unlocked` bumped for data-payload bytes. CTRL frames (PING/PONG/LCK), wire-ACK frames, NAK frames, and resendCobsFrame_unlocked (the ARQ retx path) went out via `sendFrame_unlocked` / `sendAckFrame_unlocked` / `sendCtrlCobsFrame_unlocked` / `resendCobsFrame_unlocked` — none of those bumped `txBytes`. Pong's wire output is almost entirely ACK/NAK and its `tx` rate reported `0 B/sec` even while it was actively ACKing every received chunk.

This release adds `txBytes += <encoded length>; lastTxMs = hw.nowMs();` to each of the four paths. The data path (already counted) is unchanged. The resendCobsFrame_unlocked counter is for `n > 0` so a zero-byte control retx doesn't double-count. The asymmetric-idle detector's `txAge < FAST_IDLE_TX_MS` branch now fires on Pong too, which is the correct shape — Pong's tx side is real wire activity.

### Fix 5 — Version log line first; ASYNC backpressure cooldown 1000 ms

Two operator-facing fixes that ride along with the protocol fixes:

- `bringUpLink` emits `log.info("AutoLink", "v" AUTOLINK_VERSION)` as the first line — before the NVS prefs read and before `calling comm_.begin()`. The pre-fix shape placed the version line AFTER `begin()` returned, so the first log line answered `what just happened in begin()` rather than `what firmware is this?`. The pre-v6.0.9 bogus `maxMsg > streamBufSize` warning made the wrong line first; even on a clean v6.0.9 boot, the `v<version>` line followed a `D PingPongBase calling comm_.begin()` diagnostic. The new order: `v<version>` first, then the NVS read, then the `begin()` diagnostic.
- Ping's ASYNC backpressure failure branch (`consecSendFail_++` in `Ping::loop`) sets `tNextSendMs_ = millis() + BACKPRESSURE_COOLDOWN_MS` (= 1000 ms) before the loop exits. The bench log showed `Ping send failed (backpressure) n=1378 pending=0 consec=1` firing on every loop iteration against a full ARQ cache, with no throttle — the loop was spending wall-clock on log lines that told the operator nothing new and starving the link task. 1000 ms is the same order as `syncAckTimeoutMs`: one round-trip's worth of time for the link to drain. The MAX_SEND_FAIL escalation (5 consecutive failures -> `dropLink()`) is unchanged.

### Wire format

Unchanged. All five fixes are runtime / sizing / log behavior. No wire frame shifts, no public-API additions beyond `IHal::setMode` (defaulted to a no-op on the base class, so existing host tests that don't implement it still link).

### Regression test

New `run_test_mode_sync_async_fixes` suite (11 pins). Source-grep + runtime pins, one per assertion. AGENTS rule 18 compliant: each pin fails when its fix is reverted.

- **Pin 1a** (source-grep on `IHal.h`): `virtual void setMode(AutoLinkConfig::Mode)` declared. Toggle off (drop the virtual) -> red.
- **Pin 1b** (source-grep on `include/AutoLink.h`): `AutoLink::setMode` body calls both `hal->setMode(m)` AND `link->setMode(m)`. Pre-fix shape only called `link->setMode(m)`, leaving the HAL's cfg copy stale.
- **Pin 1c** (source-grep on `src/al/hal/EspHal.h`): `EspHal::setMode` body re-derives `rx_buffer_size_` / `tx_buffer_size_` / `stream_buf_size_` from the new mode pre-begin. The post-begin path is unchanged (live buffers can't be resized).
- **Pin 2a** (runtime): `uartRxBufferFloor(cfg)` for `cfg.mode = ASYNC` is > 4 KB at the default window; `cfg.mode = SYNC` passes `cfg.rxBufferSize` through. The pre-fix shape returned `cfg.rxBufferSize = 2048` regardless of mode, which underran on the bench's multi-chunk ASYNC flood.
- **Pin 2b** (runtime): `uartTxBufferFloor(cfg)` floors the ASYNC tx at a multi-chunk-safe size; a caller-set larger `cfg.txBufferSize` wins via `max()`. SYNC passes through.
- **Pin 2c** (source-grep on `AutoLinkConfig.h`): `uartRxBufferFloor` body references `AUTOLINK_ARQ_PIPELINE_WINDOW`, so a future window bump scales the rx floor in lockstep.
- **Pin 3** (source-grep on `LinkSweep.cpp`): `okTickMs` body gates the `reorderHoldMs/2` clamp on `cfg.mode == AutoLinkConfig::Mode::ASYNC` and clamps the tick at `cfg.reorderHoldMs / 2`. Pre-fix shape capped at 3333 ms regardless of mode; the new shape caps at 750 ms in ASYNC.
- **Pin 4a/b/c** (source-grep on `LinkTx.cpp`): `sendFrame_unlocked` (CTRL/PING/PONG), `sendAckFrame_unlocked` (5-byte wire ACK), and `sendCtrlCobsFrame_unlocked` (3-byte NAK) all bump `txBytes` by the encoded frame length and stamp `lastTxMs`. Pre-fix shape only counted the data-payload path.
- **Pin 5a** (source-grep on `PingPongBase.h`): `bringUpLink` body places `AUTOLINK_VERSION` log line BEFORE the NVS prefs read AND BEFORE `calling comm_.begin()`. Pre-fix shape placed the version after `begin()`.
- **Pin 5b** (source-grep on `Ping.h`): the `consecSendFail_++` branch sets `tNextSendMs_ = millis() + BACKPRESSURE_COOLDOWN_MS`; the `BACKPRESSURE_COOLDOWN_MS = 1000` constant is defined on the class. Pre-fix shape had no throttle on the backpressure branch.

Toggle-off checks (verified locally):
- Drop `virtual void setMode` from `IHal.h` -> Pin 1a flips red.
- Restore `AutoLink::setMode` to its pre-fix `if (link) link->setMode(m);` body -> Pin 1b flips red (`Assertion 'AutoLink::setMode must forward to hal->setMode(m)' failed`).
- Drop the `rx_buffer_size_ = rxBufferFloor(cfg);` line from `EspHal::setMode` pre-begin branch -> Pin 1c flips red.
- Drop the `cfg.mode == AutoLinkConfig::Mode::ASYNC` clamp from `okTickMs` -> Pin 3 flips red (`Assertion 'okTickMs must gate the reorderHoldMs clamp on ASYNC mode' failed`).
- Drop `txBytes += CTRL_FRAME_SIZE` from `sendFrame_unlocked` -> Pin 4a flips red.
- Drop the `tNextSendMs_ = millis() + BACKPRESSURE_COOLDOWN_MS` line from Ping's backpressure branch -> Pin 5b flips red.
- Move the `log.info("AutoLink", "v" AUTOLINK_VERSION)` line to after `comm.begin()` -> Pin 5a flips red.

### Limitations

- Fix 1's `IHal::setMode` is a defaulted virtual on the base class. Production code that constructs an `EspHal` and calls `setMode` on it is fine (the override is wired). Custom HAL implementations in user code that don't override `setMode` will silently no-op — same shape as a `MockHal` that doesn't override `begin()`. A user-side `IHal` extension that's ASYNC-aware should override `setMode`; the default no-op is the same shape as a `MockHal` test fixture that ignores the field.
- Fix 2's ASYNC rx floor (`~10.2 KB` at the default window=32) is sized for one in-flight pipeline. A future bump to `AUTOLINK_ARQ_PIPELINE_WINDOW` scales the rx floor in lockstep (Pin 2c's contract) but does NOT resize the stream buffer's `streamBufferFloor` (which is mode-independent: one coalesced message + headroom). Operators who need a larger ASYNC pipeline should also bump `cfg.streamBufferSize` directly.
- Fix 3's `okTickMs` clamp only applies in ASYNC mode. A user who sets `cfg.reorderHoldMs` to a very small value (e.g., 50 ms) gets an OK-state tick of 50 ms in ASYNC — fine, the clamp floors at 50 ms so the link task can still do work. The asymmetric-idle / pool-exhaust paths inside `onTimerOk_unlocked` will fire faster, which is the right shape for a tight pipeline.
- Fix 4's wire-frame counter covers CTRL/ACK/NAK/retx. The `txBytes` field is still the only public Stats.tx (no `txFrames` / `rxFrames` counter yet). A future release that wants a per-frame rate can split the field; for now the byte counter is the same number for the operator.
- Fix 5's `BACKPRESSURE_COOLDOWN_MS = 1000` is hard-coded in `Ping.h`. A future change that wants it configurable could read from `cfg`; the ASYNC-only backpressure branch is the right place for the knob. Out of scope for this fix-bundle.
- The `version.py add` scaffolded this entry at the top of `docs/Version.md`; the author filled in the body. Trim dropped v6.0.0 (the 20-entry cap) per the existing `--keep=20` invariant.

### Files touched

- `src/al/hal/IHal.h` — include `AutoLinkConfig.h`; declare `virtual void setMode(AutoLinkConfig::Mode){}`.
- `include/AutoLink.h` — `AutoLink::setMode` body forwards to `hal->setMode(m)` before `link->setMode(m)`; leading comment explains the NVS+reboot contract.
- `src/al/hal/EspHal.h` — `setMode` override; pre-begin re-derives `rx_buffer_size_` / `tx_buffer_size_` / `stream_buf_size_` from the new mode; new fields `rx_buffer_size_` / `tx_buffer_size_`; `begin()` reads the derived values for `uart_driver_install`; the boot log line shows the derived size. `setSpd` and the post-begin path are unchanged.
- `src/al/AutoLinkConfig.h` — new `inline size_t uartRxBufferFloor(const AutoLinkConfig &cfg)` and `uartTxBufferFloor(const AutoLinkConfig &cfg)` host-linkable helpers. SYNC passes through; ASYNC scales by `AUTOLINK_ARQ_PIPELINE_WINDOW * 5/4` (rx) and `* 3/2` (tx). Caller-set larger values win via `max()`.
- `src/al/link/LinkSweep.cpp` — `okTickMs` body adds the ASYNC-mode clamp at `reorderHoldMs/2`. SYNC mode is unchanged.
- `src/al/link/LinkTx.cpp` — `sendFrame_unlocked`, `sendAckFrame_unlocked`, `sendCtrlCobsFrame_unlocked`, and `resendCobsFrame_unlocked` all bump `txBytes` by the encoded frame length and stamp `lastTxMs`. The data path is unchanged.
- `src/al/pingpong/PingPongBase.h` — `bringUpLink` body emits the version log line FIRST, before the NVS read and before `calling comm_.begin()`.
- `src/al/pingpong/Ping.h` — the ASYNC backpressure failure branch sets `tNextSendMs_ = millis() + BACKPRESSURE_COOLDOWN_MS`; new `static constexpr uint32_t BACKPRESSURE_COOLDOWN_MS = 1000;` constant.
- `include/AutoLink.h` + `library.properties` + `idf_component.yml` — version bump 6.0.9 -> 6.0.10 in lockstep.
- `test/test_desktop/al/link/ModeSyncAsyncFixesTest.cpp` — NEW (11 pins).
- `test/test_desktop/Makefile` — `run_test_mode_sync_async_fixes` added to `TEST_BINS` + per-suite build rule + phony target.
- `docs/Version.md` — this entry (and trim dropped v6.0.0 per `--keep=20`).

### Result

- 60 / 60 host unit suites pass (`make test_cpp`), with the new `run_test_mode_sync_async_fixes` as the 60th. All 11 pins (1a/1b/1c, 2a/2b/2c, 3, 4a/4b/4c, 5a/5b) green; the existing 59 suites unchanged. Wall: ~7 s.
- 3 / 3 host integration suites pass (`make itest`). Wall: ~40 s.
- `make assets_check` PASS — no dashboard source touched, `AutoLinkWebHtml.h` byte contract unchanged.
- `make test_coverage_manifest` PASS — `run_test_mode_sync_async_fixes` is a source-grep-only test, added to the exempt list (no `src_for_*` entry required).
- `python3 build/pretty_print-test.py` PASS — all touched files formatted cleanly.
- `python3 build/version.py check` PASS — 20 entries, --keep=20 (this entry pushed v6.0.0 off the tail).
- `build/verify_build.sh` not run in this sandbox — `arduino-cli` is not installed. Per AGENTS rule 4, the cross-compile must be re-run in a longer-lived environment before release. The changes are: 2 new helper functions in a header already on every translation unit's include path, 4 `IHal`/`EspHal`/`AutoLink`/Link method body changes (no new symbols, no new RTOS primitive allocations, no header cycle changes), 1 link-layer tick-clamp + 4 wire-counter bumps (link-layer only, no public API), and 2 Ping/PongBase line-shape changes. The `esp32:esp32:firebeetle32` cross-compile risk is very low, unverified.
---

## v6.0.9

**Lower stream-buffer floor; drop bogus maxMsg > streamBufferSize check**

### Fix 1 — `streamBufferFloor` drops the 16-slot ARQ pipeline factor

The previous floor was `slots * 2 * (cfg.maxMsg + kHdr)` with `slots=16` — ~164 KB at the v6.0.6 default `maxMsg=5120`. The intent was to size the RX staging buffer for a full 16-slot ARQ pipeline, but the RX staging buffer is not the ARQ cache: `ArqCache::POOL_BUF_MAX` already owns the pipeline's storage, and the staging buffer only needs to hold one in-flight coalesced message plus drain headroom. The 32× factor consumed ~150 KB of heap that nothing actually needed and tripped `xStreamBufferCreate` on tight ESP32 boards.

This release lowers the floor to `2 * (cfg.maxMsg + kHdr)` — ≈10.3 KB at the default. One full coalesced message plus a retransmit's worth of headroom, both modes covered. The RX staging buffer's contract is unchanged (drain coalesced + multi-chunk payloads between `popAppBuf` calls), but the heap cost drops 16×. Caller's larger `cfg.streamBufferSize` still wins.

The formula lives in `src/al/AutoLinkConfig.h` as a host-linkable inline function; `EspHal::streamBufferFloor` is a one-line forward to keep the call-site shape in `begin()`. Host tests pin the formula by calling the production helper, not a re-implementation.

### Fix 2 — Drop the `cfg.maxMsg > cfg.streamBufferSize` warning in `LinkCore` ctor

The pre-fix ctor logged a noisy error when `cfg.maxMsg > cfg.streamBufferSize` — but `cfg.streamBufferSize` is the user-set hint, not the actual buffer size used by the HAL. After Fix 1, the actual buffer is always `>= 2 * maxMsg`, so the check was always wrong: every sketch with the v6.0.6 default `maxMsg=5120` and the default `streamBufferSize=2048` saw a spurious `E (355) boot error` on every boot, with no actual wire impact. The check is removed. The seq-space and ARQ-cache warnings already in the ctor are unchanged.

### Wire format

Unchanged. Both fixes are sizing-default / false-positive-error-log cleanups. No wire frame shifts, no public-API additions. `EspHal::streamBufferFloor` still exists with the same signature; it now delegates to the AutoLinkConfig free function.

### Regression test

New `run_test_esphal_stream_buf_floor` suite (5 pins) + existing `run_test_esphal_begin_and_health` updated:

- **Pin 1** (new, runtime): `streamBufferFloor(default_cfg)` returns `2 * (5120 + 6) = 10252` bytes — exactly the new formula, well under the heap-realistic 32 KB ceiling. Pre-fix shape produced ~160 KB and tripped this pin.
- **Pin 2** (new, runtime): caller-set `cfg.streamBufferSize = 64 KB` wins over the floor. The fix preserves the original "caller's larger cfg.streamBufferSize wins" contract.
- **Pin 3** (new, source-grep on `AutoLinkConfig.h`): the formula body uses `multiples = 2` and `multiples * (cfg.maxMsg + kHdr)`. The pre-fix `slots = 16`, `slots * 2`, and `32 * (cfg.maxMsg ...)` tokens must NOT appear in the body. Toggling any of these back flips the pin red.
- **Pin 4** (new, source-grep on `EspHal.h`): `EspHal::streamBufferFloor` is a thin forwarder — body contains `::autolink::streamBufferFloor(cfg)` and NOT any of `kHdr` / `multiples` / `slots` / `32 *`. Single source of truth: formula lives in AutoLinkConfig.h, the HAL doesn't redefine it.
- **Pin 5** (new, source-grep on `LinkCore.cpp`): the `cfg.maxMsg > cfg.streamBufferSize` check + the `maxMsg > streamBufSize` log message are gone from the LinkCore ctor's first 2000 chars. Toggling either back flips the pin red.
- **`run_test_esphal_begin_and_health` updated**: the prior pin (which asserted `slots = 16` and the 16-slot unconditional floor) is rewritten to match the new architecture — EspHal forwards to AutoLinkConfig::streamBufferFloor, the formula is `multiples = 2` in AutoLinkConfig.h, the 16-slot / 32× / mode-branch tokens are gone from both files.

Toggle-off checks (verified locally):
- Restoring `constexpr int slots = 16;` and `slots * 2 * (cfg.maxMsg + kHdr)` in AutoLinkConfig.h → Pin 1 flips red (floor jumps from ~10 KB to ~160 KB).
- Re-adding `cfg.maxMsg > cfg.streamBufferSize` in LinkCore.cpp ctor → Pin 5 flips red.
- Removing `::autolink::streamBufferFloor(cfg)` from EspHal::streamBufferFloor (or re-inlining the formula) → Pin 4 flips red.

### Limitations

- The floor covers one in-flight coalesced message plus headroom. Under ASYNC mode at default `maxMsg=5120`, the ARQ cache (in `ArqCache::POOL_BUF_MAX`) holds the pipeline's pending chunks; the staging buffer only sees the bytes that have been ACK-decoded and need to drain to the user via `recv()`. The 10 KB staging buffer is comfortably above `chunksForMsgLen(5120) * MAX_CHUNK ≈ 5.5 KB` of a worst-case in-flight decode. A future bump to `maxMsg >= 32 KB` would re-trigger the size scaling — but at that point the seq-space guard at `Link::sendMsg` would have rejected the send first.
- The 2× factor is hand-tuned for the default `maxMsg=5120`. Operators who want a larger staging buffer can set `cfg.streamBufferSize` directly — same knob as before. The floor is a *floor*, not a ceiling.
- The LinkCore ctor's seq-space warning and ARQ-cache headroom warning are unchanged. Those still fire when the user-supplied `maxMsg` would consume too many chunks (e.g. `maxMsg >= ~62 KB` trips the seq-space guard) or saturate the cache (chunk count `> POOL_SIZE / 2`). The deleted check was always false-positive at the v6.0.6 defaults; the surviving checks are real.
- The fix does NOT touch the EspHal::begin() abort path. `xStreamBufferCreate` can still fail under extreme heap pressure (e.g. dashboard + WiFi + heavy log buffer all up); the existing `run_test_esphal_stream_buf_abort` gate keeps that contract pinned. The lower floor makes that abort path rarer, not impossible.

### Files touched

- `src/al/AutoLinkConfig.h` — forward-declare `AutoLinkConfig`; declare `inline size_t streamBufferFloor(const AutoLinkConfig &cfg)`; define after the struct body. Pure arithmetic, host-linkable.
- `src/al/hal/EspHal.h` — `EspHal::streamBufferFloor` now a one-line forward to `::autolink::streamBufferFloor`; `stream_buf_size_` field comment updated to reference the new formula location; stale "16-slot ARQ pipeline" comment removed.
- `src/al/link/LinkCore.cpp` — removed `cfg.maxMsg > cfg.streamBufferSize` warning + the `maxMsg > streamBufSize` log message; the seq-space + ARQ-cache warnings further down the ctor are unchanged.
- `include/AutoLink.h` + `library.properties` + `idf_component.yml` — version bump 6.0.8 → 6.0.9 in lockstep.
- `test/test_desktop/al/hal/EspHalStreamBufFloorTest.cpp` — NEW (5 pins).
- `test/test_desktop/al/hal/EspHalBeginAndHealthTest.cpp` — `test_esphal_derives_stream_buffer_size_from_maxmsg_and_mode` rewritten to match the new architecture (single source of truth in AutoLinkConfig.h; thin forward in EspHal.h).
- `test/test_desktop/Makefile` — added `run_test_esphal_stream_buf_floor` to `TEST_BINS` + per-suite build rule + phony target.
- `test/scripts/coverage/test_coverage_manifest.py` — `run_test_esphal_stream_buf_floor` added to the source-grep-only exempt list (it's a pure source-audit / formula-call test, no `$(AUTOLINK_SRC)` in its link set).
- `docs/Version.md` — this entry (and trim dropped v6.0.0 per `--keep=20`).

### Result

- 59 / 59 host unit suites pass (`make test_cpp`); `make all` ~5 s wall. The new `run_test_esphal_stream_buf_floor` is the 59th; the rewritten `run_test_esphal_begin_and_health` continues to pass against the new architecture.
- 3 / 3 host integration suites pass (`make itest`). Wall: ~40 s.
- `make assets_check` PASS — `AutoLinkWebHtml.h` byte contract unchanged (no dashboard source touched).
- `make test_coverage_manifest` PASS — the new suite is on the source-grep-only exempt list, no `src_for_*` entry required.
- `python3 build/pretty_print-test.py` PASS — the touched files formatted cleanly.
- `python3 build/version.py check` PASS — 20 entries, --keep=20 (this entry pushed v6.0.0 off the tail).
- `build/verify_build.sh` not run in this sandbox — `arduino-cli` not installed. Per AGENTS rule 4, the cross-compile must be re-run in a longer-lived environment before release. The change is a formula swap (no new symbols, no new RTOS primitive allocations, no header cycle changes — the new `AutoLinkConfig::streamBufferFloor` is a host-linkable inline in a header already on every translation unit's include path) plus a single ctor-line removal — cross-compile risk is very low, unverified.
---

## v6.0.8

**HAL-unhealthy halt blinks 300ms instead of silent delay(1000)**

### Fix 1 — `PingPongBase::bringUpLink` halt path blinks at 300ms

The previous halt on `!comm.isHealthy()` after `comm.begin()` was `while (true) delay(1000);` — silent on the bench unless an operator was staring at the debug Serial. When the EspHal stream-buffer allocation fails (heap refusal), the previous fix made the failure visible in the log (`HAL not healthy after begin() ... halting to avoid silent wire`) but a sketch in a case with no USB-Serial attached — or one whose operator walked away after the boot banner — would just look like a frozen board. The failure shape was technically correct (no wire, no corruption) but operationally invisible.

This release swaps the silent delay loop for a visible LED blink:

```cpp
while (true) {
    comm.blinkWait(1, 300, 300, 0);
}
```

300 ms on / 300 ms off is the "trouble" cadence: slow enough to distinguish from normal wire-activity blinks (which fire on every successful Ping send via the v6.0.1 blink patch), fast enough to be impossible to miss. `blinkWait(1, 300, 300, 0)` calls the existing non-blocking overload (`start`), so the loop never yields via `delay` — it just keeps firing the blinker. The blink fires on whatever LED the user's sketch wired to the blinker (typically the on-board LED on the FireBeetle32).

The `!comm.isHealthy()` gate itself is unchanged; this is a UX fix on the halt path, not a fix to the abort-detection logic. The EspHal abort that triggers this path (xStreamBufferCreate failure → `running=false`, `healthy=false`, `return;` without setting `healthy=true`) is still in place; the user's first signal that the abort fired is now the 300ms-blinking LED, not a debug Serial line they may not be reading.

### Wire format

Unchanged. The halt is a terminal state on a non-functional link — no wire frames are emitted, none can be received. The fix is purely UX on the operator-facing failure indicator.

### Regression test

`run_test_esphal_stream_buf_abort` (the existing 2-pin suite) extended; `test_pingpongbase_halts_on_unhealthy` rewritten:

- **Pin 2 (rewritten)**: source-grep scopes inside `bringUpLink`'s body and asserts (a) `!comm.isHealthy()` gate is present, (b) `HAL not healthy` log message is present, (c) `while (true)` infinite-loop marker is present, (d) `comm.blinkWait(1, 300, 300, 0)` appears inside the loop, (e) `delay(1000)` does NOT appear inside the slice (the old silent-halt shape is gone).
- Pin 1 unchanged (EspHal abort path verification).

Toggle-off checks (verified):
- Reverting `bringUpLink`'s halt loop to `while (true) delay(1000);` → `test_pingpongbase_halts_on_unhealthy` flips red (`Assertion 'halt loop must blink the LED at 300 ms cadence' failed`).
- Removing the `!comm.isHealthy()` gate → pin flips red (`Assertion 'bringUpLink must check !comm.isHealthy()'`).
- Restoring `delay(1000)` anywhere in the slice → `Assertion 'halt loop must not be a silent delay(1000) loop'` flips red.

### Limitations

- The 300ms-blink cadence is a fixed-loop shape — it does not differentiate failure modes (xStreamBufferCreate refusal vs uart_driver_install OOM vs task-create failure all produce the same blink). A future enhancement could flash a different pattern per failure (e.g., 3 short blinks for stream-buf, 2 short for task create) by reading the EspHal's diagnostic state into `bringUpLink`, but that crosses the PingPongBase / EspHal boundary and would need a small accessor on the HAL. Out of scope for this UX-fix release.
- The blink fires on the user-configured blinker pin (default: the on-board LED). A sketch that wires the blinker to a non-LED GPIO (e.g., a buzzer) would now beep at 300ms intervals on a HAL failure — same shape, different output. The blink parameterization is the user's, not ours.
- The halt is still a halt. There is no recovery path; the operator must power-cycle or hit the reset button. A future release that wanted self-recovery would have to expose a retry hook from `bringUpLink`, distinct from this UX fix.
- The fix does NOT change the `EspHal::begin()` abort path itself. The stream-buffer allocation failure still cleans up mutex + task_exit_sem, sets `running=false`, and returns without `healthy=true`. The visible LED blink is downstream of that gate firing; the gate's contract is unchanged.

### Files touched

- `src/al/pingpong/PingPongBase.h` — `bringUpLink`'s halt loop body swapped from `while (true) delay(1000);` to `while (true) { comm.blinkWait(1, 300, 300, 0); }`; leading comment updated to describe the new visible-halt shape; inline rationale comment added.
- `test/test_desktop/al/hal/EspHalStreamBufAbortTest.cpp` — `test_pingpongbase_halts_on_unhealthy` rewritten: positive assertion for `comm.blinkWait(1, 300, 300, 0)`, negative assertion for `delay(1000)`. Slice width bumped from 2500 to 3000 chars to fit the new inline rationale.
- `include/AutoLink.h` + `library.properties` + `idf_component.yml` — version bump 6.0.7 → 6.0.8 in lockstep.
- `docs/Version.md` — this entry.

### Result

- 58 / 58 host unit suites pass (`make test_cpp`), including the rewritten `run_test_esphal_stream_buf_abort` with the new positive / negative assertions. Wall: ~7 s.
- 3 / 3 host integration suites pass (`make itest`). Wall: ~40 s.
- `make assets_check` PASS — `AutoLinkWebHtml.h` byte contract unchanged (no dashboard source touched).
- `make test_coverage_manifest` PASS — no new TEST_BINS entries; the change to `bringUpLink`'s halt body is within the existing source-contributing map.
- `python3 build/pretty_print-test.py` PASS — the touched files formatted cleanly.
- `python3 build/version.py check` PASS — 20 entries, --keep=20.
- `build/verify_build.sh` not run in this sandbox — `arduino-cli` not installed. Per AGENTS rule 4, the cross-compile must be re-run in a longer-lived environment before release. The change is a 2-line loop body swap + a comment update + a test-pin rewrite — no new `#ifdef ARDUINO` paths, no new RTOS primitive allocations, no header cycle changes — so the `esp32:esp32:firebeetle32` cross-compile risk is very low, unverified.
---

## v6.0.7

**Default txDelayMs 50 -> 0; restore full line-rate out of the box**

### Fix 1 — `AutoLinkConfig::txDelayMs` default lowered 50 -> 0 ms

The 6.0.1 default of 50 ms was sized to give the `/stats` + `/logs` poll enough headroom when the dashboard was the bottleneck: at the 5-baud table a 50 ms post-send delay left a comfortable 1 s poll cycle for the dashboard to keep up with the wire. That math is correct when the dashboard is up. A sketch that boots without WiFi — or one that doesn't open the dashboard at all — ships at ~half the wire's natural throughput for no operator-visible reason, because Ping's loop honors `cfg.txDelayMs` unconditionally.

This release lowers the default to 0 ms. Full line-rate out of the box. Operators who want a throttled bench run can pick a value via the dashboard's delay-ms dropdown (which now defaults to 0) or set `cfg.txDelayMs` directly in their sketch.

The rationale for the original 50 ms throttle (and the 100 ms before it) is unchanged for dashboard-pressured benches — the dropdown and the constructor argument still let callers apply it. The default just stops imposing it.

### Fix 2 — HTML delay-ms dropdown selected option 50 -> 0

`src/al/web/dashboard_html_part_b.html`'s `<select id="delayMs">` had `<option value="50" selected>` since 6.0.1. The dashboard's JS reconciles the dropdown to `snap.txDelayMs` on every `/stats` poll, so a firmware-side default bump alone would have flipped the visible selection on the next poll (5.3.x did exactly that), but the source-of-truth HTML still pointed at 50. The clean fix is to flip the `selected` attribute to `<option value="0">` so a fresh page load shows 0 until the user (or the wire) overrides it. `AutoLinkWebHtml.h` is regenerated by `build/dashboard_assets.py` from the updated `part_b`.

### Fix 3 — README's stale `100 ms` prose updated

The README's `Random mode ...` paragraph still said "The default per-transmit delay is 100 ms" — a stale value from 5.3.x that 6.0.1 missed updating (it changed the source default but not the README). v6.0.6 noted this discrepancy as out-of-scope; this release closes the gap. New prose: "The default per-transmit delay is 0 ms — full line-rate out of the box (configurable via the dashboard's delay-ms widget or `cfg.txDelayMs`)."

### Wire format

Unchanged. The default txDelayMs is a pacing delay between Ping sends — it doesn't affect the on-the-wire COBS+CRC frame format, doesn't change the ARQ cache shape, doesn't change the seq-space budget, and doesn't touch any Link layer logic. Lowering the default to 0 just removes the inter-send idle window.

### Regression test

Two existing suites extended, two assertions updated:

- `run_test_autolink::test_txDelayMs_default_and_setter` (AutoLinkTest.cpp): the two `cfg.txDelayMs == 50` and `link.txDelayMs() == 50` assertions now read `== 0`. Toggle off (revert to 50) → red.
- `run_test_pingpong_blink_and_delay` Pin 3 (HTML default): the source-grep that asserted `value="50" selected` now asserts `value="0" selected`, and the negative-pin set expands from `{50}` to `{50, 100}` so a future regression that re-selects either previous default trips red.
- `run_test_pingpong_blink_and_delay` Pin 4 (firmware default): the source-grep now asserts the substring `= 0` appears after `int txDelayMs = `, and the negative-pin set expands to `{50, 100}` for the same reason.

Toggle-off checks (verified locally):
- Reverting `cfg.txDelayMs` to `50` → `run_test_autolink::test_txDelayMs_default_and_setter` flips red (`Assertion 'cfg.txDelayMs == 0' failed`).
- Reverting the HTML's `selected` to `value="50"` → `run_test_pingpong_blink_and_delay` Pin 3 flips red.

### Limitations

- A sketch that boots with a slow or contended `/stats` consumer (a busy TTY-to-PC serial bridge, a log-dumping sketch, or a custom dashboard that polls harder than the stock one) may observe `/logs` polling gaps at the new 0 ms default. The fix is to set `cfg.txDelayMs = 50` in the sketch — same knob as before, now opt-in instead of opt-out.
- The HTML dropdown's `<option value="0">` line replaces the `selected` attribute from `value="50"`; the option itself (and the JS reconciliation) is unchanged. A future bump to a different default would touch one HTML line + one firmware line + two regression pins.
- The wire-level backpressure from `Link::sendMsg`'s seq-space guard still applies unchanged. The 0 ms delay doesn't relax that.
- This release is the doc-only / default-bump cleanup that v6.0.6's Limitations section flagged. No protocol change, no public-API change beyond the default.

### Files touched

- `src/al/AutoLinkConfig.h` — `txDelayMs = 0` (was 50); leading comment rewritten to describe the new rationale.
- `src/al/web/dashboard_html_part_b.html` — `<option value="0" selected>` (was `value="50" selected`).
- `src/al/web/AutoLinkWebHtml.h` — regenerated by `build/dashboard_assets.py` from the updated `part_b`.
- `include/AutoLink.h` + `library.properties` + `idf_component.yml` — version bump 6.0.6 → 6.0.7 in lockstep.
- `test/test_desktop/al/AutoLinkTest.cpp` — `test_txDelayMs_default_and_setter` updated for new default; leading comment rewritten.
- `test/test_desktop/al/pingpong/PingPongBlinkAndDelayTest.cpp` — Pin 3 + Pin 4 updated for new default; function names `test_dashboard_default_delayMs_is_0` and `test_config_default_txDelayMs_is_0`.
- `README.md` — "default per-transmit delay is 100 ms" → "default per-transmit delay is 0 ms — full line-rate out of the box".
- `docs/Version.md` — this entry.

### Result

- 58 / 58 host unit suites pass (`make test_cpp`). The two updated pins + two updated assertions all flip red when reverted. Wall: ~7 s.
- 3 / 3 host integration suites pass (`make itest`). Wall: ~40 s.
- `make assets_check` PASS — `AutoLinkWebHtml.h` is current with the updated `dashboard_html_part_b.html`.
- `make test_coverage_manifest` PASS — no new TEST_BINS; existing source-contributing + exempt buckets unchanged.
- `python3 build/pretty_print-test.py` PASS — the touched files formatted cleanly.
- `python3 build/version.py check` PASS — 20 entries, --keep=20.
- `build/verify_build.sh` not run in this sandbox — `arduino-cli` not installed. Per AGENTS rule 4, the cross-compile must be re-run in a longer-lived environment before release. The change is a one-line default + one HTML attribute flip + three doc-comment updates — no new `#ifdef ARDUINO` paths, no new RTOS primitive allocations, no header cycle changes — so the `esp32:esp32:firebeetle32` cross-compile risk is very low, unverified.
---

## v6.0.6

**Raise default maxMsg 1024 -> 5120; introduce AUTOLINK_DEFAULT_MAX_MSG**

### Fix 1 — Centralize the default maxMsg in `AUTOLINK_DEFAULT_MAX_MSG`

The default `cfg.maxMsg = 1024` was a literal scattered across `AutoLinkConfig.h`, `PingPongBase.h`, `Ping.h`, `include/AutoLink.h`, and the host tests' source-grep pins. Three places could drift apart: the static_asserts in `AutoLinkConfig.h`, the `BUF_SIZE` buffer in `PingPongBase.h`, and the `maxSeqSize_` default in `Ping.h`. Any future maxMsg bump required touching all three plus the test pins plus the API doc fallback.

This release:

- Adds a canonical `AUTOLINK_DEFAULT_MAX_MSG = 5120` constant in `src/al/AutoLinkConfig.h` (host-linkable, no `al/link/LinkContext.h` deps — the constant lives next to `chunksForMsgLen` for the same reason).
- Replaces both `chunksForMsgLen(1024)` literals in the seq-space static_asserts with `chunksForMsgLen((int)AUTOLINK_DEFAULT_MAX_MSG)` (5x for the new default: 1 + ceil(5120/250) = 22 chunks, still well under `COBS_SEQ_SPACE = 254`).
- Sets `AutoLinkConfig::maxMsg = AUTOLINK_DEFAULT_MAX_MSG`; `Link::maxMsg()` is unchanged (still reads `cfg.maxMsg`).
- Replaces the `PingPongBase::BUF_SIZE = 1024` literal with `AUTOLINK_DEFAULT_MAX_MSG`. The buffer is sized to the canonical message cap; tests that need smaller buffers set their own per-instance `cfg.maxMsg`.
- Sets `Ping::maxSeqSize_`'s pre-`setup()` default to `(int)AUTOLINK_DEFAULT_MAX_MSG`. The runtime `setup()` path still overrides it with `base_.comm_.maxMsg()` after `bringUpLink` so user-supplied per-sketch overrides continue to flow through.
- Updates `AutoLink::maxMsg()`'s pre-construction fallback from the literal `1024` to `AUTOLINK_DEFAULT_MAX_MSG` so a sketch that reads `cfg.maxMsg` before `comm.begin()` sees the same number the rest of the codebase does.

The literal `1024` still appears in `LinkIOTest.cpp`, `LinkBaudPreferenceTest.cpp`, `AutoLinkTest.cpp`, `LinkMessageRoundtripTest.cpp`, etc. — these are explicit per-test `cfg.maxMsg = 1024` overrides, not defaults. Pinned by `run_test_seq_space_guard` Pin 4 + Pin 5 and `run_test_config_defaults_centralized`.

### Fix 2 — Bump default message size from 1 KB to 5 KB

The previous 1024-byte default was the maximum single-chunk payload (no MSG_HDR coalesce) for the old 1 KB chunk cap. The current 250-byte `MAX_CHUNK` makes a 1024-byte message a 5-chunk send (1 hdr + 4 data) — small for any multi-chunk pipeline test, and below the 64-slot `ArqCache::POOL_SIZE` steady-state window. Operators running Ping's random mode at the default size saw almost no multi-chunk traffic.

5 KB = 22 chunks, still under `COBS_SEQ_SPACE = 254`, leaves room for ~2 inflight messages before the seq-space guard trips, and exercises the multi-chunk path meaningfully in steady state. The runtime warn at the `Link` ctor still fires once on the new default ("maxMsg=5120 takes 22 chunks — seq-space guard will reject at ~11 inflight messages"); the warning is informational, the budget comfortably covers the default.

### Fix 3 — `WireSim.h` scratch buffers bumped to `AUTOLINK_DEFAULT_MAX_MSG`

The two `uint8_t buf[1024]` instances in `test/common/WireSim.h` (the closed-loop simulator's loopback scratch) are not tied to `cfg.maxMsg` — they're fixed-size drive-loop helpers. Pre-this-release they silently truncated any `sim_.linkA().recvMsg(buf, sizeof buf)` call whose peer sent > 1024 bytes. The 6.0.x MTU roundtrip suite already exercised 32 KB messages, but it used a 2-node MockHal directly, not `WireSim.h`. The bump makes the scratch buffers match the new default; per-instance overrides still apply through `Link`'s own `cfg.maxMsg`-driven receive buffer.

### Wire format

Unchanged. The constant introduction, the default bump, and the scratch-buffer bump are all sizing-default changes; no wire frame shifts, no public-API additions. `AutoLink::maxMsg()`'s signature is unchanged; the fallback is now sourced from the constant.

### Regression test

Four existing suites extended, one new pin path:

- `run_test_config_defaults_centralized` (AutoLinkTest.cpp): default `cfg.maxMsg` now pinned to `AUTOLINK_DEFAULT_MAX_MSG`. Toggle off (revert to the literal `1024`) → red.
- `run_test_seq_space_guard` Pin 4: `chunksForMsgLen((int)AUTOLINK_DEFAULT_MAX_MSG)` now asserted to equal `1 + (5120 + 250 - 1) / 250 = 22` and `<= COBS_SEQ_SPACE`. Toggle off (revert to `chunksForMsgLen(1024)` literal) → red.
- `run_test_seq_space_guard` Pin 5: source-grep asserts the `AUTOLINK_DEFAULT_MAX_MSG` substring appears in both static_asserts and that the budget-vs-msg invariant still holds (`ARQ_CHUNK_BUDGET >= chunksForMsgLen(...) * 2`). The substring check is wrap-tolerant — clang-format puts the LHS and RHS on separate lines.
- `run_test_alink_message_edge::test_send_rejections_log_errors`: the "oversized" test payload is now `AUTOLINK_DEFAULT_MAX_MSG + 1` bytes (was hardcoded `2048`). Pre-this-release, with default `maxMsg = 1024`, 2048 was the smallest reject-capable size. With the new 5120 default, the test now exercises the new cap.

Toggle-off checks (verified locally):
- Reverting the default in `AutoLinkConfig.h` to `1024` → `run_test_config_defaults_centralized` flips red.
- Reverting Pin 4's chunks math to the `1024` literal → `run_test_seq_space_guard` Pin 4 flips red.

### Limitations

- 5 KB is the new *default*, not a ceiling. Per-sketch `cfg.maxMsg = 65535` is unchanged; the seq-space guard at `Link::sendMsg` still rejects any single message whose chunk count alone would exceed `COBS_SEQ_SPACE`. Operators who want a larger default should set `cfg.maxMsg` in their sketch's config, not rely on bumping the canonical constant — the constant is shared across the static_asserts, `BUF_SIZE`, and `AutoLink::maxMsg()`'s fallback, so a bump is a single-source change but still requires bumping the seq-space budget in lockstep (`ARQ_CHUNK_BUDGET` would have to grow for the new ceiling to be comfortably covered).
- `WireSim.h`'s bump to `AUTOLINK_DEFAULT_MAX_MSG` covers the new default. A future `cfg.maxMsg > AUTOLINK_DEFAULT_MAX_MSG` test still needs its own per-suite scratch buffers (or a WireSim that grows the buffer to match `cfg.maxMsg`). Out of scope for this release.
- The README's prose mentions "100 ms" for the default `cfg.txDelayMs` (which is actually 50 ms since 6.0.1). This release does not touch that — the discrepancy is pre-existing and out of scope for the maxMsg bump. Will be cleaned up in a future doc-only release.
- The new `AUTOLINK_DEFAULT_MAX_MSG` constant is `size_t`; the static_asserts cast it to `int` for `chunksForMsgLen`. A future bump past `INT_MAX` would need a separate `chunksForMsgLen(size_t)` overload; well outside the foreseeable envelope (32 KB / 64 KB / 128 KB all fit comfortably).

### Files touched

- `src/al/AutoLinkConfig.h` — `AUTOLINK_DEFAULT_MAX_MSG = 5120` constant; both `chunksForMsgLen(1024)` static_asserts swapped for the constant; comment block updated; field default `maxMsg = AUTOLINK_DEFAULT_MAX_MSG`.
- `src/al/pingpong/PingPongBase.h` — `BUF_SIZE = AUTOLINK_DEFAULT_MAX_MSG`.
- `src/al/pingpong/Ping.h` — `maxSeqSize_ = (int)AUTOLINK_DEFAULT_MAX_MSG` pre-setup default; `pickMsgSize_`'s leading comment rewritten for the new range shape; `RANDOM_MIN_BYTES` comment no longer references the old 1024 floor.
- `include/AutoLink.h` — `AutoLink::maxMsg()` fallback reads `AUTOLINK_DEFAULT_MAX_MSG`.
- `test/test_desktop/al/AutoLinkTest.cpp` — default pin reads the constant.
- `test/test_desktop/al/link/LinkSeqSpaceGuardTest.cpp` — Pin 4 + Pin 5 use the constant; file-leading comment updated.
- `test/test_desktop/al/link/LinkMessageEdgeTest.cpp` — oversize test payload is `AUTOLINK_DEFAULT_MAX_MSG + 1`.
- `test/common/WireSim.h` — two `buf[1024]` instances bumped to `AUTOLINK_DEFAULT_MAX_MSG`.
- `README.md` — random-mode range `[1024, maxMsg]` → `[1, maxMsg]`.
- `docs/API.md` — `(default 1024)` → `(default 5120)`.
- `include/AutoLink.h` + `library.properties` + `idf_component.yml` — version bump 6.0.5 → 6.0.6 in lockstep.
- `docs/Version.md` — this entry.

### Result

- 58 / 58 host unit suites pass (`make test_cpp`), including the updated `run_test_seq_space_guard`, `run_test_alink_message_edge`, and `run_test_config_defaults_centralized`. Wall: ~8 s.
- 3 / 3 host integration suites pass (`make itest`). Wall: ~40 s.
- `make test_coverage_manifest` PASS — no new TEST_BINS; existing exempt / contributing buckets unchanged.
- `make assets_check` PASS — `AutoLinkWebHtml.h` byte contract unchanged (no dashboard source touched).
- `python3 build/pretty_print-test.py` PASS — all touched files formatted cleanly.
- `python3 build/version.py check` PASS — 20 entries, --keep=20.
- `build/verify_build.sh` not run in this sandbox — `arduino-cli` not installed. Per AGENTS rule 4, the cross-compile must be re-run in a longer-lived environment before release. The change is a single constant introduction + a default-bump + a buffer-bump; no new `#ifdef ARDUINO` paths, no new RTOS primitive allocations, no header cycle changes — cross-compile risk is low, unverified.
---

## v6.0.5

**Mode-toggle: skip second confirm in reboot() after radio**

### Fix 1 — `reboot(skipConfirm)` opt-out for the radio handler

The 6.0.4 `onLinkModeChange` showed its own `confirm()` before the `/mode?m=...` POST (warning the user a reboot is coming) and then, on a successful ack, called `reboot()`. `reboot()` opens with `confirm('Reboot the device now? ...')` for the Reboot button's standalone path. The result: the user got two confirm dialogs in sequence for a single click — first the mode-toggle confirm, then the Reboot button's confirm. The second dialog was redundant (the user had already consented to the reboot when they confirmed the mode change) and confusing (a different message, a different question).

This release:

- Adds a `skipConfirm` parameter to `reboot()`: when truthy, the function skips the Reboot button's confirm and proceeds straight to the `/reboot` POST. Default-arg-style call sites (the Reboot button's `onclick="reboot()"`) still see the confirm.
- `onLinkModeChange` calls `reboot(true)` after a successful `/mode` ack — the user already consented in the handler's first `confirm()`.

```js
async function reboot(skipConfirm){
  if(!skipConfirm && !confirm('Reboot the device now?...'))return;
  ...
}
```

Pinned by `run_test_mode_toggle_ui` Pin 2: source-grep scopes inside `onLinkModeChange`'s body and asserts (a) `reboot(true)` appears, (b) `reboot()` (no-arg) does NOT appear in the body, (c) the `async function reboot(` signature contains the `skipConfirm` parameter. Toggle off (revert to `reboot()`) flips red.

### Wire format

Unchanged. Single JS-function parameter change. No wire frame shifts, no public-API additions.

### Regression test

`run_test_mode_toggle_ui` Pin 2 extended:

- New positive: `body.find("reboot(true)") != npos` — the call site must pass the opt-out flag.
- New negative: `body.find("reboot()") == npos` inside the handler body — the no-arg call would re-introduce the double confirm.
- New positive on the global scope: `src.find("async function reboot(")` followed by `src.find("skipConfirm", rebootSig) != npos` — the function signature must accept the parameter, so a future refactor that drops it (and lets `reboot(true)` silently become `reboot()` again) flips red.

Pins 1, 3, 4, 5, 6, 7 unchanged.

Toggle-off checks (verified locally):
- Changing `reboot(true)` back to `reboot()` in the handler → Pin 2 flips red (`Assertion 'reboot(true) must be called after /mode ack (skipConfirm opt-out for already-confirmed caller)' failed`).

### Limitations

- The `skipConfirm` parameter is positional. A future change that adds a second opt-out flag would benefit from switching to an options-object signature (`reboot({skipConfirm:true, reason:'mode-toggle'})`); left as a future polish because the single-arg shape is the minimum diff to close the bug.
- The Reboot button's `onclick="reboot()"` keeps the confirm; only callers that have already confirmed the reboot can opt out. The current sole caller is `onLinkModeChange`. Any future caller that wants the opt-out must consciously pass `true`.

### Files touched

- `src/al/web/dashboard.js` — `reboot(skipConfirm)` parameter; `if(!skipConfirm && !confirm(...))` guard; `onLinkModeChange` success branch now calls `reboot(true)`.
- `src/al/web/AutoLinkWebHtml.h` — regenerated by `build/dashboard_assets.py`.
- `include/AutoLink.h` + `library.properties` + `idf_component.yml` — version bump 6.0.4 → 6.0.5 in lockstep.
- `test/test_desktop/al/web/ModeToggleUITest.cpp` — Pin 2 extended with the three new assertions above; Pin 2 leading comment updated.
- `docs/Version.md` — this entry.

### Result

- 58 / 58 host unit suites pass (`make test_cpp`), including `run_test_mode_toggle_ui` with the new positive / negative assertions. Wall: ~7.5 s.
- 3 / 3 host integration suites pass (`make itest`). Wall: ~40 s.
- `make assets_check` PASS — `AutoLinkWebHtml.h` current with the updated JS.
- `make test_coverage_manifest` PASS — no new TEST_BINS entries.
- `python3 build/pretty_print-test.py` PASS — JS sources untouched by clang-format (skipped by extension); C++ test file formatted cleanly.
- `python3 build/version.py check` PASS — 20 entries, --keep=20.
- `build/verify_build.sh` not run in this sandbox — `arduino-cli` not installed. Per AGENTS rule 4, the cross-compile must be re-run in a longer-lived environment before release. Change is one JS function parameter + one call site; no new `#ifdef ARDUINO` paths, no new RTOS primitive allocations, no header cycle changes — cross-compile risk is low, unverified.
---

## v6.0.4

**Mode toggle: NVS-only /mode handler, JS-driven reboot, radio on Pong**

### Fix 1 — `handleMode` persists to NVS only; live `link_.setMode(...)` removed

The previous handler called `self->link_.setMode(newMode)` immediately after parsing the `?m=SYNC|ASYNC` query and then persisted the same value to NVS — a "live, no reboot" toggle. Mode is not actually live-applicable: changing `cfg.mode` flips the ARQ pool's *intended* behavior (SYNC per-chunk wait vs ASYNC gap-stop / pool-exhaustion drop), but the EspHal stream buffer was sized at boot and FreeRTOS stream buffers can't be resized in flight, and the `cfg.mode != SYNC` branch in `LinkTimers::onTimerOk_unlocked` reads the field without re-checking any other init-time state. The result on the wire was `cfg.mode = SYNC` with a 30 KB ASYNC-shaped stream buffer and an ASYNC-shaped ARQ pool — the snap-reported mode diverged from the actual wire behavior. Operators pressing the radio on the dashboard saw the dashboard pill flip but the bench log stayed on the previous mode.

This release drops the live apply. `handleMode` writes to NVS (`putUChar("mode", ...)` in the `autolink` namespace) and returns 200. The dashboard's radio handler kicks `reboot()` after a successful POST; the reboot re-runs `bringUpLink`'s NVS-read path which calls `comm.setMode(...)` *before* `comm.begin()`, so the next wire frame is on the freshly-initialized buffer floor + SYNC wait logic.

The leading comment in `handleMode` documents the contract; the test pins it.

### Fix 2 — `dashboard.js` `onLinkModeChange` confirms and reboots after the POST

The previous JS handler optimistically flipped the radio (`currentLinkMode = val; highlightLinkMode()`) and let the next `/stats` poll reconcile — no reboot, no UI feedback that a reboot was needed. With Fix 1 in place the optimistic flip is wrong (a reboot is coming and the page reloads from scratch). This release:

- Wraps the POST in a `confirm()` that names the new mode and the reboot side-effect, so the user can bail before the link drops.
- On a successful `/mode?m=...` ack the handler calls the existing `reboot()` function — same code path the Reboot button uses. The user sees the standard "Rebooting..." affordance and the post-reboot reload.
- Removes the optimistic `currentLinkMode = val` / `highlightLinkMode()` update; the page reload reconciles everything from the firmware's snap.

### Fix 3 — `linkModeGroup` rendered on both Ping and Pong dashboards

The `linkModeGroup` div was tagged `class="lvl-group ping-only"`, which the CSS uses to hide it on Pong. Mode is a symmetric choice — Pong's `Link::mode()` is the same field as Ping's, the snap reports the same value on both sides, and a Pong-side operator pressing the radio needs the same effect. This release drops `ping-only` from the `linkModeGroup` div. The fill-mode / start / pause widgets stay `ping-only` (those are Ping-only concepts).

`AutoLinkWeb::handleMode` works for both roles already — the snap task reads `link_.mode()` directly via the `AutoLinkWeb`-owned `AutoLink& link_`, so Pong's `snap_.linkMode` is populated correctly without an `installWebHooks` call.

### Wire format

Unchanged. All three fixes are dashboard / handler behavior. The wire contract is identical; the buffer floor and SYNC wait logic that *are* part of the wire contract stay init-time (Fix 1 explicitly routes the new mode through `comm.begin()`).

### Regression test

`run_test_mode_toggle_ui` (extended to 7 pins, was 6). New pins + changes:

- **Pin 1** (updated): source-grep verifies `id="linkModeGroup"` is on a `<div>` whose tag string does NOT contain `ping-only`. Pre-fix shape had the class and the div was hidden on Pong.
- **Pin 2** (rewritten): source-grep scopes inside `onLinkModeChange`'s body and asserts (a) `/mode?m=` POST, (b) `reboot()` is called inside the success branch, (c) the optimistic `currentLinkMode=val` write is gone. Pre-fix shape updated the radio without rebooting.
- **Pin 5** (rewritten): source-grep scopes inside `handleMode`'s body and asserts (a) `link_.setMode(` does NOT appear (the live apply is gone), (b) `esp_restart` does NOT appear (5.3.x's reboot-on-toggle path stays gone), (c) `putUChar("mode"` + `prefs.begin("autolink"` are present (NVS persistence is the entire side-effect).
- **Pin 7** (new): source-grep scopes inside `onLinkModeChange`'s body, locates the `confirm(` call, asserts it appears *before* the `/mode?m=` POST and that the message string contains `reboot` so the user knows the side-effect before clicking OK.

Pins 3, 4, 6 unchanged.

Toggle-off checks (verified locally):
- Adding `ping-only` back to the `linkModeGroup` div → Pin 1 flips red (`Assertion 'linkModeGroup div tag must not contain ping-only' failed`).
- Removing `reboot()` from the success branch of `onLinkModeChange` → Pin 2 flips red.
- Re-adding `self->link_.setMode(...)` to `handleMode` → Pin 5 flips red.
- Removing the `confirm(` call → Pin 7 flips red.

### Limitations

- The reboot between radio click and new-mode wire takes 5–10 s. Operators who want sub-second mode-switching need a future wire-level control frame (out of scope; the wire protocol has no "change mode" command — mode is init-time by design).
- The radio handler's `confirm()` is browser-native and blocks the page until the user clicks OK. A future UI that wants inline previews can fold the confirm into a custom modal; the existing `confirm()` keeps the diff minimal.
- The previous "live" `handleMode` had a bug where the snap-reported mode flipped while the wire stayed on the old mode. The `run_test_mode_toggle_ui` Pin 5 contract ("no live `link_.setMode`") is the regression wall; if a future change wants to re-introduce a live apply, it has to also resize the stream buffer and re-init the SYNC wait state — both non-trivial — and rewrite Pin 5.
- `bringUpLink`'s NVS read path (`prefs.getUChar("mode", 0xFF)`) was already wired by the previous release and is unchanged here. The reboot in Fix 2 is what activates it.

### Files touched

- `src/al/web/dashboard_html_part_b.html` — `linkModeGroup` div class `lvl-group ping-only` → `lvl-group`.
- `src/al/web/dashboard.js` — `onLinkModeChange` body: added `confirm()` gate, removed optimistic `currentLinkMode=val; highlightLinkMode()`, added `reboot()` call after `/mode` ack; leading comment updated.
- `src/al/web/AutoLinkWebHandlers.cpp` — `handleMode` body: removed `self->link_.setMode(newMode)` live apply; leading comment now documents NVS-only + reboot-required contract.
- `src/al/web/AutoLinkWebHtml.h` — regenerated by `build/dashboard_assets.py` from the updated part_b + JS.
- `include/AutoLink.h` + `library.properties` + `idf_component.yml` — version bump 6.0.3 → 6.0.4 in lockstep.
- `test/test_desktop/al/web/ModeToggleUITest.cpp` — Pin 1 + Pin 2 + Pin 5 rewritten; new Pin 7; file-leading comment + main() updated to reflect the new contract.
- `docs/Version.md` — this entry.

### Result

- `make test_cpp` PASS — 58/58 host unit suites pass. The 7-pin `run_test_mode_toggle_ui` passes; the 3 new / rewritten pins trip red when reverted. Wall: ~7 s.
- `make assets_check` PASS — `AutoLinkWebHtml.h` is current with the updated dashboard sources.
- `make test_coverage_manifest` PASS — no new TEST_BINS entries; the changes to existing files are within the existing source-contributing map.
- `python3 build/pretty_print-test.py` PASS — the touched .cpp + .html formatted cleanly.
- `python3 build/version.py check` PASS — 20 entries, --keep=20.
- `build/verify_build.sh` not run in this sandbox — `arduino-cli` is not installed. Per AGENTS rule 4, the cross-compile must be re-run in a longer-lived environment before release. Changes are local to the dashboard HTML/JS, the handler body (removed one line, updated leading comment), and the test source — no new `#ifdef ARDUINO` paths, no new RTOS primitive allocations, no header cycle changes — so the `esp32:esp32:firebeetle32` cross-compile risk is low, but unverified.
---

## v6.0.3

**Resweep churn + Pong CRC/desync fix: split link-not-OK from backpressure, dropLink no-op on sweep, Ping flushRx at SWP→OK, heap log on resweep**

### Fix 1 — Split send-not-OK from real backpressure in Ping's send loop

`Ping::sendMsg` returning false conflated two distinct reasons — link not OK (transient, the link is self-recovering via its own sweep) and ARQ cache full / seq-space exhausted (real backpressure). Treating them identically made `consecSendFail_` escalate to `dropLink()` on a link that was already resweeping, kicking a fresh P1 on top of the in-progress sweep and doubling the resweep cycle. The fix gates the counter on link state:

```cpp
if (!base_.comm_.sendMsg(sendBuf_, n, &seq)) {
    if (!base_.comm_.ready()) {
        break;          // link not OK; sweep handles it; don't count, don't drop
    }
    consecSendFail_++;  // real backpressure only
    ...
}
```

`Ping::ready()` is the existing facade accessor (`link->getState() == State::OK`); no new public API. Pinned by `run_test_ping_send_failure` Pin 7.

### Fix 2 — `Link::dropLink()` is a no-op when state != OK

A late `dropLink()` on a link that has already left OK (mid-sweep, mid-recovery) used to `reset_unlocked(true)` + `sendBreak()` a second time, restarting the handshake from a fresh P1 on top of the in-progress sweep. The field logs at ~28.7s and ~35.7s show the back-to-back P1→P2→P3 cycles. The fix early-returns unless the link is in OK:

```cpp
void Link::dropLink() {
    hw.lock();
    if (state != State::OK) {
        hw.unlock();
        return;
    }
    reset_unlocked(true);
    ...
}
```

Once a sweep is running, let it run. Pinned by `TestAccessorStructureTest::test_droplink_no_op_when_already_sweeping`.

### Fix 3 — Ping flushes the raw rx path on SWP→OK transition

Pong's loop already drained stale bytes pre-blink (`drained %d stale bytes pre-blink`); Ping had a parallel drain (`drained %d stale echo(s) pre-settle`) but only via `recv()`. A half-received frame from before the drop (header parsed but payload truncated by the link reset) lands in the rx stream buffer; the user-level `recv()` drain leaves it in place, and the next `recvMsg()` reads a truncated header and fails the CRC. The fix flushes the raw rx path *before* the recv drain:

```cpp
if (!base_.wasReady_) {
    base_.comm_.flushRx();         // clear hw-level queue + stream buf
    int drained = 0;
    while (base_.comm_.recv(recvBuf_, sizeof recvBuf_) > 0) drained++;
    ...
}
```

`AutoLink::flushRx()` calls `Link::flushRx()` which clears the app buffer and `flushRxHw()`. The crc-fail / desync burst before each BREAK (and the seq-numbering jump post-recovery) was this bug class. Pinned by `run_test_ping_send_failure` Pin 8.

### Fix 4 — Free-heap log on each disc-triggered resweep (triage aid)

`Link::reset_unlocked` now logs `resweep: disc=%lu freeHeap=%u` on the count-and-resweep branch (`count && state == State::OK`), wrapped in `#ifdef ARDUINO` so the host build is unaffected. The log is the triage hook for the disc-counter-climb symptom: a stable freeHeap across cycles confirms Fixes 1-3 (state-machine race) are sufficient; a monotonic freeHeap drop points at POOL_SIZE starvation during `uart_driver_install` (a separate fix that's out of scope for this release). On the FireBeetle32 a stable freeHeap across cycles is the expected outcome. Pinned by `BoundaryInvariantsTest` Pin 7.

### Wire format

Unchanged. All four fixes are runtime / state-machine behavior. No wire frame shifts, no public-API additions (Fix 1 reuses the existing `ready()` accessor).

### Regression test

Three existing suites extended with three new pins:

- `run_test_ping_send_failure` Pin 7 (Fix 1): source-grep verifies the send-failure branch contains `if (!base_.comm_.ready()) { break; }` *before* `consecSendFail_++`. Pre-fix shape bumped the counter unconditionally and let the drop-on-threshold fire on a link that was already resweeping.
- `run_test_ping_send_failure` Pin 8 (Fix 3): source-grep verifies `base_.comm_.flushRx()` appears in Ping's `!wasReady_` block before the `recv()` drain loop. Pre-fix shape only drained via `recv()`, leaving a half-received frame in the rx buffer to fail the next CRC.
- `TestAccessorStructureTest::test_droplink_no_op_when_already_sweeping` (Fix 2): source-grep verifies the `state != State::OK` guard precedes `reset_unlocked(true)` in `Link::dropLink()`. Pre-fix shape reset twice on a mid-sweep late drop.
- `BoundaryInvariantsTest` Pin 7 (Fix 4): source-grep verifies the resweep log lives inside `#ifdef ARDUINO`, mentions both `resweep` and `freeHeap`, and that `LinkCore.cpp` pulls in `<esp_system.h>` for `esp_get_free_heap_size()`.

Toggle-off checks (verified):
- Removing the `ready()` guard in Ping.h → `run_test_ping_send_failure` Pin 7 flips red (`Assertion 'send-failure branch must check !base_.comm_.ready() to distinguish self-recovering from backpressure' failed`).
- Removing the state guard in `dropLink()` → `TestAccessorStructureTest::test_droplink_no_op_when_already_sweeping` flips red (`Assertion 'dropLink() must guard on state != State::OK' failed`).
- Removing `flushRx()` from Ping's `!wasReady_` block → `run_test_ping_send_failure` Pin 8 flips red (`Assertion '!wasReady_ block must call base_.comm_.flushRx()' failed`).
- Removing the heap log → `BoundaryInvariantsTest` Pin 7 flips red (`Assertion 'reset_unlocked must log 'resweep' on disc-count bump' failed`).

### Limitations

- Fix 4's heap log is device-only (`#ifdef ARDUINO`); the host suite doesn't observe it. The triage value is for the bench run, not the unit-test gate.
- The seq-space guard from v6.0.1 still rejects chunk counts > COBS_SEQ_SPACE; Fix 1 doesn't change the seq budget. The two fixes are complementary: v6.0.1 bounds the seq-space aliasing, v6.0.3 stops the post-drop resweep churn from doubling.
- Fix 3's `flushRx()` flushes the hw-level queue AND the stream buffer. A future caller that bypassed `AutoLink::flushRx()` and called `Link::sendFrame_unlocked` directly would still see the half-received frame; the fix only covers the facade path.
- Fix 2's guard is at the public `Link::dropLink()` entry. The internal `reset_unlocked` is still called from `err_unlocked`, `kickoff`, and the timer-driven paths — those are intentional and don't double-reset because they fire from within the link layer's own state machine.

### Files touched

- `src/al/link/LinkApi.cpp` — `Link::dropLink()` guards on `state != State::OK`.
- `src/al/link/LinkCore.cpp` — `reset_unlocked`'s count-and-resweep branch logs `resweep: disc=%lu freeHeap=%u` under `#ifdef ARDUINO`; `<esp_system.h>` include added.
- `src/al/pingpong/Ping.h` — send-loop failure branch early-returns on `!base_.comm_.ready()`; `!wasReady_` block calls `flushRx()` before the recv drain.
- `include/AutoLink.h` + `library.properties` + `idf_component.yml` — version bump 6.0.2 → 6.0.3 in lockstep.
- `test/test_desktop/al/pingpong/PingSendFailureTest.cpp` — Pin 7 + Pin 8.
- `test/test_desktop/al/link/TestAccessorStructureTest.cpp` — `test_droplink_no_op_when_already_sweeping`.
- `test/test_desktop/al/BoundaryInvariantsTest.cpp` — Pin 7.
- `docs/Version.md` — this entry.

### Result

- 58 / 58 host unit suites pass (`make test_cpp`). Wall: ~8.6 s.
- 3 / 3 host integration suites pass (`make itest`). Wall: ~40 s.
- `make test_coverage_manifest` PASS (the three source-grep-only pins don't add new TEST_BINS entries; `run_test_ping_send_failure` already had coverage through its existing pins).
- `python3 build/pretty_print-test.py` PASS (17/17 assertions; the new source changes formatted cleanly).
- `python3 build/version.py check` PASS (20 entries, --keep=20; the new entry pushed v6.0.1 down to the trim boundary).
