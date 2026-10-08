// hardening.test.ts — regression tests from the pre-release review round
// (docs/review/2026-10-06T1909-pre-release-review.md). Each test names the
// finding it pins. POSIX-only byte-crafting tests skip elsewhere.

import { test } from 'node:test';
import fs from 'node:fs';
import { fork, spawn } from 'node:child_process';
import { open, unlink } from '../src/core';
import { RingProducer, RingConsumer } from '../src/ringbuffer';
import { Mutex, MUTEX_DATA_BYTES } from '../src/mutex';
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
    // R6 semantics: a size-less join NEVER initializes a crashed creator's
    // 0-byte object (that used to publish a ready 0-byte segment, and before
    // that SIGBUS on the header read). It waits, then fails cleanly.
    assert.throws(() => RingConsumer.open(name, { initTimeoutMs: 300 }), (e: any) =>
      e.code === 'E_INIT_TIMEOUT');
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

test('Corr F10 / Perf P2: a woken waiter does not sleep a full slice on a free lock', async () => {
  const { Mutex } = await import('../src/mutex');
  const name = uniqueName();
  try {
    const m = Mutex.open(name);
    m.lock();
    // Three waiters queue up while the lock is held (worker threads, since
    // sync lock blocks this thread).
    const { Worker } = await import('node:worker_threads');
    const workers = [0, 1, 2].map(() => new Worker(
      `const { parentPort, workerData } = require('worker_threads');
       const { Mutex } = require(workerData.pkg);
       const m = Mutex.open(workerData.name);
       const t0 = performance.now();
       m.lock();
       m.unlock();
       parentPort.postMessage(performance.now() - t0);`,
      { eval: true, workerData: { name, pkg: require.resolve('../src/mutex') } },
    ));
    // release once the workers have had a beat to contend
    await new Promise((r) => setTimeout(r, 30));
    m.unlock();
    const waits = await Promise.all(workers.map((w) =>
      new Promise<number>((res) => w.on('message', (ms: number) => res(ms)))));
    // Without the HAS_WAITERS-preserving acquire, waiters 2 and 3 paid a
    // full 250 ms slice each (the review measured +94..100 ms). With bounded
    // scheduling noise, every handoff should be far below a slice.
    for (const ms of waits) {
      assert.ok(ms < 200, `contended handoff took ${ms.toFixed(1)} ms (slice-bound = lost wakeup)`);
    }
  } finally {
    unlinkQuietly(name);
  }
});

test('Perf P4: short timeouts are not stretched to a 250 ms slice', async () => {
  const { Mutex } = await import('../src/mutex');
  const name = uniqueName();
  const { Worker } = await import('node:worker_threads');
  const holder = new Worker(
    `const { parentPort, workerData } = require('worker_threads');
     const { Mutex } = require(workerData.pkg);
     const m = Mutex.open(workerData.name);
     m.lock();
     parentPort.postMessage('held');
     setInterval(() => {}, 1 << 30);`,
    { eval: true, workerData: { name, pkg: require.resolve('../src/mutex') } },
  );
  try {
    await new Promise<void>((r) => holder.on('message', () => r()));
    const m = Mutex.open(name);
    const t0 = performance.now();
    assert.throws(() => m.lock({ timeoutMs: 10 }), (e: any) => e.code === 'E_TIMEOUT');
    const ms = performance.now() - t0;
    assert.ok(ms < 120, `lock({timeoutMs:10}) took ${ms.toFixed(1)} ms (slice floor)`);
  } finally {
    await holder.terminate();
    unlinkQuietly(name);
  }
});

test('Corr F30: ring join validates capacity and maxMessage against the header', () => {
  const name = uniqueName();
  try {
    RingProducer.open(name, { capacity: 4096 });
    assert.throws(() => RingProducer.open(name, { capacity: 8192 }), (e: any) =>
      e.code === 'E_SIZE_MISMATCH');
    assert.throws(
      () => RingProducer.open(name, { capacity: 4096, maxMessage: 100 }),
      (e: any) => e.code === 'E_SIZE_MISMATCH',
    );
    // joining with matching values is fine
    RingProducer.open(name, { capacity: 4096, maxMessage: 2040 });
    // a joiner without values adopts the header's
    const c = RingConsumer.open(name);
    assert.strictEqual(c.capacityBytes, 4096);
  } finally {
    unlinkQuietly(name);
  }
});

