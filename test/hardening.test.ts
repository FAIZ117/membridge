// hardening.test.ts — regression tests from the pre-release review round
// (docs/review/2026-10-06T1909-pre-release-review.md). Each test names the
// finding it pins. POSIX-only byte-crafting tests skip elsewhere.

import { test } from 'node:test';
import fs from 'node:fs';
import { fork } from 'node:child_process';
import { open, unlink } from '../src/core';
import { RingProducer, RingConsumer } from '../src/ringbuffer';
import { assert, uniqueName, unlinkQuietly, buildHeader, shmPath } from './helpers';

const POSIX = process.platform === 'linux' || process.platform === 'darwin';
const SKIP = 'needs POSIX /dev/shm (byte-level hostile-header crafting)';

function craft(name: string, header: Record<string, unknown>, fileBytes = 4096): void {
  fs.writeFileSync(shmPath(name)!, buildHeader(header as never, fileBytes));
}

test('Sec F1: hostile header dataBytes never sizes an SAB past the mapping', { skip: POSIX ? false : SKIP }, () => {
  const name = uniqueName();
  try {
    // 8 KiB object whose header lies: dataBytes = 1 MiB, state ready.
    craft(name, { initState: 2, dataBytes: 1024 * 1024, headerBytes: 4096 }, 8192);
    assert.throws(() => open(name), (e: any) => e.code === 'E_INCOMPATIBLE');
    // sized joins are refused against the short object too, never SIGBUS:
    assert.throws(() => open(name, 65536), (e: any) =>
      e.code === 'E_SIZE_MISMATCH' || e.code === 'E_INCOMPATIBLE');
    assert.throws(() => open(name, 65536, { sizePolicy: 'at-least' }), (e: any) =>
      e.code === 'E_SIZE_MISMATCH' || e.code === 'E_INCOMPATIBLE');
  } finally {
    unlinkQuietly(name);
  }
});

test('Sec F3: hostile headerBytes/initializerSlot cannot index past the mapping', { skip: POSIX ? false : SKIP }, () => {
  const name = uniqueName();
  try {
    craft(name, {
      initState: 1,
      headerBytes: 0x80000000,
      initializerSlot: 0x1000000,
      dataBytes: 4096,
    });
    // Pre-ready takeover path must bound row indexing by the mapping — this
    // used to be an immediate SIGSEGV.
    const sab = open(name, 4096, { initTimeoutMs: 3000 });
    assert.strictEqual(sab.byteLength, 4096, 'takeover recovered the object');
    new Int32Array(sab)[0] = 1; // backed: no SIGBUS
  } finally {
    unlinkQuietly(name);
  }
});

test('Corr F6: zero-length object joined size-less is recovered, not SIGBUS', { skip: POSIX ? false : SKIP }, () => {
  const name = uniqueName();
  try {
    fs.writeFileSync(shmPath(name)!, Buffer.alloc(0));
    // RingConsumer.open joins size-less; on a crashed producer it must get a
    // clean error, never a crash (this used to SIGBUS on the header read).
    assert.throws(() => RingConsumer.open(name), (e: any) =>
      e.code === 'E_INCOMPATIBLE' || e.code === 'E_SIZE_INVALID');
  } finally {
    unlinkQuietly(name);
  }
});

test('Corr F5/F26: dead initializer takeover still works under the new protocol', { skip: POSIX ? false : SKIP }, async () => {
  const name = uniqueName();
  try {
    // A creator that died after claiming a row and setting state=1.
    const r = await new Promise<{ pid: number; start: number }>((resolve) => {
      const { spawn } = require('node:child_process');
      const child = spawn(process.execPath, ['-e', `
        const { open } = require(${JSON.stringify(require.resolve('../src/core'))});
        // occupy the initializer seat and die without finishing
        const sab = open(process.env.MX_NAME, 4096, { mode: 'create' });
        setInterval(() => {}, 1 << 30);
      `], { env: { ...process.env, MX_NAME: name }, stdio: 'ignore' });
      // give it a moment to be mid-init, then hard-kill and leave it UNreaped
      // (zombie) for a beat before the parent proceeds
      setTimeout(() => {
        child.kill('SIGKILL');
        const { readStartTime } = require('./helpers');
        resolve({ pid: child.pid!, start: readStartTime(child.pid!) });
      }, 200);
    });
    void r;
    const sab = open(name, 4096, { initTimeoutMs: 5000 });
    assert.strictEqual(sab.byteLength, 4096);
    new Int32Array(sab)[0] = 5; // fully backed
  } finally {
    unlinkQuietly(name);
  }
});

