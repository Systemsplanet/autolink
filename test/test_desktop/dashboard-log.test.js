// dashboard-log.test.js -- log/msg-pause/copy/save/reset
// behavior: msgPaused init, copy/save unix-line output,
// pause toggle device POSTs, log-overlay open/close,
// level-change and reset logging, fallbackCopy labels.
//
// Imports the shared harness from
// dashboard-test-harness.js.

'use strict';

const h = require('./dashboard-test-harness');
const fs = require('fs');
const path = require('path');

async function test_msgPaused_starts_true() {
    console.log('\n=== Test: msgPaused starts true (no messages at boot) ===');
    const dom = await h.setup();
    h.eq(dom.window.msgPaused, true,
        'msgPaused global must init true (matches firmware paused_=true)');
    const topPbtn = dom.window.document.getElementById('topPbtn');
    const text = topPbtn.innerHTML;
    h.truthy(text.indexOf('Start') !== -1,
        'button should render "Start" at boot, got: ' + text);
    console.log('  PASS');
}

async function test_copy_save_emit_unix_newlines() {
    console.log('\n=== Test: Copy/Save emit unix-line-delimited output ===');
    const dom = await h.setup();
    const log = dom.window.document.getElementById('log');
    for (const txt of ['line one', 'line two', 'line three']) {
        const d = dom.window.document.createElement('div');
        d.className = 'I';
        d.textContent = txt;
        log.appendChild(d);
    }
    h.assert(typeof dom.window.al_logAsText === 'function',
        'al_logAsText should be a global function');
    const out = dom.window.al_logAsText();
    h.eq(out.charAt(out.length - 1), '\n',
        'output must end with a single trailing newline');
    h.eq(out, 'line one\nline two\nline three\n',
        'lines must be joined with \\n and end with trailing \\n');
    log.innerHTML = '';
    h.eq(dom.window.al_logAsText(), '',
        'empty log should return empty string (no trailing \\n either)');
    console.log('  PASS');
}

async function test_copy_save_no_longer_use_textcontent() {
    console.log('\n=== Test: copyLog/saveLog do NOT read parent.textContent ===');
    const src = fs.readFileSync(
        path.join(__dirname, '../../src/al/web/dashboard.js'),
        'utf8'
    );
    h.truthy(src.indexOf('function copyLog') !== -1, 'copyLog function exists');
    h.truthy(src.indexOf('function saveLog') !== -1, 'saveLog function exists');
    h.truthy(src.indexOf('function al_logAsText') !== -1,
        'al_logAsText helper exists (the source of truth)');
    const copyFn = src.match(/function copyLog\(\)\{[\s\S]*?\n\}/);
    h.truthy(copyFn, 'copyLog function body parseable');
    h.truthy(copyFn[0].indexOf('al_logAsText') !== -1,
        'copyLog must call al_logAsText() — found: ' + copyFn[0].slice(0, 80));
    const saveFn = src.match(/function saveLog\(\)\{[\s\S]*?\n\}/);
    h.truthy(saveFn, 'saveLog function body parseable');
    h.truthy(saveFn[0].indexOf('al_logAsText') !== -1,
        'saveLog must call al_logAsText() — found: ' + saveFn[0].slice(0, 80));
    console.log('  PASS');
}

