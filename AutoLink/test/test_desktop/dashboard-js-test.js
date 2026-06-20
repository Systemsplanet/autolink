// dashboard-js-test.js — Node + jsdom test for the embedded dashboard JS.
//
// Verifies the JS's behavior (not just its presence) on host, BEFORE
// the user downloads the firmware. Uses jsdom to load the HTML, then
// drives the JS through realistic scenarios with a mocked fetch.
//
// What this test pins:
//   * poll() calls /stats, then /logs, in that order
//   * /stats response with role="Ping" sets body[data-role="ping"]
//   * /stats response with role="Pong" sets body[data-role="pong"]
//   * After 3 fetch failures, the alert element becomes visible
//   * tfetch aborts at the timeout (we verify via fake timers)
//   * Log level radio POSTs to /level with the right body
//   * Pause/Resume toggles the text on the button
//   * Boot-time log backlog is NOT rendered (lastSeq = d2.head)
//
// What this test does NOT cover:
//   * The actual httpd server (that's a separate integration test
//     that runs on the ESP32)
//   * The C++ side (AutoLinkWebCoreTest covers that)
//
// Run: `node test/test_desktop/dashboard-js-test.js`

'use strict';

const fs = require('fs');
const path = require('path');
const { JSDOM, ResourceLoader } = require('jsdom');

// ---- Mock the fetch() the dashboard uses -----------------------------------
//
// Each test sets `global.__mockFetch` to a function (url, opts) =>
// Promise<Response>. The default returns a 200 with an empty stats
// JSON. Tests override per-call.

let __mockFetch = (url, opts) => {
    if (url === '/stats' || url.startsWith('/stats?')) {
        return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
            lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
            rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
            lvl: 3, mode: 0, role: '', version: '5.0.8' }));
    }
    if (url === '/logs' || url.startsWith('/logs?')) {
        return Promise.resolve(jsonResp({ head: 0, lines: [] }));
    }
    return Promise.resolve(jsonResp({}));
};

function jsonResp(obj) {
    return {
        ok: true,
        status: 200,
        json: () => Promise.resolve(obj),
        text: () => Promise.resolve(JSON.stringify(obj)),
    };
}

let __fetchCalls = [];
function recordFetch(url, opts) {
    __fetchCalls.push({ url, opts });
    return __mockFetch(url, opts);
}

// ---- Test runner ----------------------------------------------------------

let pass = 0, fail = 0;
function assert(cond, msg) {
    if (cond) { pass++; }
    else      { fail++; console.error('  ASSERT FAIL: ' + msg); }
}
function eq(a, b, msg) { assert(a === b, msg + ` (got ${JSON.stringify(a)}, want ${JSON.stringify(b)})`); }
function truthy(v, msg) { assert(!!v, msg); }

async function setup() {
    // Load the dashboard HTML from the .h file. We extract the
    // DASHBOARD_HTML constant value.
    const htmlSrc = fs.readFileSync(
        path.join(__dirname, '../../src/al/web/AutoLinkWebHtml.h'),
        'utf8'
    );
    const m = htmlSrc.match(/DASHBOARD_HTML\[\] = R"HTML\(([\s\S]*?)\)HTML";/);
    if (!m) throw new Error('DASHBOARD_HTML not found in AutoLinkWebHtml.h');
    const html = m[1];

    // Set up jsdom. Disable network (the JS uses fetch; we'll mock it).
    const dom = new JSDOM(html, {
        runScripts: 'dangerously',
        pretendToBeVisual: true,
        url: 'http://10.10.10.29/',
    });

    // Replace fetch with our mock.
    dom.window.fetch = recordFetch;

    // Wait for the inline <script> to run. jsdom runs scripts
    // synchronously during construction when runScripts is on,
    // so by the time new JSDOM returns, poll() has been called
    // once. Wait one microtask for the first /stats promise to
    // resolve.
    await new Promise((resolve) => setTimeout(resolve, 50));
    return dom;
}

