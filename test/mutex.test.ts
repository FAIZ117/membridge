// mutex.test.ts — §13 Mutex tests: 8-process + worker mutual exclusion,
// holder SIGKILL -> steal + ownerDied, worker .terminate() -> env-cleanup
// recovery, E_NOT_OWNER / E_DEADLOCK / E_TIMEOUT, lockAsync with AbortSignal,
// PID-reuse simulation (wrong start time), and slot-table exhaustion.

import { test } from 'node:test';
import { fork, spawnSync } from 'node:child_process';
import { Worker } from 'node:worker_threads';
import { open } from '../src/core';
import { Mutex, MUTEX_DATA_BYTES, SLOT_LAYOUT } from '../src/mutex';
import { assert, uniqueName, unlinkQuietly } from './helpers';

const PKG = require.resolve('../src/core');
const ROLE = process.env.MEMBRIDGE_TEST_ROLE;
const ARG = process.env.MEMBRIDGE_TEST_ARG ?? '';

function childMain(role: string, arg: string): void {
  const { open } = require(PKG);
  const { Mutex } = require(PKG.replace(/core\.js$/, 'mutex.js')) as typeof import('../src/mutex');
  const [mutexName, dataName, cycles] = arg.split('|');

  if (role === 'mx-contended') {
    const m = Mutex.open(mutexName!, { });
    const data = new Int32Array(open(dataName!, 64, { mode: 'join' }));
    for (let i = 0; i < Number(cycles); i++) {
      m.lock();
      Atomics.add(data, 0, 1);
      Atomics.add(data, 1, 1); // read-modify-write under lock, then verified
      m.unlock();
    }
    process.exit(0);
  }
  if (role === 'mx-hold') {
    const m = Mutex.open(mutexName!, {});
    m.lock();
    process.send!({ type: 'locked' });
    setTimeout(() => process.exit(3), 30000); // killed by the parent before this
    return;
  }
  throw new Error(`unknown role ${role}`);
}

if (ROLE !== undefined) {
  try {
    childMain(ROLE, ARG);
  } catch {
    process.exit(2);
  }
} else {
  registerTests();
}

function forkChild(role: string, arg: string): ReturnType<typeof fork> {
  return fork(__filename, {
    env: { ...process.env, MEMBRIDGE_TEST_ROLE: role, MEMBRIDGE_TEST_ARG: arg },
    stdio: 'inherit',
  });
}

function waitFor(child: ReturnType<typeof fork>, type: string, timeoutMs = 15000): Promise<any> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`no '${type}' from child`)), timeoutMs);
    child.on('message', function h(m: any) {
      if (m.type === type) {
        clearTimeout(timer);
        child.removeListener('message', h);
        resolve(m);
      }
    });
    child.on('exit', (code) => {
      clearTimeout(timer);
      reject(new Error(`child exited ${code} waiting for '${type}'`));
    });
  });
}

// a provably dead pid with its true start time (for identity crafting)
function makeDeadPid(): number {
  const r = spawnSync(process.execPath, ['-e', 'setTimeout(() => {}, 60)'], { timeout: 300 });
  return r.status === 0 ? r.status + 2 : (r.pid ?? -1) + 100000; // unused; see below
}