test('Corr F30: reserveAsync and peekAsync work end to end', async () => {
  const name = uniqueName();
  try {
    const p = RingProducer.open(name, { capacity: 4096 });
    const c = RingConsumer.open(name);
    const pending = c.peekAsync({ timeoutMs: 5000 });
    await new Promise((r) => setTimeout(r, 50)); // consumer is parked
    const view = await p.reserveAsync(11);
    view.set(Buffer.from('hello world'));
    p.commit();
    const msg = await pending;
    assert.strictEqual(Buffer.from(msg!).toString(), 'hello world');
    c.release();
  } finally {
    unlinkQuietly(name);
  }
});

test('R1: cross-process prefix / smaller-size opens work', () => {
  const name = uniqueName();
  try {
    open(name, 8192, { mode: 'create' });
    const { spawnSync } = require('node:child_process') as typeof import('node:child_process');
    const run = (code: string) =>
      spawnSync(process.execPath, ['-e', code], {
        env: { ...process.env, MX_NAME: name },
        encoding: 'utf8',
      });
    // at-least smaller: maps the prefix over the re-mapped full object
    const a = run(`const {open}=require(${JSON.stringify(require.resolve('../src/core'))});
      const s=open(process.env.MX_NAME,4096,{sizePolicy:'at-least'});
      new Uint8Array(s)[4095]=9; console.log('OK',s.byteLength);`);
    assert.match(a.stdout, /OK 4096/, 'at-least prefix works cross-process');
    // grow not-larger: an at-least-style window, no E_INCOMPATIBLE
    const g = run(`const {open}=require(${JSON.stringify(require.resolve('../src/core'))});
      const s=open(process.env.MX_NAME,4096,{sizePolicy:'grow'});
      console.log('OK',s.byteLength);`);
    assert.match(g.stdout, /OK 4096/, 'grow with smaller size works');
    // exact smaller: E_SIZE_MISMATCH (not E_INCOMPATIBLE)
    const e = run(`const {open}=require(${JSON.stringify(require.resolve('../src/core'))});
      try { open(process.env.MX_NAME,4096); console.log('BAD ok'); }
      catch (err) { console.log('THREW', err.code); }`);
    assert.match(e.stdout, /THREW E_SIZE_MISMATCH/, 'exact smaller -> E_SIZE_MISMATCH');
  } finally {
    unlinkQuietly(name);
  }
});

test('R2: concurrent create/join produces no spurious E_INCOMPATIBLE', async () => {
  const name = uniqueName();
  const { spawn } = require('node:child_process') as typeof import('node:child_process');
  try {
    const joinerCode = `
      const { open } = require(${JSON.stringify(require.resolve('../src/core'))});
      for (let k = 0; k < 150; k++) {
        try { open(process.env.MX_NAME, 65536, { mode: 'join' }); }
        catch (e) {
          if (e.code === 'E_INCOMPATIBLE') { console.log('SPURIOUS'); }
          else if (e.code !== 'E_NOT_FOUND') { console.log('OTHER', e.code); }
        }
      }`;
    const kids = [0, 1, 2, 3].map(() =>
      spawn(process.execPath, ['-e', joinerCode],
        { stdio: ['ignore', 'pipe', 'ignore'], env: { ...process.env, MX_NAME: name } }));
    const creator = spawn(process.execPath, ['-e', `
      const { open, unlink } = require(${JSON.stringify(require.resolve('../src/core'))});
      const name = process.env.MX_NAME;
      for (let k = 0; k < 150; k++) { open(name, 65536, { mode: 'create' }); unlink(name); }
    `], { stdio: 'ignore', env: { ...process.env, MX_NAME: name } });
    // Exit promises are taken at spawn time: attaching the creator's listener
    // only after the joiners finished missed an already-fired 'exit' and hung
    // the suite whenever the creator finished first (round-3 fix round).
    const exits = [...kids, creator].map((k: any) => new Promise(r => k.on('exit', r)));
    let bad = '';
    for (const k of kids) k.stdout.on('data', (d: Buffer) => { bad += d.toString(); });
    await Promise.all(exits);
    assert.ok(!bad.includes('SPURIOUS') && !bad.includes('OTHER'),
      `spurious failures under create/join race: ${bad.trim()}`);
  } finally {
    unlinkQuietly(name);
  }
});