async function test_pause_toggle() {
    console.log('\n=== Test: Pause/Start button toggles label ===');
    const dom = await h.setup();
    dom.window.deviceRole = 'Ping';
    const topPbtn = dom.window.document.getElementById('topPbtn');
    h.truthy(topPbtn, 'topPbtn should exist');
    const initial = topPbtn.innerHTML;
    h.truthy(initial.indexOf('Start') !== -1,
        'button should start as "Start" (device is paused at boot), got: ' + initial);
    dom.window.toggleMsgPause();
    const toggled = topPbtn.innerHTML;
    h.truthy(toggled.indexOf('Pause') !== -1,
        'first toggle (Start -> Pause) should show "Pause", got: ' + toggled);
    h.assert(initial !== toggled, 'toggle should change the button text');
    dom.window.toggleMsgPause();
    const back = topPbtn.innerHTML;
    h.eq(back, initial, 'second toggle should restore the original text');
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

async function test_pause_logs_state_change() {
    console.log('\n=== Test: pause toggle logs the new state ===');
    const dom = await h.setup();
    dom.window.deviceRole = 'Ping';
    const topPbtn = dom.window.document.getElementById('topPbtn');
    h.truthy(topPbtn, 'topPbtn should exist');
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
    h.truthy(logStr.includes('button:') || logStr.includes('pause') || logStr.includes('Pause'),
        'toggleMsgPause should log a message about the button press, got: ' + logStr);
    console.log('  PASS');
}

async function test_pause_toggle_posts_to_pausemsg_endpoint() {
    console.log('\n=== Test: pause toggle POSTs /pausemsg (device-side pause) ===');
    const dom = await h.setup();
    const calls = [];
    h.__mockFetch = (url, opts) => {
        calls.push({ url: url, opts: opts });
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.1.33' }));
        }
        if (url.startsWith('/pausemsg')) {
            return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({}), text: () => Promise.resolve('ok') });
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    h.eq(dom.window.deviceRole, 'Ping',
        'deviceRole should be Ping after poll() (was ' + dom.window.deviceRole + ')');
    calls.length = 0;
    await dom.window.toggleMsgPause();
    await new Promise(r => setTimeout(r, 20));
    const pauseCalls = calls.filter(c => c.url.startsWith('/pausemsg'));
    h.eq(pauseCalls.length, 1,
        'toggleMsgPause should issue exactly one /pausemsg POST, got ' + pauseCalls.length);
    h.eq(pauseCalls[0].url, '/pausemsg?p=0',
        'first click (Start) should send p=0 (resume sending), got ' + pauseCalls[0].url);
    h.eq(pauseCalls[0].opts && pauseCalls[0].opts.method, 'POST',
        'should be POST, got ' + JSON.stringify(pauseCalls[0].opts));

    calls.length = 0;
    await dom.window.toggleMsgPause();
    await new Promise(r => setTimeout(r, 20));
    const resumeCalls = calls.filter(c => c.url.startsWith('/pausemsg'));
    h.eq(resumeCalls.length, 1,
        'second toggle should issue another /pausemsg POST, got ' + resumeCalls.length);
    h.eq(resumeCalls[0].url, '/pausemsg?p=1',
        'second click (Pause) should send p=1 (pause sending), got ' + resumeCalls[0].url);

    console.log('  PASS');
}

async function test_pause_toggle_noop_on_pong() {
    console.log('\n=== Test: pause toggle is a no-op on Pong (no-flicker) ===');
    const dom = await h.setup();
    const calls = [];
    h.__mockFetch = (url, opts) => {
        calls.push({ url: url, opts: opts });
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Pong', version: '5.1.33' }));
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    h.eq(dom.window.deviceRole, 'Pong',
        'deviceRole should be Pong after poll (was ' + dom.window.deviceRole + ')');

    const topPbtn = dom.window.document.getElementById('topPbtn');
    h.truthy(topPbtn, 'topPbtn should exist');
    h.eq(topPbtn.style.display, 'none',
        'topPbtn should be display:none on Pong (was: "' + topPbtn.style.display + '")');

    const beforeMsgPaused = dom.window.msgPaused;
    const beforeLabel = topPbtn.innerHTML;
    calls.length = 0;
    await dom.window.toggleMsgPause();
    await new Promise(r => setTimeout(r, 20));
    const pauseCalls = calls.filter(c => c.url.startsWith('/pausemsg'));
    h.eq(pauseCalls.length, 0,
        'no /pausemsg POST on Pong (got ' + pauseCalls.length + ')');
    h.eq(dom.window.msgPaused, beforeMsgPaused,
        'msgPaused should not change on Pong (was ' + beforeMsgPaused + ', now ' + dom.window.msgPaused + ')');
    h.eq(topPbtn.innerHTML, beforeLabel,
        'button label should not change on Pong click (was "' + beforeLabel + '", now "' + topPbtn.innerHTML + '")');
    console.log('  PASS');
}

async function test_pause_toggle_noop_before_role_known() {
    console.log('\n=== Test: pause toggle no-op before device role is known ===');
    const dom = await h.setup();
    dom.window.deviceRole = null;
    const before = dom.window.msgPaused;
    await dom.window.toggleMsgPause();
    await new Promise(r => setTimeout(r, 20));
    h.eq(dom.window.msgPaused, before,
        'msgPaused should not change when role is null (was ' + before + ', now ' + dom.window.msgPaused + ')');
    console.log('  PASS');
}

async function test_pause_label_reconciles_with_stats_msgpaused() {
    console.log('\n=== Test: /stats.msgPaused reconciles the button label ===');
    const dom = await h.setup();
    dom.window.deviceRole = 'Ping';
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, msgPaused: 0, role: 'Ping', version: '5.1.29' }));
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    dom.window.msgPaused = true;
    dom.window.applyMsgPauseLabel();
    const topPbtn = dom.window.document.getElementById('topPbtn');
    const beforeLabel = topPbtn.innerHTML;
    h.truthy(beforeLabel.indexOf('Start') !== -1,
        'before poll, button should say Start (msgPaused=true local), got: ' + beforeLabel);
    await dom.window.poll();
    const afterLabel = topPbtn.innerHTML;
    h.truthy(afterLabel.indexOf('Pause') !== -1,
        'after poll reconciled to msgPaused=0, button should say Pause, got: ' + afterLabel);
    console.log('  PASS');
}

