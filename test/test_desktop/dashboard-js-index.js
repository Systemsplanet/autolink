// dashboard-js-index.js -- thin runner that executes
// every dashboard JS spec file in sequence. Used by
// `make test_dashboard_js` (the same target the
// pre-split single-file test used) and by hand.
//
// Each spec runs as its own Node process (via require
// + child_process spawn) so the pass/fail summary
// from one spec doesn't leak into the next. The
// runner itself only orchestrates the spawn order
// and prints a combined pass/fail total.
//
// Run: node dashboard-js-index.js
'use strict';

const { spawnSync } = require('child_process');
const path = require('path');

const specs = [
    'dashboard-role.test.js',
    'dashboard-poll.test.js',
    'dashboard-log.test.js',
    'dashboard-timeout.test.js',
];

let totalFail = 0;
for (const spec of specs) {
    console.log(`\n=== ${spec} ===`);
    const r = spawnSync(process.execPath, [path.join(__dirname, spec)],
        { stdio: 'inherit' });
    if (r.status !== 0) {
        totalFail++;
    }
}

console.log(`\n=== Dashboard JS: ${totalFail} spec(s) failed ===`);
process.exit(totalFail > 0 ? 1 : 0);