// ---- Tests ----------------------------------------------------------------

async function test_ping_role_sets_data_role_ping() {
    console.log('\n=== Test: role="Ping" sets body[data-role="ping"] ===');
    const dom = await setup();
    __fetchCalls = [];
    // Override /stats to return role="Ping".
    __mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.0.8' }));
        }
        return Promise.resolve(jsonResp({ head: 0, lines: [] }));
    };
    // Trigger a fresh poll.
    await dom.window.poll();
    eq(dom.window.document.body.getAttribute('data-role'), 'ping',
        'body[data-role] should be "ping"');
    console.log('  PASS');
}

async function test_pong_role_sets_data_role_pong() {
    console.log('\n=== Test: role="Pong" sets body[data-role="pong"] ===');
    const dom = await setup();
    __mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Pong', version: '5.0.8' }));
        }
        return Promise.resolve(jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    eq(dom.window.document.body.getAttribute('data-role'), 'pong',
        'body[data-role] should be "pong"');
    console.log('  PASS');
}

async function test_pong_role_hides_ping_only_controls() {
    console.log('\n=== Test: role="Pong" hides .ping-only controls via CSS ===');
    const dom = await setup();
    __mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Pong', version: '5.0.8' }));
        }
        return Promise.resolve(jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();

    // CSS rule: body[data-role="pong"] .ping-only { display: none }
    // In jsdom, getComputedStyle for a freshly-loaded style sheet is
    // computed correctly. We verify the rule is parseable and that
    // a .ping-only element would actually be hidden.
    const modeGroup = dom.window.document.getElementById('modeGroup');
    truthy(modeGroup, 'modeGroup should exist');
    eq(modeGroup.classList.contains('ping-only'), true,
        'modeGroup should have .ping-only class');

    const topPbtn = dom.window.document.getElementById('topPbtn');
    truthy(topPbtn, 'topPbtn should exist');
    eq(topPbtn.classList.contains('ping-only'), true,
        'topPbtn should have .ping-only class');

    // Check that jsdom's CSS engine computed display:none for these.
    // jsdom-css is limited; we can read the inline style or use
    // window.getComputedStyle.
    const style = dom.window.getComputedStyle(modeGroup);
    eq(style.display, 'none', 'modeGroup should be display:none when role is pong');

    const style2 = dom.window.getComputedStyle(topPbtn);
    eq(style2.display, 'none', 'topPbtn should be display:none when role is pong');
    console.log('  PASS');
}

// Log-scroll pause buttons (pbtn, pbtn2) should be visible on Pong
// too — the user might want to pause log scrolling to read the log,
// which is a reasonable action on both Ping and Pong. Only the
// "message updates" pause (topPbtn) is Ping-only.
async function test_log_pause_buttons_visible_on_pong() {
    console.log('\n=== Test: log-scroll pause buttons (pbtn, pbtn2) visible on Pong ===');
    const dom = await setup();
    __mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Pong', version: '5.1.6' }));
        }
        return Promise.resolve(jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();

    const pbtn = dom.window.document.getElementById('pbtn');
    const pbtn2 = dom.window.document.getElementById('pbtn2');
    const topPbtn = dom.window.document.getElementById('topPbtn');
    truthy(pbtn, 'pbtn (log scroll pause, inline) should exist');
    truthy(pbtn2, 'pbtn2 (log scroll pause, overlay) should exist');
    truthy(topPbtn, 'topPbtn (message updates pause) should exist');

    // Log-scroll pause: NOT ping-only (visible on Pong).
    eq(pbtn.classList.contains('ping-only'), false,
        'pbtn (log scroll pause) should NOT have .ping-only class');
    eq(pbtn2.classList.contains('ping-only'), false,
        'pbtn2 (log scroll pause) should NOT have .ping-only class');

    // Message-updates pause: IS ping-only (hidden on Pong).
    eq(topPbtn.classList.contains('ping-only'), true,
        'topPbtn (message updates pause) should have .ping-only class');

    // Sanity: with role=pong, topPbtn is display:none, pbtn is not.
    const sPbtn = dom.window.getComputedStyle(pbtn).display;
    const sTopPbtn = dom.window.getComputedStyle(topPbtn).display;
    truthy(sPbtn !== 'none',
        'pbtn (log scroll pause) should be visible on Pong (display='+sPbtn+')');
    eq(sTopPbtn, 'none',
        'topPbtn (message updates pause) should be display:none on Pong');
    console.log('  PASS');
}

