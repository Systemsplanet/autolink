// dashboard-timeout.test.js -- tfetch() default timeout
// contract (5000 ms; never 2500 ms).
//
// Imports the shared harness from
// dashboard-test-harness.js.

'use strict';

const h = require('./dashboard-test-harness');

async function test_fetch_timeout_default() {
    console.log('\n=== Test: tfetch default timeout is 5000ms (not 2500) ===');
    const dom = await h.setup();
    const src = dom.window.tfetch.toString();
    h.truthy(src.includes('ms||5000'),
        'tfetch should default to 5000ms timeout');
    h.truthy(!src.includes('ms||2500'),
        'tfetch should NOT have 2500ms as default');
    console.log('  PASS');
}

(async () => {
    try {
        await test_fetch_timeout_default();
    } catch (e) {
        console.error('UNCAUGHT:', e);
    }
    h.summary('dashboard-timeout');
})();