async function test_level_change_logs_request_and_result() {
    console.log('\n=== Test: log-level change logs both the request and the result ===');
    const dom = await h.setup();
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.0.9' }));
        }
        if (url.startsWith('/level')) {
            return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({}), text: () => Promise.resolve('') });
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    const captured = [];
    const origLog = dom.window.console.log;
    dom.window.console.log = function(...args) { captured.push(args.join(' ')); };
    try {
        const debug = dom.window.document.querySelector('input[name="lvl"][value="4"]');
        h.truthy(debug, 'Debug radio should exist');
        debug.checked = true;
        debug.dispatchEvent(new dom.window.Event('change'));
        await new Promise((resolve) => setTimeout(resolve, 50));
        dom.window.console.log = origLog;
    } catch (e) {
        dom.window.console.log = origLog;
        throw e;
    }
    const logStr = captured.join('\n');
    h.truthy(logStr.includes('lv=4'),
        'log-level change should log lv=4, got: ' + logStr);
    h.truthy(logStr.includes('button:'),
        'log-level change should log a button: tag, got: ' + logStr);
    console.log('  PASS');
}

async function test_reboot_logs_progress() {
    console.log('\n=== Test: reboot logs the button press and progress ===');
    const dom = await h.setup();
    dom.window.confirm = () => true;
    h.__mockFetch = (url) => {
        if (url === '/reboot' || url.startsWith('/reboot?')) {
            return new Promise(() => {});
        }
        if (url.startsWith('/stats')) {
            return new Promise(() => {});
        }
        return Promise.resolve(h.jsonResp({}));
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
    h.truthy(logStr.includes('button: reboot'),
        'reboot should log a button: reboot message, got: ' + logStr);
    rebootPromise.catch(() => {});
    console.log('  PASS');
}

async function test_reset_logs_request_and_result() {
    console.log('\n=== Test: reset logs the request and the result ===');
    const dom = await h.setup();
    h.__mockFetch = (url) => {
        if (url === '/reset' || url.startsWith('/reset?')) {
            return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({}), text: () => Promise.resolve('') });
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
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
    h.truthy(logStr.includes('button: reset'),
        'reset should log a button: reset message, got: ' + logStr);
    h.truthy(logStr.includes('reset result:'),
        'reset should log a result line, got: ' + logStr);
    console.log('  PASS');
}

async function test_reset_button_updates_dashboard_counters() {
    console.log('\n=== Test: Reset button updates dashboard counters (the fix) ===');
    const dom = await h.setup();
    dom.window.deviceRole = 'Ping';
    let countersReset = false;
    h.__mockFetch = (url, opts) => {
        if (url.startsWith('/stats')) {
            const stats = countersReset
                ? { state: 'OK', errCount: 0, errTotal: 0,
                    lostMsgs: 0, txBps: 0, rxBps: 0,
                    txTotal: 0, rxTotal: 0,
                    rssi: -65, freeHeap: 200000, uptimeS: 0,
                    baudRate: 115200, lvl: 3, mode: 0,
                    msgPaused: 1, role: 'Ping', version: '5.1.34' }
                : { state: 'OK', errCount: 5, errTotal: 3,
                    lostMsgs: 7, txBps: 1024, rxBps: 2048,
                    txTotal: 99999, rxTotal: 88888,
                    rssi: -65, freeHeap: 200000, uptimeS: 0,
                    baudRate: 115200, lvl: 3, mode: 0,
                    msgPaused: 1, role: 'Ping', version: '5.1.34' };
            return Promise.resolve(h.jsonResp(stats));
        }
        if (url === '/reset' || url.startsWith('/reset?')) {
            countersReset = true;
            return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({}), text: () => Promise.resolve('') });
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    var txtotBefore = dom.window.document.getElementById('txtot').textContent;
    var disconBefore = dom.window.document.getElementById('discon').textContent;
    h.truthy(txtotBefore.indexOf('total') === 0,
        'txtot should start with "total" before reset (was: "' + txtotBefore + '")');
    h.truthy(txtotBefore !== 'total 0 B' && txtotBefore !== 'total \u2014',
        'txtot should be non-zero before reset (was: "' + txtotBefore + '")');
    h.eq(disconBefore, '3', 'discon should be 3 before reset');

    await dom.window.resetAll();
    await dom.window.poll();

    var txtotAfter = dom.window.document.getElementById('txtot').textContent;
    var rxtotAfter = dom.window.document.getElementById('rxtot').textContent;
    var disconAfter = dom.window.document.getElementById('discon').textContent;
    var lostAfter = dom.window.document.getElementById('lostmsgs').textContent;
    var errcntAfter = dom.window.document.getElementById('errcnt').textContent;
    h.truthy(txtotAfter.indexOf('0') !== -1 && txtotAfter.indexOf('total') !== -1,
        'txtot should be "total 0 B" after reset (was: "' + txtotAfter + '")');
    h.eq(rxtotAfter, 'total 0 B',
        'rxtot should be total 0 B after reset (was: "' + rxtotAfter + '")');
    h.eq(disconAfter, '0',
        'discon should be 0 after reset (was: "' + disconAfter + '")');
    h.eq(lostAfter, '0 lost msgs',
        'lostmsgs should be 0 lost msgs after reset (was: "' + lostAfter + '")');
    h.eq(errcntAfter, '0 frame errors',
        'errcnt should be 0 frame errors after reset (was: "' + errcntAfter + '")');
    console.log('  PASS');
}

async function test_clear_log_logs_count() {
    console.log('\n=== Test: clearLog logs the number of entries cleared ===');
    const dom = await h.setup();
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.0.9' }));
        }
        if (url.startsWith('/logs')) {
            return Promise.resolve(h.jsonResp({
                head: 3,
                lines: [
                    { seq: 0, sev: 'I', text: 'line a' },
                    { seq: 1, sev: 'I', text: 'line b' },
                    { seq: 2, sev: 'I', text: 'line c' },
                ],
            }));
        }
        return Promise.resolve(h.jsonResp({}));
    };
    await dom.window.poll();
    await dom.window.poll();
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
    h.truthy(logStr.includes('button: clear log'),
        'clearLog should log a button: clear log message, got: ' + logStr);
    h.truthy(logStr.includes('3 entries'),
        'clearLog should log the count of cleared entries, got: ' + logStr);
    console.log('  PASS');
}