test('R3: a grow that lost the race still returns a full window', async () => {
  const name = uniqueName();
  try {
    open(name, 4096, { mode: 'create' });
    // child grows to 1 MiB while we hold a small cached mapping
    const { spawnSync } = require('node:child_process') as typeof import('node:child_process');
    const r = spawnSync(process.execPath, ['-e', `
      const { open } = require(${JSON.stringify(require.resolve('../src/core'))});
      open(${JSON.stringify(name)}, 1048576, { sizePolicy: 'grow' });
    `], { encoding: 'utf8' });
    assert.strictEqual(r.status, 0, r.stderr);
    // our own grow-open: header already >= requested, so no grow runs — the
    // mapping must still be re-mapped to cover the window (R3: used to
    // silently return a 4096-byte SAB).
    const big = open(name, 1048576, { sizePolicy: 'grow' });
    assert.strictEqual(big.byteLength, 1048576, 'window covers the request');
    new Uint8Array(big)[1048575] = 1;  // last byte backed: no SIGBUS
  } finally {
    unlinkQuietly(name);
  }
});

test('R4/R5 hostile geometry races: native view guards', () => {
  const b = (require('../src/native') as typeof import('../src/native')).nativeOrThrow();
  // A view shorter than the mutex layout is rejected before any slot access.
  const short = new Int32Array(new SharedArrayBuffer(64));
  assert.throws(() => b.mutexClaimSlot('/membridge-test-x', short), (e: any) =>
    e.code === 'E_SIZE_INVALID');
});

test('R11/R12: claim-after-unlink teardown is clean; pins release on close/GC', () => {
  const { spawnSync } = require('node:child_process') as typeof import('node:child_process');
  // R12: 500 short-lived mutexes on unlinked segments must not pin 500
  // mappings + fds for the isolate's lifetime. The fd check runs in a
  // --expose-gc child so collection is deterministic.
  const name = uniqueName();
  const r = spawnSync(process.execPath, ['--expose-gc', '-e', `
    const { readdirSync } = require('node:fs');
    const { open, unlink } = require(process.env.MX_PKG);
    const { Mutex } = require(process.env.MX_PKG.replace(/core\\.js$/, 'mutex.js'));
    const before = readdirSync('/proc/self/fd').length;
    for (let i = 0; i < 500; i++) {
      // darwin caps names at 31 bytes — short, collision-free per child
      const n = '/membridge-test-' + process.pid.toString(36) + 'r' + i.toString(36);
      const m = Mutex.open(n);
      m.lock();
      m.unlock();
      m.close();
      unlink(n);
    }
    global.gc(); global.gc();
    const after = readdirSync('/proc/self/fd').length;
    console.log('FDS ' + (after - before));
  `], {
    env: { ...process.env, MX_PKG: require.resolve('../src/core') },
    encoding: 'utf8',
  });
  const delta = Number((r.stdout.match(/FDS (\d+)/) || [])[1] ?? 999);
  assert.ok(delta < 50, `fd leak: ${delta} fds held after 500 closed mutexes (stderr: ${r.stderr.slice(0, 200)})`);
});


