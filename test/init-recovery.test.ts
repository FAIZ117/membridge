// init-recovery.test.ts — §13 crash recovery: creator died mid-init (joiner
// takes over, including the zero-length object case), E_INIT_TIMEOUT against
// a live initializer (then takeover after it dies), header magic/version
// mismatch -> E_INCOMPATIBLE, and the env-gated E_NO_SPACE tmpfs test.
//
// Crash states are hand-crafted by writing the §5.2 byte layout into the shm
// object directly — an outside cross-check of cc/header.h. POSIX-only; the
// tmpfs test additionally requires MEMBRIDGE_TEST_TMPFS (CI mounts one).

import { test } from 'node:test';
import type { TestContext } from 'node:test';
import fs from 'node:fs';
import { spawn } from 'node:child_process';
import { open, unlink } from '../src/core';
import {
  assert,
  uniqueName,
  buildHeader,
  readStartTime,
  shmPath,
  assertThrowsCode,
  unlinkQuietly,
} from './helpers';

const POSIX = process.platform === 'linux' || process.platform === 'darwin';
const SKIP = 'needs POSIX /dev/shm (byte-level crash-state crafting)';
const TMPFS = process.env.MEMBRIDGE_TEST_TMPFS !== undefined;
const SKIP_TMPFS = 'needs MEMBRIDGE_TEST_TMPFS pointing at a small tmpfs (CI mounts one)';

// Spawn a child that would live forever, record its pid + true start time,
// then SIGKILL it and wait for it to be reaped. Returns a provably dead
// identity (pid reuse cannot fool us: the start time is pinned).
async function makeDeadIdentity(): Promise<{ pid: number; startTime: number }> {
  const child = spawn(process.execPath, ['-e', 'setTimeout(() => {}, 60000)'], {
    stdio: 'ignore',
  });
  const pid = child.pid ?? -1;
  const startTime = readStartTime(pid);
  child.kill('SIGKILL');
  await new Promise<void>((resolve) => child.on('exit', resolve));
  return { pid, startTime };
}

async function makeLiveIdentity(): Promise<{ pid: number; startTime: number; child: ReturnType<typeof spawn> }> {
  const child = spawn(process.execPath, ['-e', 'setTimeout(() => {}, 60000)'], {
    stdio: 'ignore',
  });
  return { pid: child.pid ?? -1, startTime: readStartTime(child.pid ?? -1), child };
}

test('joiner takes over when the creator died before writing the header', { skip: POSIX ? false : SKIP }, async (t) => {
  const name = makeTrackedNameLocal(t);
  const path = shmPath(name);
  const headerBytes = 4096;
  const dataBytes = 4096;
  // Creator crashed after shm_open but before ftruncate AND before WriteHeader:
  // a zero-length object with no header at all.
  fs.writeFileSync(path!, Buffer.alloc(0));
  const sab = open(name, dataBytes); // join (file exists) -> takeover
  assert.strictEqual(sab.byteLength, dataBytes);
  new Int32Array(sab)[10] = 5;
  assert.strictEqual(new Int32Array(open(name, dataBytes))[10], 5);
  const onDisk = fs.readFileSync(path!);
  assert.strictEqual(onDisk.length, headerBytes + dataBytes, 'takeover sized the object');
});

test('joiner takes over a half-initialized header with a dead initializer', { skip: POSIX ? false : SKIP }, async (t) => {
  const name = makeTrackedNameLocal(t);
  const path = shmPath(name);
  const dead = await makeDeadIdentity();
  const ourNs = 0; // unknown to the test; CheckLiveness reads our ns itself
  fs.writeFileSync(
    path!,
    buildHeader({
      initState: 1,
      initializerSlot: 0,
      dataBytes: 4096,
      attachSlots: [
        { pid: dead.pid, threadId: 0, startTime: dead.startTime, pidNsInode: ourNs, refcount: 1 },
      ],
    }),
  );
  const sab = open(name, 4096);
  assert.strictEqual(sab.byteLength, 4096);
  new Int32Array(sab)[0] = 11;
  assert.strictEqual(new Int32Array(open(name, 4096))[0], 11);
});

test('live initializer: E_INIT_TIMEOUT, then takeover after it dies', { skip: POSIX ? false : SKIP }, async (t) => {
  const name = makeTrackedNameLocal(t);
  const path = shmPath(name);
  const live = await makeLiveIdentity();
  try {
    fs.writeFileSync(
      path!,
      buildHeader({
        initState: 1,
        initializerSlot: 0,
        dataBytes: 4096,
        attachSlots: [
          { pid: live.pid, threadId: 0, startTime: live.startTime, pidNsInode: 0, refcount: 1 },
        ],
      }),
    );
    assertThrowsCode(() => open(name, 4096, { initTimeoutMs: 300 }), 'E_INIT_TIMEOUT');
    // now the "initializer" dies; a fresh joiner takes over and succeeds
    live.child.kill('SIGKILL');
    await new Promise<void>((resolve) => live.child.on('exit', resolve));
    const sab = open(name, 4096);
    assert.strictEqual(sab.byteLength, 4096);
  } finally {
    unlinkQuietly(name);
    live.child.kill('SIGKILL');
  }
});

test('foreign or corrupt header -> E_INCOMPATIBLE', { skip: POSIX ? false : SKIP }, async (t) => {
  const name1 = makeTrackedNameLocal(t);
  fs.writeFileSync(shmPath(name1)!, buildHeader({ magic: 0x12345678, initState: 2, dataBytes: 4096 }));
  assertThrowsCode(() => open(name1, 4096), 'E_INCOMPATIBLE');

  const name2 = makeTrackedNameLocal(t);
  fs.writeFileSync(shmPath(name2)!, buildHeader({ layoutVersion: 99, initState: 2, dataBytes: 4096 }));
  assertThrowsCode(() => open(name2, 4096), 'E_INCOMPATIBLE');

  const name3 = makeTrackedNameLocal(t);
  fs.writeFileSync(shmPath(name3)!, buildHeader({ magic: 0xdeadbeef, layoutVersion: 7, initState: 2, dataBytes: 4096 }));
  assertThrowsCode(() => open(name3, 4096), 'E_INCOMPATIBLE');
});

test('two sequential joiners after takeover both see a ready segment', { skip: POSIX ? false : SKIP }, async (t) => {
  const name = makeTrackedNameLocal(t);
  const path = shmPath(name);
  const dead = await makeDeadIdentity();
  fs.writeFileSync(
    path!,
    buildHeader({
      initState: 1,
      initializerSlot: 0,
      dataBytes: 8192,
      attachSlots: [
        { pid: dead.pid, threadId: 0, startTime: dead.startTime, pidNsInode: 0, refcount: 1 },
      ],
    }),
  );
  const [a, b] = await Promise.all([
    Promise.resolve().then(() => open(name, 8192)),
    Promise.resolve().then(() => open(name, 8192)),
  ]);
  assert.strictEqual(a.byteLength, 8192);
  assert.strictEqual(b.byteLength, 8192);
  new Int32Array(a)[1] = 3;
  assert.strictEqual(new Int32Array(b)[1], 3);
});

// E_NO_SPACE: only where the CI prepared a small tmpfs (§13 platform rules).
test('E_NO_SPACE on a full small tmpfs', { skip: TMPFS ? false : SKIP_TMPFS }, (t) => {
  const name = makeTrackedNameLocal(t);
  assertThrowsCode(() => open(name, 512 * 1024 * 1024), 'E_NO_SPACE');
});

function makeTrackedNameLocal(t: TestContext): string {
  const name = uniqueName();
  t.after(() => unlinkQuietly(name));
  return name;
}
