'use strict';
// U5 (PLAN §2): do Atomics.wait / Atomics.notify pair up across *distinct*
// BackingStores covering the same mapping? V8 keys its waiter list by
// address (verified M1), which is what makes the §5.4 registry design work:
// every open() makes a new BackingStore + SAB over one shared Mapping, and
// Atomics on different SABs still pair up — same isolate (window) and across
// isolates (worker's own open()).
//
// PASS = the worker's wait returns 'ok' well before its 5 s timeout in both
// cases. 'timed-out' despite notifies would mean V8 keys by BackingStore and
// §5.4 must share one BackingStore per (name, length).

const { Worker } = require('worker_threads');
const { ADDON, addon, probeName, unlinkQuietly } = require('./lib');

const a = addon();
const name = probeName('u5');
const bytes = 4096;

// The waiter runs in a worker isolate with its own open() -> its own
// BackingStore over the same process-wide Mapping.
const WORKER_CODE = `
  const { parentPort, workerData } = require('worker_threads');
  const a = require(process.env.SHM_BRIDGE_PROBE_ADDON);
  const sab = a.open(workerData.name, workerData.bytes);
  const i32 = new Int32Array(sab);
  parentPort.postMessage({ type: 'ready' });
  const t0 = process.hrtime.bigint();
  const r = Atomics.wait(i32, 0, 0, workerData.timeoutMs);
  parentPort.postMessage({
    type: 'result',
    result: r,
    elapsedMs: Number(process.hrtime.bigint() - t0) / 1e6,
  });
`;

function spawnWaiter() {
  const w = new Worker(WORKER_CODE, {
    eval: true,
    workerData: { name, bytes, timeoutMs: 5000 },
    env: { ...process.env, SHM_BRIDGE_PROBE_ADDON: ADDON },
  });
  w.on('error', e => console.error('[worker error]', e.stack || e));
  let resolveReady, resolveResult;
  const ready = new Promise(r => { resolveReady = r; });
  const result = new Promise(r => { resolveResult = r; });
  // Persistent handler with type dispatch — `once` + predicate is broken by
  // design (once consumes the listener on the first message of ANY type).
  w.on('message', m => {
    if (m.type === 'ready') resolveReady();
    else if (m.type === 'result') resolveResult(m);
  });
  return { w, ready, result };
}

// Notify `i32` every 150 ms until `done` settles; returns notifies that woke.
// Repeating removes the worker-startup race (~40 ms until parked) and stays
// decisive: 'ok' => paired; 'timed-out' despite notifies => not paired.
async function notifyRepeatedly(i32, done) {
  let count = 0;
  const timer = setInterval(() => { count += Atomics.notify(i32, 0, 1); }, 150);
  try {
    await done;
  } finally {
    clearInterval(timer);
  }
  return count;
}

async function run() {
  const sabA = a.open(name, bytes);       // main isolate, primary store
  const sabB = a.window(name, 0, bytes);  // main isolate, second store, same mapping
  const iA = new Int32Array(sabA);
  const iB = new Int32Array(sabB);

  // Case B: worker's store woken via the main isolate's window() store
  let h = spawnWaiter();
  await h.ready;
  await new Promise(r => setTimeout(r, 100));
  const notifiedB = await notifyRepeatedly(iB, h.result);
  const msgB = await h.result;
  const caseB = { notifiedTotal: notifiedB, result: msgB.result, elapsedMs: Math.round(msgB.elapsedMs) };
  console.log('U5 case B (worker store <- main window store):', JSON.stringify(caseB));

  // Case A: worker's store woken via the main isolate's primary store
  h = spawnWaiter();
  await h.ready;
  await new Promise(r => setTimeout(r, 100));
  const notifiedA = await notifyRepeatedly(iA, h.result);
  const msgA = await h.result;
  const caseA = { notifiedTotal: notifiedA, result: msgA.result, elapsedMs: Math.round(msgA.elapsedMs) };
  console.log('U5 case A (worker store <- main primary store):', JSON.stringify(caseA));

  const wakeOk = c => c.result === 'ok' && c.notifiedTotal >= 1 && c.elapsedMs < 4000;
  const pass = wakeOk(caseA) && wakeOk(caseB);
  console.log(pass
    ? 'PASS u5-atomics-pairing: V8 keys waiters by address — distinct BackingStores over one mapping pair up'
    : 'FAIL u5-atomics-pairing: distinct BackingStores do NOT pair -> §5.4 must share one BackingStore per (name, length)');
  return pass;
}

run()
  .then(pass => { if (!pass) process.exitCode = 1; })
  .catch(e => { console.error('FAIL u5-atomics-pairing:', e); process.exitCode = 1; })
  .finally(() => unlinkQuietly(a, name));