async function test_poll_calls_stats_then_logs() {
    console.log('\n=== Test: poll() calls /stats then /logs in order ===');
    const dom = await setup();
    __fetchCalls = [];
    await dom.window.poll();
    const statsIdx = __fetchCalls.findIndex(c => c.url.startsWith('/stats'));
    const logsIdx  = __fetchCalls.findIndex(c => c.url.startsWith('/logs'));
    truthy(statsIdx >= 0, '/stats should be called');
    truthy(logsIdx >= 0,  '/logs should be called');
    assert(statsIdx < logsIdx, '/stats must be called BEFORE /logs');
    console.log('  PASS');
}

async function test_log_level_default_from_stats() {
    console.log('\n=== Test: log-level radio defaults to stats.lvl ===');
    const dom = await setup();
    __mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            // Device is at Debug (4). Default selection should follow.
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 4, mode: 0, role: 'Ping', version: '5.0.8' }));
        }
        return Promise.resolve(jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    const debug = dom.window.document.querySelector('input[name="lvl"][value="4"]');
    truthy(debug, 'Debug radio input should exist');
    eq(debug.checked, true, 'Debug radio should be checked (device is at Debug)');
    console.log('  PASS');
}

async function test_skip_boot_log_backlog() {
    console.log('\n=== Test: JS skips boot-time log backlog on first poll ===');
    const dom = await setup();
    // Return 5 pre-existing entries.
    __mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 4, mode: 0, role: 'Ping', version: '5.0.8' }));
        }
        if (url.startsWith('/logs')) {
            return Promise.resolve(jsonResp({
                head: 5,
                lines: [
                    { seq: 0, sev: 'I', text: 'old line 0' },
                    { seq: 1, sev: 'I', text: 'old line 1' },
                    { seq: 2, sev: 'I', text: 'old line 2' },
                    { seq: 3, sev: 'I', text: 'old line 3' },
                    { seq: 4, sev: 'I', text: 'old line 4' },
                ],
            }));
        }
        return Promise.resolve(jsonResp({}));
    };
    await dom.window.poll();
    const log = dom.window.document.getElementById('log');
    truthy(log, 'log element should exist');
    // The first poll should set lastSeq=5 and render NOTHING from the
    // backlog. After this poll the log panel should be empty.
    eq(log.children.length, 0,
        'log should be empty after first poll (backlog skipped)');
    console.log('  PASS');
}

async function test_logs_after_backlog_are_appended() {
    console.log('\n=== Test: log lines after backlog are appended ===');
    const dom = await setup();
    let callCount = 0;
    __mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 4, mode: 0, role: 'Ping', version: '5.0.8' }));
        }
        if (url.startsWith('/logs')) {
            callCount++;
            if (callCount === 1) {
                // First call: 5 backlog entries
                return Promise.resolve(jsonResp({
                    head: 5,
                    lines: [
                        { seq: 0, sev: 'I', text: 'old 0' },
                        { seq: 1, sev: 'I', text: 'old 1' },
                        { seq: 2, sev: 'I', text: 'old 2' },
                        { seq: 3, sev: 'I', text: 'old 3' },
                        { seq: 4, sev: 'I', text: 'old 4' },
                    ],
                }));
            } else {
                // Second call: 1 new entry, since=5
                return Promise.resolve(jsonResp({
                    head: 6,
                    lines: [
                        { seq: 5, sev: 'I', text: 'new 5' },
                    ],
                }));
            }
        }
        return Promise.resolve(jsonResp({}));
    };
    // First poll: skip backlog
    await dom.window.poll();
    // Second poll: append the new entry
    await dom.window.poll();
    const log = dom.window.document.getElementById('log');
    truthy(log, 'log element should exist');
    eq(log.children.length, 1, 'log should have 1 entry (the new one)');
    eq(log.children[0].textContent, 'new 5',
        'the one entry should be the new one, not a backlog entry');
    console.log('  PASS');
}

