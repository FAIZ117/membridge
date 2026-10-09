// sync.test.ts — §13 sync tests: the F1 regression (cross-process wake lands
// well before the timeout), cross-process waitAsync with the futex_waitv
// multiplexer, no libuv-pool consumption, worker .terminate() teardown with a
// pending waitAsync (promise resolved 'timed-out', mapping pin released), and
// API basics.
//
// Multi-process tests re-enter this file via fork()/spawnSync() with
// MEMBRIDGE_TEST_ROLE set; children run their role and never register tests.

import { test } from 'node:test';
import { fork, spawnSync } from 'node:child_process';
import { Worker } from 'node:worker_threads';
import { open, unlink } from '../src/core';
import * as sync from '../src/sync';
import { assert, uniqueName, holdForTest, unlinkQuietly, assertThrowsCode } from './helpers';

const PKG = require.resolve('../src/core');
const ROLE = process.env.MEMBRIDGE_TEST_ROLE;
const ARG = process.env.MEMBRIDGE_TEST_ARG ?? '';

function unlinkLocal(name: string): void {
  try {
    unlink(name);
  } catch {
    /* ok */
  }
}

// ---- child roles ------------------------------------------------------------

function childMain(role: string, arg: string): void {
  const { open } = require(PKG);
  const syncChild = require(PKG.replace(/core\.js$/, 'sync.js')) as typeof import('../src/sync');
  // join-only: the parent creates the segment; a straggler child must not
  // re-create a segment the parent already cleaned up (/dev/shm hygiene).
  const sab = open(arg, 4096, { mode: 'join' });
  const i32 = new Int32Array(sab);

  if (role === 'sync-wait') {
    process.send!({ type: 'ready' });
    const t0 = Date.now();
    const r = syncChild.wait(i32, 0, 0, 3000);
    process.send!({ type: 'result', result: r, elapsedMs: Date.now() - t0 });
    process.exit(0);
  }
  if (role === 'async-mux') {
    process.send!({ type: 'ready' });
    let delivered = 0;
    const t1 = Date.now();
    syncChild.waitAsync(i32, 1, 0, 1500).then((r) => {
      process.send!({ type: 'r1', result: r, elapsedMs: Date.now() - t1 });
      if (++delivered === 2) process.exit(0);
    });
    const t0 = Date.now();
    syncChild.waitAsync(i32, 0, 0, 700).then((r) => {
      process.send!({ type: 'r0', result: r, elapsedMs: Date.now() - t0 });
      if (++delivered === 2) process.exit(0);
    });
    setTimeout(() => process.exit(3), 8000); // watchdog; loop held by waits
    return;
  }
  if (role === 'pool-starvation') {
    const waits: Promise<string>[] = [];
    for (let i = 0; i < 6; i++) waits.push(syncChild.waitAsync(i32, i, 0, 300));
    const fs = require('node:fs');
    const t0 = Date.now();
    fs.statSync(__filename);
    const fsMs = Date.now() - t0;
    Promise.all(waits).then((rs) => {
      process.send!({ type: 'result', fsMs, allTimedOut: rs.every((r) => r === 'timed-out') });
      process.exit(0);
    });
    setTimeout(() => process.exit(3), 10000);
    return;
  }
  if (role === 'terminate-pin') {
    const w = new Worker(
      `const { parentPort, workerData } = require('worker_threads');
       const { open } = require(workerData.pkg);
       const sync = require(workerData.pkg.replace(/core\\.js$/, 'sync.js'));
       const i32 = new Int32Array(open(workerData.name, 4096));
       sync.waitAsync(i32, 0, 0, 60000).then((r) => parentPort.postMessage({ resolvedAs: r }));
       parentPort.postMessage({ parked: true });`,
      { eval: true, workerData: { name: arg, pkg: PKG } },
    );
    w.on('message', (m: { parked?: boolean }) => {
      if (m.parked) {
        setTimeout(() => {
          w.terminate().then(() => {
            const { debugRegistryHas } = require(PKG);
            const gc = globalThis.gc as (() => void) | undefined;
            for (let i = 0; i < 6; i++) gc?.();
            console.log('PINRESULT ' + JSON.stringify({ stillPinned: debugRegistryHas(arg) }));
            process.exit(0);
          });
        }, 100);
      }
    });
    setTimeout(() => process.exit(3), 15000);
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
  // async roles fall through here with a live loop; nothing else runs
} else {
  registerTests();
}

function forkChild(role: string, arg: string): ReturnType<typeof fork> {
  return fork(__filename, {
    env: { ...process.env, MEMBRIDGE_TEST_ROLE: role, MEMBRIDGE_TEST_ARG: arg },
    stdio: 'inherit',
  });
}

function waitFor(child: ReturnType<typeof fork>, type: string, timeoutMs = 10000): Promise<any> {
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

// ---- parent tests -----------------------------------------------------------

function registerTests(): void {
  test('F1 regression: sync.notify wakes a waiter in ANOTHER process well before its timeout', async (t) => {
    const name = uniqueName();
    const child = forkChild('sync-wait', name);
    try {
      holdForTest(t, open(name, 4096));
      await waitFor(child, 'ready');
      await new Promise((r) => setTimeout(r, 200)); // child is parked by now
      const i32 = new Int32Array(open(name, 4096));
      sync.notify(i32, 0, 1); // void per §6; the wake is proven by the child
      const result = await waitFor(child, 'result');
      assert.strictEqual(result.result, 'ok');
      assert.ok(result.elapsedMs < 1500, `wake took ${result.elapsedMs} ms (F1 regression)`);
    } finally {
      child.kill('SIGKILL');
      unlinkLocal(name);
    }
  });

  test('cross-process waitAsync: muxed wake + timeout concurrently', async (t) => {
    const name = uniqueName();
    const child = forkChild('async-mux', name);
    try {
      holdForTest(t, open(name, 4096));
      await waitFor(child, 'ready');
      await new Promise((r) => setTimeout(r, 200));
      const i32 = new Int32Array(open(name, 4096));
      sync.notify(i32, 1, 1); // wake the short-timeout wait
      const r1 = await waitFor(child, 'r1');
      assert.strictEqual(r1.result, 'ok', 'woken wait resolved ok');
      assert.ok(r1.elapsedMs < 1500, `wake took ${r1.elapsedMs} ms`);
      const r0 = await waitFor(child, 'r0');
      assert.strictEqual(r0.result, 'timed-out', 'other wait timed out independently');
    } finally {
      child.kill('SIGKILL');
      unlinkLocal(name);
    }
  });

  test('waitAsync does not consume libuv pool threads (UV_THREADPOOL_SIZE=1 child)', async (t) => {
    const name = uniqueName();
    const child = forkChild('pool-starvation', name);
    try {
      holdForTest(t, open(name, 4096));
      const result = await waitFor(child, 'result', 15000);
      assert.ok(result.fsMs < 200, `fs.statSync took ${result.fsMs} ms while 6 waits pended`);
      assert.strictEqual(result.allTimedOut, true);
    } finally {
      child.kill('SIGKILL');
      unlinkLocal(name);
    }
  });

  test('worker .terminate() with a pending waitAsync: teardown resolves, pin released', (t) => {
    const name = uniqueName();
    try {
      holdForTest(t, open(name, 4096));
      const r = spawnSync(process.execPath, ['--expose-gc', __filename], {
        env: { ...process.env, MEMBRIDGE_TEST_ROLE: 'terminate-pin', MEMBRIDGE_TEST_ARG: name },
        encoding: 'utf8',
        timeout: 20000,
      });
      assert.ok(
        r.stdout.includes('"stillPinned":false'),
        `pin not released (exit ${r.status}): ${r.stdout} ${r.stderr}`,
      );
    } finally {
      unlinkLocal(name);
    }
  });

  test('sync basics: not-equal, timed-out, validation', () => {
    const name = uniqueName();
    try {
      const i32 = new Int32Array(open(name, 4096));
      i32[0] = 5;
      assert.strictEqual(sync.wait(i32, 0, 0, 100), 'not-equal');
      const t0 = Date.now();
      assert.strictEqual(sync.wait(i32, 1, 0, 120), 'timed-out');
      assert.ok(Date.now() - t0 >= 100, 'timeout actually waited');
      assertThrowsCode(
        () => sync.wait(new Float64Array(4) as unknown as Int32Array, 0, 0, 10),
        'E_NAME_INVALID',
      );
      assertThrowsCode(() => sync.wait(i32, 99999, 0, 10), 'E_NAME_INVALID');
    } finally {
      unlinkLocal(name);
    }
  });

  test('sync.wait wakes from a worker isolate (worker stores + notifies)', async () => {
    const name = uniqueName();
    try {
      const i32 = new Int32Array(open(name, 4096));
      const w = new Worker(
        `const { workerData } = require('worker_threads');
         const { open } = require(workerData.pkg);
         const sync = require(workerData.pkg.replace(/core\\.js$/, 'sync.js'));
         const i32 = new Int32Array(open(workerData.name, 4096));
         setTimeout(() => { Atomics.store(i32, 0, 9); sync.notify(i32, 0, 1); }, 100);`,
        { eval: true, workerData: { name, pkg: PKG } },
      );
      void w;
      const t0 = Date.now();
      const r = sync.wait(i32, 0, 0, 2000); // blocks this thread until notify
      assert.strictEqual(r, 'ok');
      assert.ok(Date.now() - t0 < 1500, `wake took ${Date.now() - t0} ms`);
      assert.strictEqual(i32[0], 9, 'the store landed before the wake');
    } finally {
      unlinkLocal(name);
    }
  });

  test('async wait cap: over-cap waiters reject with E_TOO_MANY_WAITERS', async () => {
    const name = uniqueName();
    try {
      const i32 = new Int32Array(open(name, 4096)); // 1024 words
      const waits: Promise<{ ok: boolean; code?: string; r?: string }>[] = [];
      for (let i = 0; i < 200; i++) {
        waits.push(
          sync.waitAsync(i32, i, 0, 500).then(
            (r) => ({ ok: true, r }),
            (e) => ({ ok: false, code: e.code }),
          ),
        );
      }
      const results = await Promise.all(waits);
      const rejected = results.filter((r) => !r.ok);
      assert.ok(rejected.length > 0, 'expected some over-cap rejections');
      assert.ok(rejected.every((r) => r.code === 'E_TOO_MANY_WAITERS'));
      const fulfilled = results.filter((r) => r.ok);
      assert.ok(fulfilled.every((r) => r.r === 'timed-out'));
    } finally {
      unlinkLocal(name);
    }
  });
}
