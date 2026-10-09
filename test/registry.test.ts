// registry.test.ts — §5.4 lifecycle + §13 registry/GC tests. GC assertions
// run in dedicated `--expose-gc` child processes (the test runner does not
// forward that flag into its per-file children); the parent asserts exit
// codes. Other tests run in-process.

import { test } from 'node:test';
import { Worker } from 'node:worker_threads';
import { spawnSync } from 'node:child_process';
import { open, unlink } from '../src/core';
import { fallbackOpen, fallbackUnlink } from '../src/fallback';
import { Mutex } from '../src/mutex';
import { RingProducer } from '../src/ringbuffer';
import { assert, assertThrowsCode, uniqueName, holdForTest, unlinkQuietly } from './helpers';

const PKG = require.resolve('../src/core'); // tests use core directly (debug hooks are not re-exported)

/** Run a script with --expose-gc in a child; exit 0 = assertion held. */
function runInGcChild(script: string, name: string): { status: number; out: string } {
  const r = spawnSync(process.execPath, ['--expose-gc', '-e', script], {
    env: { ...process.env, MEMBRIDGE_TEST_NAME: name, MEMBRIDGE_TEST_PKG: PKG },
    encoding: 'utf8',
  });
  return { status: r.status ?? -1, out: r.stdout + r.stderr };
}

test('GC: mapping freed only after the last SAB is collected (child, --expose-gc)', () => {
  const name = uniqueName();
  try {
    const r = runInGcChild(
      `
      const { open, debugRegistryHas } = require(process.env.MEMBRIDGE_TEST_PKG);
      const name = process.env.MEMBRIDGE_TEST_NAME;
      let sab = open(name, 4096);
      new Int32Array(sab)[0] = 3;
      if (!debugRegistryHas(name)) process.exit(2);
      sab = null;
      for (let i = 0; i < 6; i++) global.gc();
      process.exit(debugRegistryHas(name) ? 3 : 0);
      `,
      name,
    );
    assert.strictEqual(r.status, 0, `child exit ${r.status}: ${r.out}`);
  } finally {
    unlinkQuietly(name);
  }
});

test('GC: mapping kept alive while ANY SAB reference remains (child, --expose-gc)', () => {
  const name = uniqueName();
  try {
    const r = runInGcChild(
      `
      const { open, debugRegistryHas } = require(process.env.MEMBRIDGE_TEST_PKG);
      const name = process.env.MEMBRIDGE_TEST_NAME;
      let a = open(name, 4096);
      const b = open(name, 4096);
      new Int32Array(a)[0] = 3;
      a = null;
      for (let i = 0; i < 6; i++) global.gc();
      if (!debugRegistryHas(name)) process.exit(2);
      if (new Int32Array(b)[0] !== 3) process.exit(3);
      process.exit(0);
      `,
      name,
    );
    assert.strictEqual(r.status, 0, `child exit ${r.status}: ${r.out}`);
  } finally {
    unlinkQuietly(name);
  }
});

test('GC: grow replaces the registry entry; old SABs stay valid (child, --expose-gc)', { skip: process.platform !== 'linux' ? 'grow is a Linux-only policy' : false }, () => {
  const name = uniqueName();
  try {
    const r = runInGcChild(
      `
      const { open, debugRegistryHas } = require(process.env.MEMBRIDGE_TEST_PKG);
      const name = process.env.MEMBRIDGE_TEST_NAME;
      const old = open(name, 4096);
      new Int32Array(old)[0] = 7;
      const grown = open(name, 16384, { sizePolicy: 'grow' });
      if (grown.byteLength !== 16384) process.exit(2);
      if (new Int32Array(grown)[0] !== 7) process.exit(3);
      if (!debugRegistryHas(name)) process.exit(4);
      if (new Int32Array(old).length !== 1024) process.exit(5);
      if (new Int32Array(old)[0] !== 7) process.exit(6);
      process.exit(0);
      `,
      name,
    );
    assert.strictEqual(r.status, 0, `child exit ${r.status}: ${r.out}`);
  } finally {
    unlinkQuietly(name);
  }
});