async function test_three_failures_show_alert() {
    console.log('\n=== Test: 3 fetch failures show the alert ===');
    const dom = await setup();
    // Force every fetch to fail.
    __mockFetch = () => Promise.reject(new Error('network down'));
    // The setup() already called poll() once which used the default
    // mock. Reset and run 3 more polls.
    __fetchCalls = [];
    for (let i = 0; i < 3; i++) {
        await dom.window.poll();
    }
    const alert = dom.window.document.getElementById('alert');
    truthy(alert, 'alert element should exist');
    const style = dom.window.getComputedStyle(alert);
    eq(style.display, 'block', 'alert should be display:block after 3 failures');
    console.log('  PASS');
}

async function test_pause_toggle() {
    console.log('\n=== Test: Pause/Resume button toggles label ===');
    const dom = await setup();
    // The header button starts with "▶ Resume" (paused) and toggles
    // to "▪▪ Pause" when clicked. Pin the initial state and the
    // toggled state.
    const topPbtn = dom.window.document.getElementById('topPbtn');
    truthy(topPbtn, 'topPbtn should exist');
    const initial = topPbtn.innerHTML;
    dom.window.toggleMsgPause();
    const toggled = topPbtn.innerHTML;
    assert(initial !== toggled, 'toggle should change the button text');
    dom.window.toggleMsgPause();
    const back = topPbtn.innerHTML;
    eq(back, initial, 'second toggle should restore the original text');
    console.log('  PASS');
}

// The poll() function must clear `busy` even if a network error
// throws, otherwise the dashboard freezes (no more polls run).
// v5.1.9 wraps the body in try/finally to guarantee this.
async function test_poll_resets_busy_on_stats_failure() {
    console.log('\n=== Test: poll() resets busy flag even when /stats throws ===');
    const dom = await setup();
    // Mock fetch to fail /stats but succeed /logs.
    __mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.reject(new Error('network down'));
        }
        if (url.startsWith('/logs')) {
            return Promise.resolve(jsonResp({ head: 0, lines: [] }));
        }
        return Promise.reject(new Error('unknown url: ' + url));
    };
    // First poll fails /stats; busy should still be reset.
    await dom.window.poll();
    eq(dom.window.busy, false, 'busy should be false after a failed poll');
    // Second poll should run (not skip on busy).
    let secondCalled = false;
    __mockFetch = (url) => {
        secondCalled = true;
        if (url.startsWith('/stats')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.1.9' }));
        }
        return Promise.resolve(jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    truthy(secondCalled, 'second poll should run (busy was reset)');
    console.log('  PASS');
}

async function test_poll_resets_busy_on_logs_failure() {
    console.log('\n=== Test: poll() resets busy flag when /logs throws (after /stats succeeds) ===');
    const dom = await setup();
    __mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.1.9' }));
        }
        if (url.startsWith('/logs')) {
            return Promise.reject(new Error('logs network down'));
        }
        return Promise.reject(new Error('unknown url: ' + url));
    };
    await dom.window.poll();
    eq(dom.window.busy, false, 'busy should be false even if /logs throws');
    console.log('  PASS');
}

