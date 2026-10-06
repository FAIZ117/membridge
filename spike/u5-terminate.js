'use strict';
// Terminate probe (feeds the M3 §13 test design): does Worker.terminate()
// resolve promptly while the worker is parked in Atomics.wait on a
// custom-BackingStore SAB? And does the isolate teardown hang?

const { Worker } = require('worker_threads');
const { addon, probeName, cleanupAll } = require('./lib');

const a = addon();
const name = probeName('u5term');
const bytes = 4096;
new Int32Array(a.open(name, bytes));

const w = new Worker(__dirname + '/u5-worker.js', {
  workerData: { name, bytes, timeoutMs: 60000 }, // parks for a long time
});
w.on('error', e => console.error('[worker error]', e.stack || e));
w.on('exit', c => console.log('[main] worker exit', c));

let resolveReady;
const ready = new Promise(r => { resolveReady = r; });
w.on('message', m => { if (m.type === 'ready') resolveReady(); });

(async () => {
  await ready;
  await new Promise(r => setTimeout(r, 100)); // parked now
  const t0 = Date.now();
  console.log('[main] terminating parked worker...');
  await w.terminate();
  console.log(`[main] terminate resolved in ${Date.now() - t0} ms`);
  cleanupAll(a, [name]);
  process.exit(0); // exit guard: process.exitCode path may still hang
})().catch(e => { console.error('[main] failed:', e); cleanupAll(a, [name]); process.exit(1); });

setTimeout(() => {
  console.error('[main] FAILED: terminate did not resolve in 10 s');
  cleanupAll(a, [name]);
  process.exit(1);
}, 10000).unref();

setInterval(() => {}, 1 << 30); // keep main alive for the test duration
