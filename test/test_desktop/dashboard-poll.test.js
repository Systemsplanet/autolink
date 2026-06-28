// dashboard-poll.test.js -- poll-cycle behavior:
// /stats then /logs ordering, backlog skip on first
// poll, busy flag reset, fill-mode radio state.
//
// Imports the shared harness from
// dashboard-test-harness.js.

'use strict';

const h = require('./dashboard-test-harness');

async function test_poll_calls_stats_then_logs() {
    console.log('\n=== Test: poll() calls /stats then /logs in order ===');
    const dom = await h.setup();
    h.resetFetchCalls();
    await dom.window.poll();
    const statsIdx = h.fetchCalls().findIndex(c => c.url.startsWith('/stats'));
    const logsIdx  = h.fetchCalls().findIndex(c => c.url.startsWith('/logs'));
    h.truthy(statsIdx >= 0, '/stats should be called');
    h.truthy(logsIdx >= 0,  '/logs should be called');
    h.assert(statsIdx < logsIdx, '/stats must be called BEFORE /logs');
    console.log('  PASS');
}

async function test_log_level_default_from_stats() {
    console.log('\n=== Test: log-level radio defaults to stats.lvl ===');
    const dom = await h.setup();
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 4, mode: 0, role: 'Ping', version: '5.0.8' }));
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    const debug = dom.window.document.querySelector('input[name="lvl"][value="4"]');
    h.truthy(debug, 'Debug radio input should exist');
    h.eq(debug.checked, true, 'Debug radio should be checked (device is at Debug)');
    console.log('  PASS');
}

async function test_skip_boot_log_backlog() {
    console.log('\n=== Test: JS skips boot-time log backlog on first poll ===');
    const dom = await h.setup();
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 4, mode: 0, role: 'Ping', version: '5.0.8' }));
        }
        if (url.startsWith('/logs')) {
            return Promise.resolve(h.jsonResp({
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
        return Promise.resolve(h.jsonResp({}));
    };
    await dom.window.poll();
    const log = dom.window.document.getElementById('log');
    h.truthy(log, 'log element should exist');
    h.eq(log.children.length, 0,
        'log should be empty after first poll (backlog skipped)');
    console.log('  PASS');
}

async function test_logs_after_backlog_are_appended() {
    console.log('\n=== Test: log lines after backlog are appended ===');
    const dom = await h.setup();
    let callCount = 0;
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 4, mode: 0, role: 'Ping', version: '5.0.8' }));
        }
        if (url.startsWith('/logs')) {
            callCount++;
            if (callCount === 1) {
                return Promise.resolve(h.jsonResp({
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
                return Promise.resolve(h.jsonResp({
                    head: 6,
                    lines: [
                        { seq: 5, sev: 'I', text: 'new 5' },
                    ],
                }));
            }
        }
        return Promise.resolve(h.jsonResp({}));
    };
    await dom.window.poll();
    await dom.window.poll();
    const log = dom.window.document.getElementById('log');
    h.truthy(log, 'log element should exist');
    h.eq(log.children.length, 1, 'log should have 1 entry (the new one)');
    h.eq(log.children[0].textContent, 'new 5',
        'the one entry should be the new one, not a backlog entry');
    console.log('  PASS');
}

async function test_three_failures_show_alert() {
    console.log('\n=== Test: 3 fetch failures show the alert ===');
    const dom = await h.setup();
    h.__mockFetch = () => Promise.reject(new Error('network down'));
    h.resetFetchCalls();
    for (let i = 0; i < 3; i++) {
        await dom.window.poll();
    }
    const alert = dom.window.document.getElementById('alert');
    h.truthy(alert, 'alert element should exist');
    const style = dom.window.getComputedStyle(alert);
    h.eq(style.display, 'block', 'alert should be display:block after 3 failures');
    console.log('  PASS');
}

async function test_poll_resets_busy_on_stats_failure() {
    console.log('\n=== Test: poll() resets busy flag even when /stats throws ===');
    const dom = await h.setup();
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.reject(new Error('network down'));
        }
        if (url.startsWith('/logs')) {
            return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
        }
        return Promise.reject(new Error('unknown url: ' + url));
    };
    await dom.window.poll();
    h.eq(dom.window.busy, false, 'busy should be false after a failed poll');
    let secondCalled = false;
    h.__mockFetch = (url) => {
        secondCalled = true;
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.1.9' }));
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    h.truthy(secondCalled, 'second poll should run (busy was reset)');
    console.log('  PASS');
}

async function test_poll_resets_busy_on_logs_failure() {
    console.log('\n=== Test: poll() resets busy flag when /logs throws (after /stats succeeds) ===');
    const dom = await h.setup();
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
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
    h.eq(dom.window.busy, false, 'busy should be false even if /logs throws');
    console.log('  PASS');
}

