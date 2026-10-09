'use strict';
// bench/ring.js — RingBuffer throughput: small and large messages through the
// two-phase zero-copy API and the copying convenience API.

const { performance } = require('node:perf_hooks');
const { Worker } = require('node:worker_threads');
const { RingProducer, RingConsumer } = require('../dist/ringbuffer');
const { unlink } = require('../dist/core');

const NAME = `/shm-bridge-bench-ring-${process.pid}`;

async function run() {
  const rows = [];
  // The consumer blocks the JS thread, so the producer lives in a worker.
  const w = new Worker(
    `const { parentPort, workerData } = require('worker_threads');
     const { RingProducer } = require(workerData.pkg);
     const p = RingProducer.open(workerData.name, { capacity: 1048576 });
     parentPort.postMessage('ready');
     parentPort.on('message', ({ size, count }) => {
       const buf = Buffer.alloc(size);
       for (let i = 0; i < count; i++) p.write(buf);
       parentPort.postMessage('done');
     });`,
    {
      eval: true,
      workerData: { name: NAME, pkg: require.resolve('../dist/ringbuffer.js') },
    },
  );
  await new Promise((r) => w.on('message', (m) => m === 'ready' && r()));
  const c = RingConsumer.open(NAME);

  for (const size of [64, 4096, 65536]) {
    const count = Math.max(1000, Math.floor(2e7 / size));
    w.postMessage({ size, count });
    const t0 = performance.now();
    let bytes = 0;
    for (let i = 0; i < count; i++) {
      const msg = c.read({ timeoutMs: 20000 });
      bytes += msg.length;
    }
    const ms = performance.now() - t0;
    rows.push({
      label: `${size} B messages, ${count} msgs (${(bytes / 1e6).toFixed(0)} MB)`,
      value: `${(count / (ms / 1e3) / 1e3).toFixed(1)}k msgs/s, ${(bytes / (ms / 1e3) / 1e6).toFixed(0)} MB/s`,
    });
  }

  await w.terminate();
  unlink(NAME);
  return rows;
}

module.exports = { run };