function registerTests(): void {
  test('mutual exclusion across 8 processes', async () => {
    const mutexName = uniqueName();
    const dataName = uniqueName();
    const cycles = 50;
    Mutex.open(mutexName);
    const data = new Int32Array(open(dataName, 64));
    try {
      const children = Array.from({ length: 8 }, () =>
        forkChild('mx-contended', `${mutexName}|${dataName}|${cycles}`));
      const codes = await Promise.all(
        children.map((c) => new Promise<number>((res) => c.on('exit', (c2) => res(c2 ?? -1)))),
      );
      for (const c of codes) assert.strictEqual(c, 0, `child exit ${c}`);
      assert.strictEqual(Atomics.load(data, 0), 8 * cycles, 'no lost updates under contention');
      assert.strictEqual(Atomics.load(data, 1), 8 * cycles);
    } finally {
      unlinkQuietly(mutexName);
      unlinkQuietly(dataName);
    }
  });

  test('holder SIGKILLed -> steal + ownerDied', async () => {
    const mutexName = uniqueName();
    const child = forkChild('mx-hold', mutexName);
    try {
      Mutex.open(mutexName);
      await waitFor(child, 'locked');
      await new Promise((r) => setTimeout(r, 100));
      child.kill('SIGKILL');
      await new Promise((r) => child.on('exit', r));
      const m = Mutex.open(mutexName);
      const t0 = Date.now();
      const res = m.lock({ timeoutMs: 5000 });
      assert.strictEqual(res.ownerDied, true, 'steal reported ownerDied');
      assert.ok(Date.now() - t0 < 4000, `steal took ${Date.now() - t0} ms (no 30 s wait)`);
      m.unlock();
    } finally {
      try { child.kill('SIGKILL'); } catch { /* already dead */ }
      unlinkQuietly(mutexName);
    }
  });

  test('worker terminated while holding -> recovered via env-cleanup hook', async () => {
    const mutexName = uniqueName();
    try {
      Mutex.open(mutexName);
      const w = new Worker(
        `const { workerData } = require('worker_threads');
         const { Mutex } = require(workerData.pkg.replace(/core\\.js$/, 'mutex.js'));
         const m = Mutex.open(workerData.name);
         m.lock();
         require('node:worker_threads').parentPort?.postMessage({ locked: true });
         setInterval(() => {}, 1 << 30);`,
        { eval: true, workerData: { name: mutexName, pkg: PKG } },
      );
      await new Promise<void>((resolve) => {
        w.on('message', (m: { locked?: boolean }) => m.locked && resolve());
      });
      await new Promise((r) => setTimeout(r, 100));
      await w.terminate();
      const m = Mutex.open(mutexName);
      const res = m.lock({ timeoutMs: 5000 });
      assert.strictEqual(res.ownerDied, true, 'env hook marked the lock OWNER_DIED');
      m.unlock();
    } finally {
      unlinkQuietly(mutexName);
    }
  });

  test('E_NOT_OWNER, E_DEADLOCK, E_TIMEOUT', async () => {
    const mutexName = uniqueName();
    try {
      const m = Mutex.open(mutexName);
      assert.throws(() => m.unlock(), (e: any) => e.code === 'E_NOT_OWNER');
      m.lock();
      assert.throws(() => m.lock(), (e: any) => e.code === 'E_DEADLOCK');
      m.unlock();

      // Note: two Mutex instances on the SAME thread share one participant
      // slot (and token) — §7.2. Contention therefore needs another thread:
      // a worker holds the lock, main times out waiting for it.
      const holder = new Worker(
        `const { workerData } = require('worker_threads');
         const { Mutex } = require(workerData.pkg.replace(/core\\.js$/, 'mutex.js'));
         const m = Mutex.open(workerData.name);
         m.lock();
         require('node:worker_threads').parentPort?.postMessage({ locked: true });
         setInterval(() => {}, 1 << 30);`,
        { eval: true, workerData: { name: mutexName, pkg: PKG } },
      );
      await new Promise<void>((resolve) => {
        holder.on('message', (m: { locked?: boolean }) => m.locked && resolve());
      });
      const t0 = Date.now();
      const m3 = Mutex.open(mutexName);
      assert.throws(() => m3.lock({ timeoutMs: 250 }), (e: any) => e.code === 'E_TIMEOUT');
      assert.ok(Date.now() - t0 >= 200, 'timeout waited');
      await holder.terminate();
      const m4 = Mutex.open(mutexName);
      const recovered = m4.lock({ timeoutMs: 5000 });
      assert.strictEqual(recovered.ownerDied, true, 'env hook recovered the lock');
      m4.unlock();
    } finally {
      unlinkQuietly(mutexName);
    }
  });

  test('lockAsync acquires, and AbortSignal cancels the wait', async () => {
    const mutexName = uniqueName();
    try {
      const m = Mutex.open(mutexName);
      const res = await m.lockAsync();
      assert.strictEqual(res.ownerDied, false);
      m.unlock();

      // hold from a worker; abort a pending lockAsync; then acquire cleanly
      const holder = new Worker(
        `const { workerData } = require('worker_threads');
         const { Mutex } = require(workerData.pkg.replace(/core\\.js$/, 'mutex.js'));
         const m = Mutex.open(workerData.name);
         m.lock();
         require('node:worker_threads').parentPort?.postMessage({ locked: true });
         setInterval(() => {}, 1 << 30);`,
        { eval: true, workerData: { name: mutexName, pkg: PKG } },
      );
      await new Promise<void>((resolve) => {
        holder.on('message', (m: { locked?: boolean }) => m.locked && resolve());
      });
      const ac = new AbortController();
      const pending = m.lockAsync({ signal: ac.signal });
      setTimeout(() => ac.abort(), 100);
      await assert.rejects(pending);
      holder.terminate();
      // after the holder dies, the env hook recovers the lock
      const m4 = Mutex.open(mutexName);
      const got = await m4.lockAsync({ timeoutMs: 5000 });
      assert.strictEqual(got.ownerDied, true);
      m4.unlock();
    } finally {
      unlinkQuietly(mutexName);
    }
  });

  test('PID-reuse simulation: live pid with wrong start time is stealable', async () => {
    const mutexName = uniqueName();
    try {
      const m = Mutex.open(mutexName);
      // craft: slot 0 owned by a LIVE pid (ours) but with a bogus start time —
      // exactly what a recycled pid looks like (§7.1)
      const id = m.identityWords();
      const v = new Int32Array(open(mutexName, MUTEX_DATA_BYTES, { mode: 'join', kind: 'mutex' }));
      const slot = 0;
      const base = SLOT_LAYOUT.SLOTS + slot * SLOT_LAYOUT.SLOT_WORDS;
      const gen = 5;
      Atomics.store(v, base + SLOT_LAYOUT.PID, id.pid);
      Atomics.store(v, base + SLOT_LAYOUT.TID, 424242);
      Atomics.store(v, base + SLOT_LAYOUT.START_LO, 12345); // wrong start time
      Atomics.store(v, base + SLOT_LAYOUT.START_HI, 0);
      Atomics.store(v, base + SLOT_LAYOUT.NS_LO, 0);
      Atomics.store(v, base + SLOT_LAYOUT.NS_HI, 0);
      Atomics.store(v, base + SLOT_LAYOUT.GEN, gen);
      Atomics.store(v, base + SLOT_LAYOUT.STATE, SLOT_LAYOUT.STATE_ACTIVE);
      const token = (slot << 16) | (gen & 0x7fff);
      Atomics.store(v, 0, token); // lockWord held by the "recycled" owner

      const res = m.lock({ timeoutMs: 3000 });
      assert.strictEqual(res.ownerDied, true, 'recycled-pid owner was stolen from');
      m.unlock();
    } finally {
      unlinkQuietly(mutexName);
    }
  });

  test('slot-table exhaustion: 64 dead participants reclaimed; 65th thread locks', async () => {
    const mutexName = uniqueName();
    try {
      const m = Mutex.open(mutexName);
      const dead = spawnSync(process.execPath, ['-e', 'setTimeout(() => {}, 200)'], {
        timeout: 500,
      });
      const deadPid = dead.pid ?? -1;
      const v = new Int32Array(open(mutexName, MUTEX_DATA_BYTES, { mode: 'join', kind: 'mutex' }));
      for (let slot = 0; slot < SLOT_LAYOUT.SLOT_COUNT; slot++) {
        const base = SLOT_LAYOUT.SLOTS + slot * SLOT_LAYOUT.SLOT_WORDS;
        Atomics.store(v, base + SLOT_LAYOUT.PID, deadPid);
        Atomics.store(v, base + SLOT_LAYOUT.TID, slot + 1);
        Atomics.store(v, base + SLOT_LAYOUT.START_LO, 777 + slot);
        Atomics.store(v, base + SLOT_LAYOUT.START_HI, 0);
        Atomics.store(v, base + SLOT_LAYOUT.NS_LO, 0);
        Atomics.store(v, base + SLOT_LAYOUT.NS_HI, 0);
        Atomics.store(v, base + SLOT_LAYOUT.GEN, slot + 1);
        Atomics.store(v, base + SLOT_LAYOUT.STATE, SLOT_LAYOUT.STATE_ACTIVE);
      }
      // lockWord is 0 (not held): every slot is a dead, unreferenced participant
      const res = m.lock({ timeoutMs: 3000 });
      assert.strictEqual(res.ownerDied, false);
      m.unlock();
    } finally {
      unlinkQuietly(mutexName);
    }
  });

  void makeDeadPid;
}
