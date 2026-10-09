// multi.test.ts — multi-process correctness (§13): the ported 8-process
// join race, the ported 8-process lost-update check as a pure correctness
// test (no throughput threshold), and a cross-process fork handshake.
//
// Children re-enter this same file via fork() with SHM_BRIDGE_TEST_ROLE set;
// the guard below runs the role and exits before any test registration.

import { test } from 'node:test';
import { fork } from 'node:child_process';
import { open, unlink } from '../src/core';
import { assert, uniqueName, holdForTest, unlinkQuietly } from './helpers';

const ROLE = process.env.SHM_BRIDGE_TEST_ROLE;
if (ROLE !== undefined) {
  childMain(ROLE, process.env.SHM_BRIDGE_TEST_ARG ?? '');
  process.exit(1); // childMain exits on its own; this is a safety net
}

function childMain(role: string, arg: string): void {
  const [name, size, payload] = arg.split('|');
  if (role === 'join-write') {
    // create-or-join the segment and set our own byte; exercises the create/
    // join + init protocol across 8 concurrent openers.
    const sab = open(name!, Number(size!));
    new Uint8Array(sab)[Number(payload)] = 1;
    process.exit(0);
  }
  if (role === 'join-inc') {
    const sab = open(name!, Number(size!));
    const i32 = new Int32Array(sab);
    for (let i = 0; i < Number(payload); i++) Atomics.add(i32, 0, 1);
    process.exit(0);
  }
  if (role === 'handshake-write') {
    const sab = open(name!, Number(size!), { mode: 'join' });
    const i32 = new Int32Array(sab);
    i32[0] = Number(payload);
    Atomics.store(i32, 1, 1); // flag
    process.exit(0);
  }
  process.exit(2);
}

function forkRole(role: string, arg: string): Promise<{ code: number | null; pid: number }> {
  return new Promise((resolve, reject) => {
    const child = fork(__filename, {
      env: { ...process.env, SHM_BRIDGE_TEST_ROLE: role, SHM_BRIDGE_TEST_ARG: arg },
      stdio: 'inherit',
    });
    child.on('exit', (code) => resolve({ code, pid: child.pid ?? -1 }));
    child.on('error', reject);
  });
}

test('8-process join race on one name (ported + new)', async (t) => {
  const name = uniqueName();
  const size = 4096;
  try {
    holdForTest(t, open(name, size)); // parent creates first so children all join
    const runs = Array.from({ length: 8 }, (_, i) => forkRole('join-write', `${name}|${size}|${i}`));
    const results = await Promise.all(runs);
    for (const r of results) assert.strictEqual(r.code, 0, `child exited ${r.code}`);
    const bytes = new Uint8Array(open(name, size));
    for (let i = 0; i < 8; i++) {
      assert.strictEqual(bytes[i], 1, `child ${i} byte missing`);
    }
  } finally {
    unlinkQuietly(name);
  }
});

test('8-process lost-update check: Atomics.add is correct cross-process (ported)', async () => {
  const name = uniqueName();
  const size = 256;
  const increments = 2000;
  try {
    const sab = open(name, size);
    new Int32Array(sab)[0] = 0;
    const runs = Array.from({ length: 8 }, () => forkRole('join-inc', `${name}|${size}|${increments}`));
    const results = await Promise.all(runs);
    for (const r of results) assert.strictEqual(r.code, 0, `child exited ${r.code}`);
    assert.strictEqual(Atomics.load(new Int32Array(sab), 0), 8 * increments);
  } finally {
    unlinkQuietly(name);
  }
});

test('fork handshake: child joins, writes, parent observes (ported)', async (t) => {
  const name = uniqueName();
  try {
    holdForTest(t, open(name, 256, { mode: 'create' }));
    const { code } = await forkRole('handshake-write', `${name}|256|424242`);
    assert.strictEqual(code, 0);
    const i32 = new Int32Array(open(name, 256));
    assert.strictEqual(i32[0], 424242);
    assert.strictEqual(i32[1], 1);
  } finally {
    unlinkQuietly(name);
  }
});
