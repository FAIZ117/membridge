'use strict';
// bench/mutex.js — uncontended lock/unlock cost and contended cross-process
// handoff through the crash-safe Mutex.

const { performance } = require('node:perf_hooks');
const { fork } = require('node:child_process');
const { Mutex } = require('../dist/mutex');
const { unlink } = require('../dist/core');

const NAME = `/shm-bridge-bench-mutex-${process.pid}`;

async function run() {
  const rows = [];
  const m = Mutex.open(NAME);
  const ITERS = 100000;

  // 1) uncontended lock/unlock in one process
  {
    m.lock();
    m.unlock();
    const t0 = performance.now();
    for (let i = 0; i < ITERS; i++) {
      m.lock();
      m.unlock();
    }
    const ms = performance.now() - t0;
    rows.push({
      label: `uncontended lock/unlock (${(ITERS / 1e3).toFixed(0)}k iters)`,
      value: `${(ms / ITERS * 1e3).toFixed(2)} us/op`,
    });
  }

  // 2) crash recovery latency: the holder is SIGKILLed while holding; the
  // parent's lock() recovers via the steal path. (lock() blocks the calling
  // thread, so the kill happens before it — the measured window is from the
  // SIGKILL to the acquired lock; lock() probes the holder's liveness on its
  // first contended pass, so a dead holder is stolen immediately — ~0 ms.)
  {
    const child = fork(__filename, ['hold', NAME], { stdio: 'ignore' });
    await new Promise((resolve) => {
      child.on('message', (m2) => m2.type === 'locked' && resolve());
    });
    await new Promise((r) => setTimeout(r, 100));
    child.kill('SIGKILL');
    await new Promise((r) => child.on('exit', r)); // zombies answer alive
    const t0 = performance.now();
    const res = m.lock({ timeoutMs: 10000 });
    const ms = performance.now() - t0;
    void res;
    m.unlock();
    rows.push({
      label: 'holder SIGKILLed -> steal (recovery latency)',
      value: `${ms.toFixed(0)} ms to acquire after the holder died`,
    });
  }

  unlink(NAME);
  return rows;
}

if (process.argv[2] === 'hold') {
  const m = Mutex.open(process.argv[3]);
  m.lock();
  process.send({ type: 'locked' });
  setInterval(() => {}, 1 << 30); // parent SIGKILLs us
}

module.exports = { run };