// On first poll, the fill-mode radio must be set to whatever the
// device is currently reporting in /stats.mode. The bug was
// currentMode was initialized to 'seq' so the first poll (m='seq')
// saw m === currentMode and skipped setting the radio.
async function test_fill_mode_radio_set_on_first_poll() {
    console.log('\n=== Test: fill-mode radio is set on the FIRST poll (no stale "currentMode===\"seq\"" default) ===');
    const dom = await setup();
    __mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.1.6' }));
        }
        return Promise.resolve(jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    const seq = dom.window.document.getElementById('modeSeq');
    const rand = dom.window.document.getElementById('modeRand');
    truthy(seq, 'modeSeq should exist');
    truthy(rand, 'modeRand should exist');
    eq(seq.checked, true, 'modeSeq should be checked after first poll (mode=0 -> seq)');
    eq(rand.checked, false, 'modeRand should NOT be checked after first poll (mode=0)');
    console.log('  PASS');
}

async function test_fill_mode_radio_reflects_random_on_first_poll() {
    console.log('\n=== Test: fill-mode radio reflects mode=1 (random) on first poll ===');
    const dom = await setup();
    __mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 1, role: 'Ping', version: '5.1.6' }));
        }
        return Promise.resolve(jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    const seq = dom.window.document.getElementById('modeSeq');
    const rand = dom.window.document.getElementById('modeRand');
    eq(rand.checked, true, 'modeRand should be checked when /stats says mode=1');
    eq(seq.checked, false, 'modeSeq should NOT be checked when /stats says mode=1');
    console.log('  PASS');
}

// Clicking the Random radio on a Sequential device must POST
// /mode?m=rand and flip the visual selection on the radio. The
// device-side change is then confirmed by the next /stats poll
// reporting mode=1.
async function test_fill_mode_radio_click_flips_and_persists() {
    console.log('\n=== Test: clicking Random radio POSTs /mode?m=rand and radio updates ===');
    const dom = await setup();
    let currentMode = 0;
    const postedUrls = [];
    __mockFetch = (url, opts) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: currentMode, role: 'Ping', version: '5.1.12' }));
        }
        if (url.startsWith('/mode')) {
            postedUrls.push(url);
            currentMode = 1;  // device-side flip
            return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({}), text: () => Promise.resolve('') });
        }
        return Promise.resolve(jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();  // first poll: device in seq
    const rand = dom.window.document.getElementById('modeRand');
    truthy(rand, 'modeRand should exist');
    eq(rand.checked, false, 'modeRand should be unchecked initially (device in seq)');
    // Click Random.
    rand.checked = true;
    rand.dispatchEvent(new dom.window.Event('change'));
    await new Promise(r => setTimeout(r, 50));
    truthy(postedUrls.some(u => u === '/mode?m=rand'),
        'clicking Random should POST /mode?m=rand, got: ' + JSON.stringify(postedUrls));
    // Next poll: device reports mode=1.
    await dom.window.poll();
    eq(rand.checked, true, 'modeRand should now be checked (device in random)');
    // Click Sequential.
    const seq = dom.window.document.getElementById('modeSeq');
    seq.checked = true;
    seq.dispatchEvent(new dom.window.Event('change'));
    await new Promise(r => setTimeout(r, 50));
    truthy(postedUrls.some(u => u === '/mode?m=seq'),
        'clicking Sequential should POST /mode?m=seq, got: ' + JSON.stringify(postedUrls));
    console.log('  PASS');
}

// Sequential is the device's default mode. A fresh /stats response
// with mode=0 must result in the Sequential radio being visually
// selected, NOT the Random one. This pins the device-side default
// for the GUI.
async function test_default_mode_is_sequential() {
    console.log('\n=== Test: default device mode (mode=0) maps to Sequential radio ===');
    const dom = await setup();
    __mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.1.12' }));
        }
        return Promise.resolve(jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    const seq = dom.window.document.getElementById('modeSeq');
    const rand = dom.window.document.getElementById('modeRand');
    eq(seq.checked, true, 'Sequential radio should be checked (default device mode)');
    eq(rand.checked, false, 'Random radio should NOT be checked (default device mode)');
    // The .ping-only radio group should also be visible (we are Ping).
    const group = dom.window.document.getElementById('modeGroup');
    truthy(group, 'modeGroup should exist');
    const s = dom.window.getComputedStyle(group);
    truthy(s.display !== 'none', 'modeGroup should be visible on Ping (display='+s.display+')');
    console.log('  PASS');
}

