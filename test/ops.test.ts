// ops.test.ts — §13 ops tests: list/stat/reap with live, dead and mixed
// attachers, and unlinkWhenUnused (the detach that empties the attach table
// unlinks the name — verified in --expose-gc children so GC drives the
// detach deterministically).

import { test } from 'node:test';
import { spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import { open, unlink } from '../src/core';
import { capacity, stat, list, reap } from '../src/ops';
import { assert, uniqueName, holdForTest, unlinkQuietly, buildHeader, shmPath, readStartTime } from './helpers';

const PKG = require.resolve('../src/core');
const LINUX = process.platform === 'linux';

test('capacity(): free/total of the shm store (Linux; E_UNSUPPORTED elsewhere)', () => {
  if (LINUX) {
    const c = capacity();
    assert.ok(c.totalBytes > 0);
    assert.ok(c.freeBytes > 0);
    assert.ok(c.freeBytes <= c.totalBytes);
  } else {
    assert.throws(() => capacity(), (e: any) => e.code === 'E_UNSUPPORTED');
  }
});

test('list(): magic-filtered; foreign files ignored', () => {
  if (!LINUX) return; // list is Linux-only (§9)
  const a = uniqueName();
  const b = uniqueName();
  const foreign = `/dev/shm/foreign-not-shm-bridge-${process.pid}`;
  try {
    open(a, 256);
    open(b, 256);
    fs.writeFileSync(foreign, Buffer.from('not a shm-bridge segment'));
    const names = list();
    assert.ok(names.includes(a), 'lists our segment');
    assert.ok(names.includes(b), 'lists the second segment');
    assert.ok(!names.includes(`/foreign-not-shm-bridge-${process.pid}`), 'foreign file filtered');
  } finally {
    unlinkQuietly(a);
    unlinkQuietly(b);
    try {
      fs.unlinkSync(foreign);
    } catch {
      /* ok */
    }
  }
});

test('stat(): kind, sizes, and our live attach row', (t) => {
  const name = uniqueName();
  try {
    holdForTest(t, open(name, 4096));
    const st = stat(name);
    assert.strictEqual(st.kind, 'plain');
    assert.strictEqual(st.dataBytes, 4096);
    assert.strictEqual(st.attachSlots.length, 1, 'one row: this process');
    assert.strictEqual(st.attachSlots[0]!.alive, true);
    assert.strictEqual(st.initState, 2);
  } finally {
    unlinkQuietly(name);
  }
});

test('stat(): kind markers for mutex and ring segments', (t) => {
  const mx = uniqueName();
  const rg = uniqueName();
  try {
    holdForTest(t, open(mx, 2064, { kind: 'mutex' }));
    assert.strictEqual(stat(mx).kind, 'mutex');
    holdForTest(t, open(rg, 256 + 4096, { kind: 'ring' }));
    assert.strictEqual(stat(rg).kind, 'ring');
  } finally {
    unlinkQuietly(mx);
    unlinkQuietly(rg);
  }
});

test('stat(): E_NOT_FOUND missing, E_INCOMPATIBLE foreign', () => {
  assert.throws(() => stat(uniqueName()), (e: any) => e.code === 'E_NOT_FOUND');
  if (!LINUX) return;
  const foreign = uniqueName();
  try {
    fs.writeFileSync(shmPath(foreign)!, Buffer.alloc(256));
    assert.throws(() => stat(foreign), (e: any) => e.code === 'E_INCOMPATIBLE');
  } finally {
    try {
      fs.unlinkSync(shmPath(foreign)!);
    } catch {
      /* ok */
    }
  }
});

/** A fully-reaped dead identity (a zombie still answers kill(pid,0) alive). */
async function makeDeadIdentity(): Promise<{ pid: number; startTime: number }> {
  const child = spawn(process.execPath, ['-e', 'setTimeout(() => {}, 60000)'], {
    stdio: 'ignore',
  });
  const pid = child.pid ?? -1;
  const startTime = readStartTime(pid);
  child.kill('SIGKILL');
  await new Promise<void>((resolve) => child.on('exit', () => resolve()));
  return { pid, startTime };
}

test('reap(): live attacher blocks; dead-only table reaps; dryRun reports', async (t) => {
  if (!LINUX) return;
  const live = uniqueName();
  const dead = uniqueName();
  try {
    holdForTest(t, open(live, 256)); // we are alive
    const res = reap({ name: live });
    assert.strictEqual(res[0]!.reaped, false, 'live attacher blocks the reap');
    assert.ok(list().includes(live), 'still there after a blocked reap');

    const { pid, startTime } = await makeDeadIdentity();
    fs.writeFileSync(
      shmPath(dead)!,
      buildHeader({
        initState: 2,
        dataBytes: 256,
        attachSlots: [{ pid, threadId: 0, startTime, pidNsInode: 0, refcount: 1 }],
      }),
    );
    const dry = reap({ name: dead, dryRun: true });
    assert.strictEqual(dry[0]!.reaped, true, 'dead-only table is reapable');
    assert.ok(list().includes(dead), 'dryRun left the name in place');
    const real = reap({ name: dead });
    assert.strictEqual(real[0]!.reaped, true);
    assert.ok(!list().includes(dead), 'name gone after reap');
  } finally {
    unlinkQuietly(live);
    unlinkQuietly(dead);
  }
});

test('reap(): mixed live/dead rows block the reap', async () => {
  if (!LINUX) return;
  const name = uniqueName();
  try {
    const { pid, startTime } = await makeDeadIdentity();
    fs.writeFileSync(
      shmPath(name)!,
      buildHeader({
        initState: 2,
        dataBytes: 256,
        attachSlots: [
          { pid, threadId: 0, startTime, pidNsInode: 0, refcount: 1 },
          { pid: process.pid, threadId: 0, startTime: -2, pidNsInode: 0, refcount: 1 },
        ],
      }),
    );
    const res = reap({ name });
    assert.strictEqual(res[0]!.reaped, false, 'a live row blocks even with dead rows present');
  } finally {
    unlinkQuietly(name);
  }
});

function runChild(script: string, name: string): { status: number | null; stdout: string; stderr: string } {
  const r = spawnSync(process.execPath, ['--expose-gc', '-e', script], {
    env: { ...process.env, SHM_BRIDGE_TEST_PKG: PKG, SHM_BRIDGE_TEST_NAME: name },
    encoding: 'utf8',
  });
  return { status: r.status, stdout: r.stdout, stderr: r.stderr };
}

test('unlinkWhenUnused: the detaching process that empties the table unlinks', (t) => {
  if (!LINUX) return; // asserts unlink visibility via /dev/shm (Linux paths)
  const parentName = uniqueName();
  const soleName = uniqueName();
  try {
    // parent holds an attach row: the child's flagged detach must NOT unlink
    holdForTest(t, open(parentName, 4096));
    const r = runChild(
      `
      const { open } = require(process.env.SHM_BRIDGE_TEST_PKG);
      let sab = open(process.env.SHM_BRIDGE_TEST_NAME, 4096, { unlinkWhenUnused: true });
      new Int32Array(sab)[0] = 5;
      sab = null;
      for (let i = 0; i < 6; i++) global.gc();
    `,
      parentName,
    );
    assert.strictEqual(r.status, 0, `child failed: ${r.stderr}`);
    assert.ok(list().includes(parentName), 'parent still attached -> name stays');

    // sole attacher with the flag: its detach empties the table -> unlink
    const r2 = runChild(
      `
      const { open } = require(process.env.SHM_BRIDGE_TEST_PKG);
      const { statSync } = require('node:fs');
      let sab = open(process.env.SHM_BRIDGE_TEST_NAME, 4096, { unlinkWhenUnused: true });
      new Int32Array(sab)[0] = 9;
      sab = null;
      for (let i = 0; i < 6; i++) global.gc();
      try {
        statSync('/dev/shm' + process.env.SHM_BRIDGE_TEST_NAME);
        console.log('STILL_THERE');
      } catch {
        console.log('GONE');
      }
    `,
      soleName,
    );
    assert.match(r2.stdout, /GONE/, 'sole-attacher detach with the flag unlinked the name');
  } finally {
    unlinkQuietly(parentName);
    unlinkQuietly(soleName);
  }
});