async function test_fill_mode_radio_set_on_first_poll() {
    console.log('\n=== Test: fill-mode radio is set on the FIRST poll (no stale "currentMode===\"seq\"" default) ===');
    const dom = await h.setup();
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.1.6' }));
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    const seq = dom.window.document.getElementById('modeSeq');
    const rand = dom.window.document.getElementById('modeRand');
    h.truthy(seq, 'modeSeq should exist');
    h.truthy(rand, 'modeRand should exist');
    h.eq(seq.checked, true, 'modeSeq should be checked after first poll (mode=0 -> seq)');
    h.eq(rand.checked, false, 'modeRand should NOT be checked after first poll (mode=0)');
    console.log('  PASS');
}

async function test_fill_mode_radio_reflects_random_on_first_poll() {
    console.log('\n=== Test: fill-mode radio reflects mode=1 (random) on first poll ===');
    const dom = await h.setup();
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 1, role: 'Ping', version: '5.1.6' }));
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    const seq = dom.window.document.getElementById('modeSeq');
    const rand = dom.window.document.getElementById('modeRand');
    h.eq(rand.checked, true, 'modeRand should be checked when /stats says mode=1');
    h.eq(seq.checked, false, 'modeSeq should NOT be checked when /stats says mode=1');
    console.log('  PASS');
}

async function test_fill_mode_radio_click_flips_and_persists() {
    console.log('\n=== Test: clicking Random radio POSTs /mode?m=rand and radio updates ===');
    const dom = await h.setup();
    let currentMode = 0;
    const postedUrls = [];
    h.__mockFetch = (url, opts) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: currentMode, role: 'Ping', version: '5.1.12' }));
        }
        if (url.startsWith('/mode')) {
            postedUrls.push(url);
            currentMode = 1;
            return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({}), text: () => Promise.resolve('') });
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    const rand = dom.window.document.getElementById('modeRand');
    h.truthy(rand, 'modeRand should exist');
    h.eq(rand.checked, false, 'modeRand should be unchecked initially (device in seq)');
    rand.checked = true;
    rand.dispatchEvent(new dom.window.Event('change'));
    await new Promise(r => setTimeout(r, 50));
    h.truthy(postedUrls.some(u => u === '/mode?m=rand'),
        'clicking Random should POST /mode?m=rand, got: ' + JSON.stringify(postedUrls));
    await dom.window.poll();
    h.eq(rand.checked, true, 'modeRand should now be checked (device in random)');
    const seq = dom.window.document.getElementById('modeSeq');
    seq.checked = true;
    seq.dispatchEvent(new dom.window.Event('change'));
    await new Promise(r => setTimeout(r, 50));
    h.truthy(postedUrls.some(u => u === '/mode?m=seq'),
        'clicking Sequential should POST /mode?m=seq, got: ' + JSON.stringify(postedUrls));
    console.log('  PASS');
}

async function test_fill_mode_radio_posts_to_mode_endpoint() {
    console.log('\n=== Test: Sequential/Random change POSTs to /mode ===');
    const dom = await h.setup();
    h.__mockFetch = (url, opts) => {
        if (url === '/stats' || url.startsWith('/stats?')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.0.8' }));
        }
        if (url === '/logs' || url.startsWith('/logs?')) {
            return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
        }
        if (url.startsWith('/mode')) {
            return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({}), text: () => Promise.resolve('') });
        }
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({}), text: () => Promise.resolve('') });
    };
    const rand = dom.window.document.querySelector('input[name="mode"][value="rand"]');
    h.truthy(rand, 'Random radio should exist');
    rand.checked = true;
    rand.dispatchEvent(new dom.window.Event('change'));
    await new Promise((resolve) => setTimeout(resolve, 50));
    const modeCall = h.fetchCalls().find(c => c.url.startsWith('/mode'));
    h.truthy(modeCall, '/mode should have been called');
    h.eq(modeCall.opts.method, 'POST', '/mode should be POST');
    h.truthy(modeCall.url.includes('m=rand'), '/mode URL should include m=rand');
    console.log('  PASS');
}

async function test_startup_logs_version_and_role() {
    console.log('\n=== Test: startup logs HTML version + firmware version + role ===');
    const dom = await h.setup();
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.0.9' }));
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    h.eq(dom.window.document.body.getAttribute('data-role'), 'ping',
        'role should be ping after poll() with the new mock');
    console.log('  PASS');
}

(async () => {
    try {
        await test_poll_calls_stats_then_logs();
        await test_log_level_default_from_stats();
        await test_skip_boot_log_backlog();
        await test_logs_after_backlog_are_appended();
        await test_three_failures_show_alert();
        await test_poll_resets_busy_on_stats_failure();
        await test_poll_resets_busy_on_logs_failure();
        await test_fill_mode_radio_set_on_first_poll();
        await test_fill_mode_radio_reflects_random_on_first_poll();
        await test_fill_mode_radio_click_flips_and_persists();
        await test_default_mode_is_sequential();
        await test_fill_mode_radio_posts_to_mode_endpoint();
        await test_startup_logs_version_and_role();
    } catch (e) {
        console.error('UNCAUGHT:', e);
    }
    h.summary('dashboard-poll');
})();