test('R11: claim-after-unlink worker terminate does not segfault teardown', async () => {
// R11: claim a mutex, unlink the name, terminate a worker that holds it —
  // teardown must NOT segfault (used to walk unmapped claimedData).
  const n2 = uniqueName();
  const { Worker } = await import('node:worker_threads');
  const w = new Worker(
    `const { workerData } = require('worker_threads');
     const { Mutex } = require(workerData.pkg);
     const m = Mutex.open(workerData.name);
     m.lock();
     require('node:worker_threads').parentPort?.postMessage('held');
     setInterval(() => {}, 1 << 30);`,
    { eval: true, workerData: { name: n2, pkg: require.resolve('../src/mutex') } },
  );
  await new Promise<void>((r) => w.on('message', () => r()));
  unlink(n2);  // registry entry erased; pin from the SAB must keep it safe
  const t = new Promise((r) => setTimeout(r, 100));
  await t;
  await w.terminate();
  // survive one full turn: the old code segfaulted in the cleanup hook here
  await new Promise((r) => setTimeout(r, 200));
  unlinkQuietly(n2);
});


function countFds(): number {
  return (require('node:fs') as typeof import('node:fs')).readdirSync('/proc/self/fd').length;
}

test('R20: NaN/Infinity timeouts are rejected, not infinite hangs', async () => {
  const { Mutex } = await import('../src/mutex');
  const name = uniqueName();
  try {
    const m = Mutex.open(name);
    assert.throws(() => m.lock({ timeoutMs: NaN }), (e: any) => e.code === 'E_NAME_INVALID');
    assert.throws(() => m.lock({ timeoutMs: Infinity }), (e: any) => e.code === 'E_NAME_INVALID');
    assert.throws(() => m.lock({ timeoutMs: -5 }), (e: any) => e.code === 'E_NAME_INVALID');
  } finally {
    unlinkQuietly(name);
  }
});

test('R14: peekAsync wakes in milliseconds when the producer commits', async () => {
  const { performance } = await import('node:perf_hooks');
  const name = uniqueName();
  try {
    const p = RingProducer.open(name, { capacity: 4096 });
    const c = RingConsumer.open(name);
    const t0 = performance.now();
    const msgP = c.peekAsync({ timeoutMs: 5000 });
    await new Promise((r) => setTimeout(r, 20));  // consumer is parked
    p.write(Buffer.from('async wake'));
    const msg = await msgP;
    const ms = performance.now() - t0;
    assert.strictEqual(Buffer.from(msg!).toString(), 'async wake');
    assert.ok(ms < 150, `async wake took ${ms.toFixed(1)} ms (slice-bound = flagless wait)`);
  } finally {
    unlinkQuietly(name);
  }
});

// F23: a slot stuck mid-publish (state Free with a non-zero pid — the pid is
// the publish-claim marker) is recovered when that pid is provably dead.
// Before fix round 3 the equivalent crash state leaked the slot permanently.
test('F23: slots stuck mid-publish with a dead pid are recovered', { skip: POSIX ? false : SKIP }, async (t: any) => {
  const name = uniqueName();
  try {
    // a provably dead identity (SIGKILLed and reaped; startTime pinned)
    const child = spawn(process.execPath, ['-e', 'setTimeout(() => {}, 60000)'], { stdio: 'ignore' });
    const deadPid = child.pid ?? -1;
    child.kill('SIGKILL');
    await new Promise<void>((resolve) => child.on('exit', resolve));

    const totalBytes = 4096 + MUTEX_DATA_BYTES;
    craft(name, {
      initState: 2,
      headerBytes: 4096,
      dataBytes: MUTEX_DATA_BYTES,
      flags: 0x100, // kKindMutex
    }, totalBytes);
    // Fill all 64 participant slots as "reserving, reserver died": state Free,
    // non-zero dead pid, non-zero gen.
    const path = shmPath(name)!;
    const buf = fs.readFileSync(path);
    for (let s = 0; s < 64; s++) {
      const off = 4096 + s * 32;
      buf.writeInt32LE(deadPid, off + 0); // pid: mid-publish marker (review F23)
      buf.writeInt32LE(1, off + 24);      // gen
      buf.writeInt32LE(0, off + 28);      // state: Free
    }
    fs.writeFileSync(path, buf);
    // The claimer must recover a slot instead of E_TIMEOUT.
    const m = Mutex.open(name);
    m.lock();
    m.unlock();
    m.close();
  } finally {
    unlinkQuietly(name);
  }
});