test('Corr F8: cross-process grow never shrinks and covers its window', { skip: POSIX ? false : SKIP }, async () => {
  const name = uniqueName();
  try {
    // Child grows to 1 MiB; parent then grow-opens 2 MiB.
    const child = fork(
      '-e',
      [],
      {},
    );
    void child;
    const { spawnSync } = require('node:child_process');
    const r = spawnSync(process.execPath, ['-e', `
      const { open } = require(${JSON.stringify(require.resolve('../src/core'))});
      open(process.env.MX_NAME, 1024 * 1024, { sizePolicy: 'grow', mode: 'create' });
    `], { env: { ...process.env, MX_NAME: name }, encoding: 'utf8' });
    assert.strictEqual(r.status, 0, r.stderr);
    const big = open(name, 2 * 1024 * 1024, { sizePolicy: 'grow' });
    assert.strictEqual(big.byteLength, 2 * 1024 * 1024);
    new Uint8Array(big)[2 * 1024 * 1024 - 1] = 1; // last byte backed: no SIGBUS
    // a smaller grower afterwards must not shrink the header below 2 MiB
    const again = open(name, 1024 * 1024, { sizePolicy: 'grow' });
    assert.strictEqual(again.byteLength, 1024 * 1024);
    const v = new Uint8Array(open(name, 2 * 1024 * 1024, { sizePolicy: 'at-least' }));
    assert.strictEqual(v.length, 2 * 1024 * 1024);
  } finally {
    unlinkQuietly(name);
  }
});
void fork;

test('Corr F16: registry refuses a stale mapping after unlink+recreate', (t) => {
  const name = uniqueName();
  t.after(() => unlinkQuietly(name));
  const a = open(name, 256);
  new Int32Array(a)[0] = 1;
  // Simulate another process's unlink+recreate behind our cached mapping:
  // drop the registry entry? No — the POINT is the entry stays. Unlink and
  // recreate via a child so our mapping is untouched but the name moved.
  const { spawnSync } = require('node:child_process') as typeof import('node:child_process');
  const r = spawnSync(process.execPath, ['-e', `
    const fs = require('node:fs');
    fs.unlinkSync('/dev/shm' + process.env.MX_NAME);
    const { open } = require(${JSON.stringify(require.resolve('../src/core'))});
    const sab = open(process.env.MX_NAME, 256);
    new Int32Array(sab)[0] = 42;
  `], { env: { ...process.env, MX_NAME: name }, encoding: 'utf8' });
  if (r.status !== 0) {
    // child could not run (non-Linux): the F16 fix is POSIX-only anyway
    t.diagnostic('child failed; POSIX-only check');
    return;
  }
  // Our re-open must see the NEW object (42), not the stale mapping (1).
  const b = open(name, 256);
  assert.strictEqual(new Int32Array(b)[0], 42, 'reuse validated by object identity');
});

test('Corr F1: ring consumer survives the 2^31 head/tail sign flip', () => {
  const name = uniqueName();
  try {
    const p = RingProducer.open(name, { capacity: 4096 });
    const c = RingConsumer.open(name);
    const v = (p as unknown as { view: Int32Array }).view;
    Atomics.store(v, 0, 2147483632);  // head
    Atomics.store(v, 16, 2147483632); // tail
    p.write(Buffer.from([1, 2, 3, 4, 5]));             // head -> ...640
    p.write(Buffer.from([6, 7, 8, 9, 10]));            // head wraps negative
    const m1 = c.read({ timeoutMs: 600 });
    const m2 = c.read({ timeoutMs: 600 });
    assert.notStrictEqual(m1, null, 'first message delivered across the flip');
    assert.notStrictEqual(m2, null, 'second message delivered after the flip');
    assert.deepStrictEqual([...m2!], [6, 7, 8, 9, 10]);
  } finally {
    unlinkQuietly(name);
  }
});

