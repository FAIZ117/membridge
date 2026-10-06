'use strict';
// U5 worker: open() the segment from a worker isolate (new BackingStore over
// the same process-wide Mapping), then Atomics.wait on word 0.

const { parentPort, workerData } = require('worker_threads');
const { addon } = require('./lib');

const a = addon();
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
