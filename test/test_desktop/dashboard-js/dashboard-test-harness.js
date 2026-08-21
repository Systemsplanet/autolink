// dashboard-test-harness.js -- shared harness for the
// split dashboard-js-test specs.
//
// What it carries:
//   * jsdom-based `setup()` (loads the dashboard
//     HTML, mocks fetch, returns the dom).
//   * `__mockFetch` per-test override.
//   * Assert helpers (assert / eq / truthy) and
//     pass/fail counters that print at the end.
//
// The HTML source can come from either the split raw assets under
// src/al/web/assets/ (the committed source of truth) or the
// generated headers under src/al/web/generated/ (build output,
// seven small per-part files -- see build/dashboard_assets.py).
// The harness prefers the raw assets and falls back to the
// generated headers so a regression in the build step still runs
// the test against the compiled output.
//
// Run via the spec files in this directory:
//   node dashboard-role.test.js
//   node dashboard-poll.test.js
//   node dashboard-log.test.js
//   node dashboard-timeout.test.js
// or as one shot via dashboard-js-index.js.

'use strict';

const fs = require('fs');
const path = require('path');
const { JSDOM, ResourceLoader } = require('jsdom');

// ---- Mock the fetch() the dashboard uses -----------------------------------

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

// ---- HTML loading ---------------------------------------------------------

// Load the dashboard HTML. The split refactor
// extracted the markup into three small files under
// src/al/web/ (dashboard_html_part_a.html, _b, _c)
// plus dashboard.css + dashboard.js. The harness
// prefers this split layout and falls back to the
// single AutoLinkWebHtml.h header if any input is
// missing (e.g. someone ran dashboard_assets.py and
// only the header is regenerated).
function loadDashboardHtml() {
    const webDir = path.join(__dirname, '../../../src/al/web');
    const assetsDir = path.join(webDir, 'assets');
    const a = path.join(assetsDir, 'dashboard_html_part_a.html');
    const b = path.join(assetsDir, 'dashboard_html_part_b.html');
    const c = path.join(assetsDir, 'dashboard_html_part_c.html');
    const css = path.join(assetsDir, 'dashboard.css');
    const jsParts = ['dashboard_1_core.js', 'dashboard_2_controls.js',
                      'dashboard_3_poll.js'].map(f => path.join(assetsDir, f));
    if (fs.existsSync(a) && fs.existsSync(b) && fs.existsSync(c) &&
        jsParts.every(fs.existsSync)) {
        // {{VERSION}} markers in the markup + JS are replaced by the
        // build step at compile time (string-literal-concat
        // boundaries). At test time we substitute the version from
        // include/AutoLink.h.
        const version = readVersion();
        const js = jsParts.map(p => fs.readFileSync(p, 'utf8')).join('');
        const raw =
            fs.readFileSync(a, 'utf8') +
            '<style>' + fs.readFileSync(css, 'utf8') + '</style>' +
            fs.readFileSync(b, 'utf8') +
            '<script>' +
            js.replace(/{{\s*VERSION\s*}}/g, version) +
            '</script>' +
            fs.readFileSync(c, 'utf8');
        return raw;
    }
    // Fall back to the generated headers under src/al/web/generated/
    // (seven small per-part files, no combined DASHBOARD_HTML array --
    // see build/dashboard_assets.py). Reachable only if the committed
    // assets/ sources are missing and only the build output survived.
    const genDir = path.join(webDir, 'generated');
    const version = readVersion();
    const assemble = (file, constName) => {
        const src = fs.readFileSync(path.join(genDir, file), 'utf8');
        const re = new RegExp(constName + '\\[\\]\\s*=\\s*([\\s\\S]*?);');
        const m = src.match(re);
        if (!m) throw new Error(`${constName} not found in ${file}`);
        const expr = m[1];
        let out = '', i = 0;
        while (i < expr.length) {
            if (expr.startsWith('R"DASH(', i)) {
                const close = expr.indexOf(')DASH"', i);
                out += expr.slice(i + 'R"DASH('.length, close);
                i = close + ')DASH"'.length;
            } else if (expr.startsWith('AUTOLINK_VERSION', i)) {
                out += version;
                i += 'AUTOLINK_VERSION'.length;
            } else {
                i++;
            }
        }
        return out;
    };
    return assemble('DashboardPartA.h', 'DASHBOARD_HTML_PART_A') +
           assemble('DashboardCss.h', 'DASHBOARD_CSS') +
           assemble('DashboardPartB.h', 'DASHBOARD_HTML_PART_B') +
           assemble('DashboardJs1.h', 'DASHBOARD_JS_1') +
           assemble('DashboardJs2.h', 'DASHBOARD_JS_2') +
           assemble('DashboardJs3.h', 'DASHBOARD_JS_3') +
           assemble('DashboardPartC.h', 'DASHBOARD_HTML_PART_C');
}

function readVersion() {
    const src = fs.readFileSync(
        path.join(__dirname, '../../../include/AutoLink.h'), 'utf8');
    const m = src.match(/#define\s+AUTOLINK_VERSION\s+"([^"]+)"/);
    return m ? m[1] : '0.0.0';
}

async function setup() {
    const html = loadDashboardHtml();
    const dom = new JSDOM(html, {
        runScripts: 'dangerously',
        pretendToBeVisual: true,
        url: 'http://10.10.10.29/',
    });

    dom.window.fetch = recordFetch;

    await new Promise((resolve) => setTimeout(resolve, 50));
    return dom;
}

function summary(label) {
    console.log(`\n=== ${label}: ${pass} passed, ${fail} failed ===`);
    process.exit(fail > 0 ? 1 : 0);
}

module.exports = {
    __mockFetch,
    jsonResp,
    recordFetch,
    assert,
    eq,
    truthy,
    setup,
    summary,
    pass: () => pass,
    fail: () => fail,
    fetchCalls: () => __fetchCalls,
    resetFetchCalls: () => { __fetchCalls = []; },
};