async function test_log_overlay_open_close_logged() {
    console.log('\n=== Test: log overlay open/close are logged ===');
    const dom = await h.setup();
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
    h.truthy(logStr.includes('overlay opened'),
        'openLogFull should log "overlay opened", got: ' + logStr);
    h.truthy(logStr.includes('overlay closed'),
        'closeLogFull should log "overlay closed", got: ' + logStr);
    console.log('  PASS');
}

async function test_copy_modern_path_reverts_button_label() {
    console.log('\n=== Test: copyLog() modern path reverts "Copied" back to "Copy" ===');
    const dom = await h.setup();
    const win = dom.window;
    let writeTextCalls = 0;
    win.navigator.clipboard = {
        writeText: function() { writeTextCalls++; return Promise.resolve(); }
    };
    const btn = win.document.getElementById('cbtn');
    h.truthy(btn, 'cbtn must exist');
    h.truthy(btn.textContent === 'Copy',
        'cbtn initial text should be "Copy", got: "' + btn.textContent + '"');
    win.copyLog();
    await new Promise((r) => setTimeout(r, 50));
    h.truthy(writeTextCalls === 1,
        'copyLog() should call navigator.clipboard.writeText once, got: ' + writeTextCalls);
    h.truthy(btn.textContent.indexOf('Copied') !== -1,
        'after copyLog(), cbtn should show "Copied", got: "' + btn.textContent + '"');
    await new Promise((r) => setTimeout(r, 1300));
    h.truthy(btn.textContent === 'Copy',
        'after revert timeout, cbtn should be back to "Copy", got: "' + btn.textContent + '"');
    console.log('  PASS');
}