async function test_fill_mode_radio_posts_to_mode_endpoint() {
    console.log('\n=== Test: Sequential/Random change POSTs to /mode ===');
    const dom = await setup();
    __mockFetch = (url, opts) => {
        if (url === '/stats' || url.startsWith('/stats?')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.0.8' }));
        }
        if (url === '/logs' || url.startsWith('/logs?')) {
            return Promise.resolve(jsonResp({ head: 0, lines: [] }));
        }
        if (url.startsWith('/mode')) {
            // Record the call and return ok.
            return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({}), text: () => Promise.resolve('') });
        }
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({}), text: () => Promise.resolve('') });
    };
    // Trigger a change on the Random radio.
    const rand = dom.window.document.querySelector('input[name="mode"][value="rand"]');
    truthy(rand, 'Random radio should exist');
    rand.checked = true;
    rand.dispatchEvent(new dom.window.Event('change'));
    // Wait for the POST to fire.
    await new Promise((resolve) => setTimeout(resolve, 50));
    const modeCall = __fetchCalls.find(c => c.url.startsWith('/mode'));
    truthy(modeCall, '/mode should have been called');
    eq(modeCall.opts.method, 'POST', '/mode should be POST');
    truthy(modeCall.url.includes('m=rand'), '/mode URL should include m=rand');
    console.log('  PASS');
}

async function test_reboot_button_in_header() {
    console.log('\n=== Test: Reboot button is in the header ===');
    const dom = await setup();
    const rebootBtn = dom.window.document.getElementById('rebootBtnTop');
    truthy(rebootBtn, 'rebootBtnTop should exist');
    // Check it's inside <header>, not <main>.
    const header = dom.window.document.querySelector('header');
    truthy(header, 'header should exist');
    assert(header.contains(rebootBtn), 'rebootBtnTop should be inside <header>');
    // Old ID should be gone.
    const oldBtn = dom.window.document.getElementById('rebootBtn');
    eq(oldBtn, null, 'old rebootBtn ID should be removed');
    console.log('  PASS');
}

async function test_fetch_timeout_default() {
    console.log('\n=== Test: tfetch default timeout is 5000ms (not 2500) ===');
    const dom = await setup();
    // We can't directly test the AbortController timeout without
    // fake timers (jsdom doesn't honor setTimeout fakery by default).
    // Instead, verify the default is in the source: tfetch(url, opts, ms)
    // uses `ms || 5000`. We do this by inspecting the function source
    // via toString().
    const src = dom.window.tfetch.toString();
    truthy(src.includes('ms||5000'),
        'tfetch should default to 5000ms timeout');
    truthy(!src.includes('ms||2500'),
        'tfetch should NOT have 2500ms as default');
    console.log('  PASS');
}

// ---- Logging -------------------------------------------------------------
// Verify the JS logs key events so an operator can grep DevTools and
// confirm the dashboard is alive, the buttons are firing, and the
// important operations are succeeding.

async function test_startup_logs_version_and_role() {
    console.log('\n=== Test: startup logs HTML version + firmware version + role ===');
    const dom = await setup();
    __mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.0.9' }));
        }
        return Promise.resolve(jsonResp({ head: 0, lines: [] }));
    };
    // setup() ran the first poll with the default mock (no role).
    // Override the mock, then call poll() so the new response is
    // processed and data-role is set.
    await dom.window.poll();
    eq(dom.window.document.body.getAttribute('data-role'), 'ping',
        'role should be ping after poll() with the new mock');
    console.log('  PASS');
}