test('at-least prefix and full view share one mapping', () => {
  const name = uniqueName();
  try {
    const full = open(name, 8192);
    new Int32Array(full)[2047] = 42;
    const prefix = open(name, 4096, { sizePolicy: 'at-least' });
    assert.strictEqual(prefix.byteLength, 4096);
    assert.strictEqual(new Int32Array(prefix)[0], 0);
    new Int32Array(prefix)[1] = 9;
    assert.strictEqual(new Int32Array(full)[1], 9);
  } finally {
    unlinkQuietly(name);
  }
});

test('worker isolate reuses the same mapping and shares memory', async () => {
  const name = uniqueName();
  try {
    const main = open(name, 4096);
    new Int32Array(main)[2] = 21;
    const workerSaw = await new Promise<number>((resolve, reject) => {
      const w = new Worker(
        `const { parentPort, workerData } = require('worker_threads');
         const { open } = require(workerData.pkg);
         const sab = open(workerData.name, 4096);
         // add BEFORE posting: the message resolves main's await, so a
         // post-first ordering races the add (lost under ASAN timing)
         Atomics.add(new Int32Array(sab), 3, 5);
         parentPort.postMessage({ first: new Int32Array(sab)[2] });`,
        { eval: true, workerData: { name, pkg: PKG } },
      );
      w.on('message', (m: { first: number }) => resolve(m.first));
      w.on('error', reject);
    });
    assert.strictEqual(workerSaw, 21, 'worker reads through the shared mapping');
    assert.strictEqual(new Int32Array(main)[3], 5, 'main reads the worker write');
  } finally {
    unlinkQuietly(name);
  }
});

test('unlink while SABs are alive: memory stays valid, name is gone', () => {
  const name = uniqueName();
  const sab = open(name, 4096);
  new Int32Array(sab)[0] = 17;
  unlink(name);
  assert.strictEqual(new Int32Array(sab)[0], 17, 'SAB survives unlink');
  unlinkQuietly(name);
});

// F33 residual: the same-process registry reuse path must honor the header's
// kind flags the same way the cross-process join path does — a mutex-kind
// segment re-opened as plain/ring is E_INCOMPATIBLE, not a silent reuse.
test('kind mismatch on registry reuse -> E_INCOMPATIBLE (review F33)', (t) => {
  const name = uniqueName();
  try {
    holdForTest(t, Mutex.open(name));  // creates a mutex-kind segment and caches the mapping
    // kind-specific request over a different kind: E_INCOMPATIBLE (plain
    // opens are kind-agnostic by design, so a plain request is NOT asserted
    // to fail — parity with the cross-process join check).
    assertThrowsCode(() => RingProducer.open(name, { capacity: 4096 }), 'E_INCOMPATIBLE');
    // the original kind still opens cleanly afterwards
    const m = Mutex.open(name);
    m.lock();
    m.unlock();
  } finally {
    unlinkQuietly(name);
  }
});

// F36: the fallback name table is per ISOLATE — a worker isolate loads a
// fresh copy of the fallback module and cannot see main-thread segments
// (a plain JS Map cannot cross realms). Pins the documented scope.
test('fallback table is per isolate; cross-isolate join is a clean E_NOT_FOUND (review F36)', async () => {
  const name = uniqueName();
  const FB = require.resolve('../fallback.js'); // runtime-relative to dist/test
  try {
    fallbackOpen(name, 4096, { mode: 'create' });
    const w = new Worker(
      `const { workerData, parentPort } = require('worker_threads');
       const { fallbackOpen } = require(workerData.fb);
       try {
         fallbackOpen(workerData.name, undefined, { mode: 'join' });
         parentPort.postMessage('BAD: joined');
       } catch (e) {
         parentPort.postMessage(e.code);
       }`,
      { eval: true, workerData: { name, fb: FB } },
    );
    const got = await new Promise<string>((res, rej) => {
      w.on('message', (m: string) => res(m));
      w.on('error', rej);
    });
    assert.strictEqual(got, 'E_NOT_FOUND', 'worker cannot see the main isolate fallback segment');
  } finally {
    fallbackUnlink(name);
  }
});
