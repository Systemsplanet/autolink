// dashboard-role.test.js -- role-conditional dashboard
// behavior: body[data-role] toggle, .ping-only
// visibility, default fill-mode pill, Save filename
// by role, Reboot button placement.
//
// Imports the shared harness from
// dashboard-test-harness.js (mock fetch, jsonResp,
// assert / eq / truthy, setup).

'use strict';

const h = require('./dashboard-test-harness');

async function test_ping_role_sets_data_role_ping() {
    console.log('\n=== Test: role="Ping" sets body[data-role="ping"] ===');
    const dom = await h.setup();
    h.resetFetchCalls();
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.0.8' }));
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    h.eq(dom.window.document.body.getAttribute('data-role'), 'ping',
        'body[data-role] should be "ping"');
    console.log('  PASS');
}

async function test_pong_role_sets_data_role_pong() {
    console.log('\n=== Test: role="Pong" sets body[data-role="pong"] ===');
    const dom = await h.setup();
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Pong', version: '5.0.8' }));
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    h.eq(dom.window.document.body.getAttribute('data-role'), 'pong',
        'body[data-role] should be "pong"');
    console.log('  PASS');
}

async function test_pong_role_hides_ping_only_controls() {
    console.log('\n=== Test: role="Pong" hides .ping-only controls via CSS ===');
    const dom = await h.setup();
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Pong', version: '5.0.8' }));
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();

    const modeGroup = dom.window.document.getElementById('modeGroup');
    h.truthy(modeGroup, 'modeGroup should exist');
    h.eq(modeGroup.classList.contains('ping-only'), true,
        'modeGroup should have .ping-only class');

    const topPbtn = dom.window.document.getElementById('topPbtn');
    h.truthy(topPbtn, 'topPbtn should exist');
    h.eq(topPbtn.classList.contains('ping-only'), true,
        'topPbtn should have .ping-only class');

    const style = dom.window.getComputedStyle(modeGroup);
    h.eq(style.display, 'none', 'modeGroup should be display:none when role is pong');

    const style2 = dom.window.getComputedStyle(topPbtn);
    h.eq(style2.display, 'none', 'topPbtn should be display:none when role is pong');
    console.log('  PASS');
}

async function test_log_pause_buttons_visible_on_pong() {
    console.log('\n=== Test: log-scroll pause buttons (pbtn, pbtn2) visible on Pong ===');
    const dom = await h.setup();
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Pong', version: '5.1.6' }));
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();

    const pbtn = dom.window.document.getElementById('pbtn');
    const pbtn2 = dom.window.document.getElementById('pbtn2');
    const topPbtn = dom.window.document.getElementById('topPbtn');
    h.truthy(pbtn, 'pbtn (log scroll pause, inline) should exist');
    h.truthy(pbtn2, 'pbtn2 (log scroll pause, overlay) should exist');
    h.truthy(topPbtn, 'topPbtn (message updates pause) should exist');

    h.eq(pbtn.classList.contains('ping-only'), false,
        'pbtn (log scroll pause) should NOT have .ping-only class');
    h.eq(pbtn2.classList.contains('ping-only'), false,
        'pbtn2 (log scroll pause) should NOT have .ping-only class');

    h.eq(topPbtn.classList.contains('ping-only'), true,
        'topPbtn (message updates pause) should have .ping-only class');

    const sPbtn = dom.window.getComputedStyle(pbtn).display;
    const sTopPbtn = dom.window.getComputedStyle(topPbtn).display;
    h.truthy(sPbtn !== 'none',
        'pbtn (log scroll pause) should be visible on Pong (display='+sPbtn+')');
    h.eq(sTopPbtn, 'none',
        'topPbtn (message updates pause) should be display:none on Pong');
    console.log('  PASS');
}

async function test_default_mode_is_sequential() {
    console.log('\n=== Test: default device mode (mode=0) maps to Sequential radio ===');
    const dom = await h.setup();
    h.__mockFetch = (url) => {
        if (url.startsWith('/stats')) {
            return Promise.resolve(h.jsonResp({ state: 'OK', errCount: 0, errTotal: 0,
                lostMsgs: 0, txBps: 0, rxBps: 0, txTotal: 0, rxTotal: 0,
                rssi: -65, freeHeap: 200000, uptimeS: 0, baudRate: 115200,
                lvl: 3, mode: 0, role: 'Ping', version: '5.1.12' }));
        }
        return Promise.resolve(h.jsonResp({ head: 0, lines: [] }));
    };
    await dom.window.poll();
    const seq = dom.window.document.getElementById('modeSeq');
    const rand = dom.window.document.getElementById('modeRand');
    h.eq(seq.checked, true, 'Sequential radio should be checked (default device mode)');
    h.eq(rand.checked, false, 'Random radio should NOT be checked (default device mode)');
    const group = dom.window.document.getElementById('modeGroup');
    h.truthy(group, 'modeGroup should exist');
    const s = dom.window.getComputedStyle(group);
    h.truthy(s.display !== 'none', 'modeGroup should be visible on Ping (display='+s.display+')');
    console.log('  PASS');
}

async function test_save_log_button_present() {
    console.log('\n=== Test: Save button is present in log controls ===');
    const dom = await h.setup();
    const sbtn = dom.window.document.getElementById('sbtn');
    h.truthy(sbtn, 'sbtn (Save) should exist');
    const text = sbtn.textContent.trim();
    h.eq(text, 'Save', 'button label should be "Save"');
    h.assert(typeof dom.window.saveLog === 'function',
        'saveLog should be a global function');
    console.log('  PASS');
}

async function test_save_log_filename_by_role() {
    console.log('\n=== Test: saveLog picks ping.txt or pong.txt by role ===');
    const dom = await h.setup();
    const src = require('fs').readFileSync(
        require('path').join(__dirname, '../../src/al/web/dashboard.js'),
        'utf8'
    );
    h.truthy(src.indexOf("'ping.txt'") !== -1,
        "source should reference 'ping.txt' for the Ping role");
    h.truthy(src.indexOf("'pong.txt'") !== -1,
        "source should reference 'pong.txt' for the Pong role");
    h.truthy(src.indexOf('a.download=name') !== -1,
        'source should bind a.download=name so the browser uses it');
    console.log('  PASS');
}

async function test_reboot_button_in_header() {
    console.log('\n=== Test: Reboot button is in the header ===');
    const dom = await h.setup();
    const rebootBtn = dom.window.document.getElementById('rebootBtnTop');
    h.truthy(rebootBtn, 'rebootBtnTop should exist');
    const header = dom.window.document.querySelector('header');
    h.truthy(header, 'header should exist');
    h.assert(header.contains(rebootBtn), 'rebootBtnTop should be inside <header>');
    const oldBtn = dom.window.document.getElementById('rebootBtn');
    h.eq(oldBtn, null, 'old rebootBtn ID should be removed');
    console.log('  PASS');
}

(async () => {
    try {
        await test_ping_role_sets_data_role_ping();
        await test_pong_role_sets_data_role_pong();
        await test_pong_role_hides_ping_only_controls();
        await test_log_pause_buttons_visible_on_pong();
        await test_default_mode_is_sequential();
        await test_save_log_button_present();
        await test_save_log_filename_by_role();
        await test_reboot_button_in_header();
    } catch (e) {
        console.error('UNCAUGHT:', e);
    }
    h.summary('dashboard-role');
})();