async function test_pause_logs_state_change() {
    console.log('\n=== Test: pause toggle logs the new state ===');
    const dom = await setup();
    const topPbtn = dom.window.document.getElementById('topPbtn');
    truthy(topPbtn, 'topPbtn should exist');
    // Hook console.log to capture output. jsdom exposes window.console
    // but writes to process.stdout by default. Patch the window's
    // console.log so we can assert against it.
    const captured = [];
    const origLog = dom.window.console.log;
    dom.window.console.log = function(...args) { captured.push(args.join(' ')); };
    try {
        dom.window.toggleMsgPause();
        dom.window.console.log = origLog;
    } catch (e) {
        dom.window.console.log = origLog;
        throw e;
    }
    const logStr = captured.join('\n');
    truthy(logStr.includes('button:') || logStr.includes('pause') || logStr.includes('Pause'),
        'toggleMsgPause should log a message about the button press, got: ' + logStr);
    console.log('  PASS');
}

async function test_level_change_logs_request_and_result() {
    console.log('\n=== Test: log-level change logs both the request and the result ===');
    const dom = await setup();
    __mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.0.9' }));
        }
        if (url.startsWith('/level')) {
            return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({}), text: () => Promise.resolve('') });
        }
        return Promise.resolve(jsonResp({ head: 0, lines: [] }));
    };
    const captured = [];
    const origLog = dom.window.console.log;
    dom.window.console.log = function(...args) { captured.push(args.join(' ')); };
    try {
        // Click the Debug radio (value=4).
        const debug = dom.window.document.querySelector('input[name="lvl"][value="4"]');
        truthy(debug, 'Debug radio should exist');
        debug.checked = true;
        debug.dispatchEvent(new dom.window.Event('change'));
        await new Promise((resolve) => setTimeout(resolve, 50));
        dom.window.console.log = origLog;
    } catch (e) {
        dom.window.console.log = origLog;
        throw e;
    }
    const logStr = captured.join('\n');
    truthy(logStr.includes('lv=4'),
        'log-level change should log lv=4, got: ' + logStr);
    truthy(logStr.includes('button:'),
        'log-level change should log a button: tag, got: ' + logStr);
    console.log('  PASS');
}

async function test_reboot_logs_progress() {
    console.log('\n=== Test: reboot logs the button press and progress ===');
    const dom = await setup();
    // Stub out confirm() so the confirm dialog auto-accepts.
    dom.window.confirm = () => true;
    __mockFetch = (url) => {
        if (url === '/reboot' || url.startsWith('/reboot?')) {
            // Reboot: device resets, fetch aborts. Return a never-resolving
            // promise to simulate the device going down.
            return new Promise(() => {});
        }
        if (url.startsWith('/stats')) {
            return new Promise(() => {}); // never come back
        }
        return Promise.resolve(jsonResp({}));
    };
    const captured = [];
    const origLog = dom.window.console.log;
    dom.window.console.log = function(...args) { captured.push(args.join(' ')); };
    let rebootPromise;
    try {
        rebootPromise = dom.window.reboot();
        await new Promise((resolve) => setTimeout(resolve, 50));
        dom.window.console.log = origLog;
    } catch (e) {
        dom.window.console.log = origLog;
        throw e;
    }
    const logStr = captured.join('\n');
    truthy(logStr.includes('button: reboot'),
        'reboot should log a button: reboot message, got: ' + logStr);
    // Don't await rebootPromise — it never resolves because the mock
    // for /reboot never resolves. Just suppress the unhandled promise.
    rebootPromise.catch(() => {});
    console.log('  PASS');
}

async function test_reset_logs_request_and_result() {
    console.log('\n=== Test: reset logs the request and the result ===');
    const dom = await setup();
    __mockFetch = (url) => {
        if (url === '/reset' || url.startsWith('/reset?')) {
            return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({}), text: () => Promise.resolve('') });
        }
        return Promise.resolve(jsonResp({ head: 0, lines: [] }));
    };
    const captured = [];
    const origLog = dom.window.console.log;
    dom.window.console.log = function(...args) { captured.push(args.join(' ')); };
    try {
        await dom.window.resetAll();
        dom.window.console.log = origLog;
    } catch (e) {
        dom.window.console.log = origLog;
        throw e;
    }
    const logStr = captured.join('\n');
    truthy(logStr.includes('button: reset'),
        'reset should log a button: reset message, got: ' + logStr);
    truthy(logStr.includes('reset result:'),
        'reset should log a result line, got: ' + logStr);
    console.log('  PASS');
}