async function test_copy_fallback_path_reverts_button_label() {
    console.log('\n=== Test: copyLog() fallback path reverts "Copied" back to "Copy" ===');
    const dom = await h.setup();
    const win = dom.window;
    Object.defineProperty(win.navigator, 'clipboard', {
        configurable: true, get: function() { return undefined; }
    });
    win.document.execCommand = function(cmd) {
        h.truthy(cmd === 'copy', 'fallback should call execCommand("copy")');
        return true;
    };
    const btn = win.document.getElementById('cbtn');
    h.truthy(btn, 'cbtn must exist');
    h.truthy(btn.textContent === 'Copy',
        'cbtn initial text should be "Copy", got: "' + btn.textContent + '"');
    win.copyLog();
    h.truthy(btn.textContent.indexOf('Copied') !== -1,
        'after copyLog() fallback, cbtn should show "Copied", got: "' + btn.textContent + '"');
    await new Promise((r) => setTimeout(r, 1300));
    h.truthy(btn.textContent === 'Copy',
        'after revert timeout, cbtn should be back to "Copy", got: "' + btn.textContent + '"');
    console.log('  PASS');
}

async function test_fallbackCopy_sets_copied_label_on_success() {
    console.log('\n=== Test: fallbackCopy() sets "Copied" label on execCommand success ===');
    const dom = await h.setup();
    const win = dom.window;
    const btn = win.document.createElement('button');
    btn.textContent = 'Copy';
    win.document.body.appendChild(btn);
    win.document.execCommand = function() { return true; };
    win.fallbackCopy('hello world', btn);
    h.truthy(btn.textContent.indexOf('Copied') !== -1,
        'fallbackCopy should set label to "Copied" on success, got: "' + btn.textContent + '"');
    const stray = win.document.querySelectorAll('textarea');
    h.truthy(stray.length === 0,
        'fallbackCopy should remove the temporary textarea from the DOM, got ' + stray.length + ' left');
    console.log('  PASS');
}

async function test_fallbackCopy_sets_failed_label_on_throw() {
    console.log('\n=== Test: fallbackCopy() sets "Failed" label when execCommand throws ===');
    const dom = await h.setup();
    const win = dom.window;
    const btn = win.document.createElement('button');
    btn.textContent = 'Copy';
    win.document.body.appendChild(btn);
    win.document.execCommand = function() { throw new Error('not allowed'); };
    win.fallbackCopy('hello world', btn);
    h.truthy(btn.textContent.indexOf('Failed') !== -1,
        'fallbackCopy should set label to "Failed" on throw, got: "' + btn.textContent + '"');
    console.log('  PASS');
}

(async () => {
    try {
        await test_msgPaused_starts_true();
        await test_copy_save_emit_unix_newlines();
        await test_copy_save_no_longer_use_textcontent();
        await test_pause_toggle();
        await test_poll_resets_busy_on_stats_failure();
        await test_poll_resets_busy_on_logs_failure();
        await test_pause_logs_state_change();
        await test_pause_toggle_posts_to_pausemsg_endpoint();
        await test_pause_toggle_noop_on_pong();
        await test_pause_toggle_noop_before_role_known();
        await test_pause_label_reconciles_with_stats_msgpaused();
        await test_level_change_logs_request_and_result();
        await test_reboot_logs_progress();
        await test_reset_logs_request_and_result();
        await test_reset_button_updates_dashboard_counters();
        await test_clear_log_logs_count();
        await test_log_overlay_open_close_logged();
        await test_copy_modern_path_reverts_button_label();
        await test_copy_fallback_path_reverts_button_label();
        await test_fallbackCopy_sets_copied_label_on_success();
        await test_fallbackCopy_sets_failed_label_on_throw();
    } catch (e) {
        console.error('UNCAUGHT:', e);
    }
    h.summary('dashboard-log');
})();
