'use strict';
// bench/contention.js — shared-memory throughput: single-process Atomics over
// a shm-bridge segment (the floor any cross-process scheme pays), plus a
// multi-process increment round to show the cross-process cost.

const { performance } = require('node:perf_hooks');
const { fork } = require('node:child_process');
const { open, unlink } = require('../dist/core');
const { wait, notify } = require('../dist/sync');

const NAME = `/shm-bridge-bench-contention-${process.pid}`;
const ITERS = 5e6;

async function run() {
  const rows = [];
  const sab = open(NAME, 64);
  const i32 = new Int32Array(sab);

  // 1) same-process Atomics.add throughput
  {
    const t0 = performance.now();
    for (let i = 0; i < ITERS; i++) Atomics.add(i32, 0, 1);
    const ms = performance.now() - t0;
    rows.push({
      label: `same-process Atomics.add (${(ITERS / 1e6).toFixed(0)}M ops)`,
      value: `${(ITERS / ms / 1e3).toFixed(1)} M ops/s`,
    });
  }

  // 2) cross-process uncontended CAS handoff (2 processes ping-pong a token)
  {
    const handoffs = 20000;
    const child = fork(__filename, ['pong', NAME, String(handoffs)], { stdio: 'ignore' });
    const t0 = performance.now();
    let n = 0;
    while (n < handoffs) {
      if (Atomics.compareExchange(i32, 1, 0, 1) === 0) {
        Atomics.store(i32, 1, 2); // token to pong
        notify(i32, 1, 1);        // wake the parked pong (shared futex)
        wait(i32, 1, 2, 5000); // cross-process futex wait (F1: Atomics cannot)
        n++;
      }
    }
    const ms = performance.now() - t0;
    child.kill('SIGKILL');
    rows.push({
      label: 'cross-process futex handoff (round trip)',
      value: `${(ms / handoffs).toFixed(3)} ms/round-trip (${(handoffs / (ms / 1e3) / 1e3).toFixed(1)}k/s)`,
    });
  }

  unlink(NAME);
  return rows;
}

// child role: flip the token back when it sees 2
if (process.argv[2] === 'pong') {
  const [role, name, handoffsArg] = process.argv.slice(2);
  void role;
  const sync = require('../dist/sync');
  const i32 = new Int32Array(open(name, 64));
  const handoffs = Number(handoffsArg);
  let n = 0;
  // Park on the token word instead of spinning (review P11: the old pong
  // burned a core and measured a spinning counterpart).
  while (n < handoffs) {
    if (Atomics.load(i32, 1) === 2) {
      Atomics.store(i32, 1, 0);
      notify(i32, 1, 1); // shared futex wake
      if (++n >= handoffs) process.exit(0);
    } else {
      sync.wait(i32, 1, 0, 1000); // park until the token flips
    }
  }
  process.exit(0);
}

module.exports = { run };
if (require.main !== module) {
  // parent import: nothing else to do
}
