// hardening.test.ts — regression tests from the pre-release review round
// (docs/review/2026-10-06T1909-pre-release-review.md). Each test names the
// finding it pins. POSIX-only byte-crafting tests skip elsewhere.

import { test } from 'node:test';
import fs from 'node:fs';
import { fork } from 'node:child_process';
import { open, unlink } from '../src/core';
import { RingConsumer } from '../src/ringbuffer';
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
