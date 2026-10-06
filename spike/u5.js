'use strict';
// U5: do Atomics.wait / Atomics.notify pair up across *distinct* BackingStores
// covering the same mapping?
//   Case A: waiter's store from a worker isolate's own open(); notifier's word
//           from the main isolate's primary store.
//   Case B: waiter's store from a worker isolate; notifier's word from a
//           window() store in the main isolate.
// The notifier repeats every 150 ms until the waiter settles: 'ok' => V8
// paired the waiters by address; 'timed-out' despite notifies => it did not,
// and §5.4 must share one BackingStore per (name, length). Repeating also
// removes the worker-startup race (a worker takes ~40 ms to reach the wait).

const { Worker } = require('worker_threads');
const { addon, probeName, cleanupAll } = require('./lib');

const a = addon();
const name = probeName('u5');
const bytes = 4096;

function spawnWaiter() {
  const w = new Worker(__dirname + '/u5-worker.js', {
    workerData: { name, bytes, timeoutMs: 5000 },
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

  // Case B: worker store <- main window store
  let h = spawnWaiter();
  await h.ready;
  await new Promise(r => setTimeout(r, 100)); // let the worker park
  const notifiedB = await notifyRepeatedly(iB, h.result);
  const msgB = await h.result;
  const caseB = { notifiedTotal: notifiedB, result: msgB.result, elapsedMs: Math.round(msgB.elapsedMs) };
  console.log('U5 case B (worker store <- main window store):', JSON.stringify(caseB));

  // Case A: worker store <- main primary store
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
  .catch(e => { console.error('FAIL u5:', e); process.exitCode = 1; })
  .finally(() => cleanupAll(a, [name]));