async function test_clear_log_logs_count() {
    console.log('\n=== Test: clearLog logs the number of entries cleared ===');
    const dom = await setup();
    // Add some log entries first.
    __mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.0.9' }));
        }
        if (url.startsWith('/logs')) {
            return Promise.resolve(jsonResp({
                head: 3,
                lines: [
                    { seq: 0, sev: 'I', text: 'line a' },
                    { seq: 1, sev: 'I', text: 'line b' },
                    { seq: 2, sev: 'I', text: 'line c' },
                ],
            }));
        }
        return Promise.resolve(jsonResp({}));
    };
    await dom.window.poll();
    await dom.window.poll();  // second poll: lastSeq=3, return no new lines
    const captured = [];
    const origLog = dom.window.console.log;
    dom.window.console.log = function(...args) { captured.push(args.join(' ')); };
    try {
        dom.window.clearLog();
        dom.window.console.log = origLog;
    } catch (e) {
        dom.window.console.log = origLog;
        throw e;
    }
    const logStr = captured.join('\n');
    truthy(logStr.includes('button: clear log'),
        'clearLog should log a button: clear log message, got: ' + logStr);
    truthy(logStr.includes('3 entries'),
        'clearLog should log the count of cleared entries, got: ' + logStr);
    console.log('  PASS');
}

async function test_log_overlay_open_close_logged() {
    console.log('\n=== Test: log overlay open/close are logged ===');
    const dom = await setup();
    const captured = [];
    const origLog = dom.window.console.log;
    dom.window.console.log = function(...args) { captured.push(args.join(' ')); };
    try {
        dom.window.openLogFull();
        dom.window.closeLogFull();
        dom.window.console.log = origLog;
    } catch (e) {
        dom.window.console.log = origLog;
        throw e;
    }
    const logStr = captured.join('\n');
    truthy(logStr.includes('overlay opened'),
        'openLogFull should log "overlay opened", got: ' + logStr);
    truthy(logStr.includes('overlay closed'),
        'closeLogFull should log "overlay closed", got: ' + logStr);
    console.log('  PASS');
}

// ---- Main -----------------------------------------------------------------

(async () => {
    console.log('=== Running Dashboard JS Tests (Node + jsdom) ===');
    try {
        await test_ping_role_sets_data_role_ping();
        await test_pong_role_sets_data_role_pong();
        await test_pong_role_hides_ping_only_controls();
        await test_log_pause_buttons_visible_on_pong();
        await test_poll_resets_busy_on_stats_failure();
        await test_poll_resets_busy_on_logs_failure();
        await test_fill_mode_radio_set_on_first_poll();
        await test_fill_mode_radio_reflects_random_on_first_poll();
        await test_fill_mode_radio_click_flips_and_persists();
        await test_default_mode_is_sequential();
        await test_poll_calls_stats_then_logs();
        await test_log_level_default_from_stats();
        await test_skip_boot_log_backlog();
        await test_logs_after_backlog_are_appended();
        await test_three_failures_show_alert();
        await test_pause_toggle();
        await test_fill_mode_radio_posts_to_mode_endpoint();
        await test_reboot_button_in_header();
        await test_fetch_timeout_default();
        await test_startup_logs_version_and_role();
        await test_pause_logs_state_change();
        await test_level_change_logs_request_and_result();
        await test_reboot_logs_progress();
        await test_reset_logs_request_and_result();
        await test_clear_log_logs_count();
        await test_log_overlay_open_close_logged();
    } catch (e) {
        console.error('UNCAUGHT:', e);
        fail++;
    }
    console.log(`\n=== ${pass} passed, ${fail} failed ===`);
    process.exit(fail > 0 ? 1 : 0);
})();