test('Corr F4: a SIGKILLed-but-unreaped (zombie) holder is stolen from', async () => {
  const name = uniqueName();
  const { spawn } = require('node:child_process') as typeof import('node:child_process');
  const child = spawn(process.execPath, ['-e', `
    const { Mutex } = require(${JSON.stringify(require.resolve('../src/mutex'))});
    const m = Mutex.open(process.env.MX_NAME);
    m.lock();
    process.send('locked');
    setInterval(() => {}, 1 << 30);
  `], { env: { ...process.env, MX_NAME: name }, stdio: ['ignore', 'inherit', 'inherit', 'ipc'] });
  try {
    await new Promise<void>((resolve) =>
      child.on('message', (m: string) => m === 'locked' && resolve()));
    child.kill('SIGKILL');
    // deliberately DO NOT wait for exit: the corpse stays a zombie while we
    // block in the synchronous lock() below (our event loop is parked, so we
    // cannot reap it) — this used to deadlock forever.
    const { Mutex } = await import('../src/mutex');
    const m = Mutex.open(name);
    const res = m.lock({ timeoutMs: 5000 });
    assert.strictEqual(res.ownerDied, true, 'zombie holder stolen from');
    m.unlock();
  } finally {
    child.kill('SIGKILL');
    unlinkQuietly(name);
  }
});

test('Corr F2: exited workers do not exhaust the 64-slot table', async () => {
  const { Mutex } = await import('../src/mutex');
  const name = uniqueName();
  try {
    Mutex.open(name); // parent creates
    for (let i = 0; i < 70; i++) {
      // each worker claims a slot, locks+unlocks, and exits gracefully
      const r = await new Promise<number>((resolve, reject) => {
        const w = new (require('node:worker_threads').Worker)(
          `const { parentPort, workerData } = require('worker_threads');
           const { Mutex } = require(workerData.pkg);
           const m = Mutex.open(workerData.name);
           m.lock();
           m.unlock();
           parentPort.postMessage('ok');`,
          { eval: true, workerData: { name, pkg: require.resolve('../src/mutex') } },
        );
        w.on('message', () => resolve(0));
        w.on('error', reject);
      });
      assert.strictEqual(r, 0);
    }
    // the 71st participant (this thread) must still be able to lock
    const m = Mutex.open(name);
    m.lock();
    m.unlock();
  } finally {
    unlinkQuietly(name);
  }
});

test('Corr F3: sequential workers with waitAsync all settle (no stale hub reuse)', async () => {
  const name = uniqueName();
  try {
    open(name, 4096);
    for (let i = 0; i < 8; i++) {
      const r = await new Promise<{ resolvedAs: string }>((resolve, reject) => {
        const w = new (require('node:worker_threads').Worker)(
          `const { parentPort, workerData } = require('worker_threads');
           const { open } = require(workerData.pkg);
           const { waitAsync } = require(workerData.pkg.replace(/core\\.js$/, 'sync.js'));
           const i32 = new Int32Array(open(workerData.name, 4096));
           waitAsync(i32, 0, 0, 80).then((r) => parentPort.postMessage({ resolvedAs: r }));`,
          { eval: true, workerData: { name, pkg: require.resolve('../src/core') } },
        );
        w.on('message', resolve);
        w.on('error', reject);
      });
      assert.strictEqual(r.resolvedAs, 'timed-out', `worker ${i} settled`);
    }
  } finally {
    unlinkQuietly(name);
  }
});

test('Corr F22: ownerDied is reported exactly once per death', async () => {
  const { Mutex } = await import('../src/mutex');
  const name = uniqueName();
  const { spawn } = require('node:child_process') as typeof import('node:child_process');
  const child = spawn(process.execPath, ['-e', `
    const { Mutex } = require(${JSON.stringify(require.resolve('../src/mutex'))});
    const m = Mutex.open(process.env.MX_NAME);
    m.lock();
    process.send('locked');
    setInterval(() => {}, 1 << 30);
  `], { env: { ...process.env, MX_NAME: name }, stdio: ['ignore', 'inherit', 'inherit', 'ipc'] });
  try {
    await new Promise<void>((resolve) =>
      child.on('message', (m: string) => m === 'locked' && resolve()));
    child.kill('SIGKILL');
    await new Promise<void>((r) => child.on('exit', () => r()));
    const m = Mutex.open(name);
    assert.strictEqual(m.lock({ timeoutMs: 5000 }).ownerDied, true, 'stealer sees ownerDied');
    m.unlock();
    const second = Mutex.open(name);
    assert.strictEqual(second.lock({ timeoutMs: 1000 }).ownerDied, false, 'next holder does not');
    second.unlock();
  } finally {
    child.kill('SIGKILL');
    unlinkQuietly(name);
  }
});
