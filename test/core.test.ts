// core.test.ts — ported shmbuf behaviors + new core tests (§13):
// SAB type/length/zero-init, all Atomics ops, same-process re-open, isolation
// between names, lifecycle no-ops, size validation, modes × policies, raw,
// names, worker_threads sharing one mapping, size-less join.

import { test } from 'node:test';
import { Worker } from 'node:worker_threads';
import { open, close, unlink, isNative } from '../src/core';
import { assert, makeTrackedName, assertThrowsCode, uniqueName } from './helpers';

test('native addon is loaded', () => {
  assert.strictEqual(isNative(), true);
});

test('open returns a SharedArrayBuffer of the requested size, zero-initialized', () => {
  const name = uniqueName();
  try {
    const sab = open(name, 4096);
    assert.ok(sab instanceof SharedArrayBuffer);
    assert.strictEqual(sab.byteLength, 4096);
    const i32 = new Int32Array(sab);
    for (let i = 0; i < i32.length; i++) assert.strictEqual(i32[i], 0);
  } finally {
    unlinkQuietly(name);
  }
});

test('all Atomics operations work on the SAB (ported)', () => {
  const name = uniqueName();
  try {
    const sab = open(name, 1024);
    const i32 = new Int32Array(sab);
    assert.strictEqual(Atomics.add(i32, 0, 5), 0); // returns old
    assert.strictEqual(Atomics.load(i32, 0), 5);
    assert.strictEqual(Atomics.sub(i32, 0, 2), 5); // 5 -> 3
    assert.strictEqual(Atomics.and(i32, 0, 0b110), 3); // 3 -> 2
    assert.strictEqual(Atomics.or(i32, 0, 0b001), 2); // 2 -> 3
    assert.strictEqual(Atomics.xor(i32, 0, 0b111), 3); // 3 -> 4
    assert.strictEqual(Atomics.exchange(i32, 0, 9), 4); // returns old 4
    assert.strictEqual(Atomics.compareExchange(i32, 0, 9, 1), 9); // 9 -> 1
    assert.strictEqual(Atomics.store(i32, 3, 42), 42);
    assert.strictEqual(Atomics.notify(i32, 0, 1), 0); // no waiters is fine
    assert.strictEqual(Atomics.wait(i32, 4, 0, 10), 'timed-out');
    const f64 = new Float64Array(sab); // second view over the same segment
    f64[0] = 1.5;
    assert.strictEqual(f64[0], 1.5);
  } finally {
    unlinkQuietly(name);
  }
});

test('same-process re-open sees the same memory (ported)', () => {
  const name = uniqueName();
  try {
    const a = open(name, 4096);
    new Int32Array(a)[7] = 1234;
    const b = open(name, 4096);
    assert.strictEqual(new Int32Array(b)[7], 1234);
    new Int32Array(b)[8] = 77;
    assert.strictEqual(new Int32Array(a)[8], 77);
  } finally {
    unlinkQuietly(name);
  }
});

test('names are isolated (ported)', () => {
  const a = uniqueName();
  const b = uniqueName();
  try {
    new Int32Array(open(a, 256))[0] = 1;
    new Int32Array(open(b, 256))[0] = 2;
    assert.strictEqual(new Int32Array(open(a, 256))[0], 1);
    assert.strictEqual(new Int32Array(open(b, 256))[0], 2);
  } finally {
    unlinkQuietly(a);
    unlinkQuietly(b);
  }
});

test('close() is a documented no-op: SAB still usable, invalid name throws', () => {
  const name = uniqueName();
  try {
    const sab = open(name, 256);
    close(name);
    new Int32Array(sab)[0] = 5; // memory stays valid
    assert.strictEqual(new Int32Array(open(name, 256))[0], 5);
    assertThrowsCode(() => close('invalid'), 'E_NAME_INVALID');
  } finally {
    unlinkQuietly(name);
  }
});

test('unlink removes the name; re-open creates a fresh segment', () => {
  const name = uniqueName();
  try {
    new Int32Array(open(name, 256))[0] = 9;
    unlink(name);
    const fresh = open(name, 256);
    assert.strictEqual(new Int32Array(fresh)[0], 0, 'new segment, not the unlinked memory');
  } finally {
    unlinkQuietly(name);
  }
});

test('unlink of a missing segment throws E_NOT_FOUND', () => {
  assertThrowsCode(() => unlink('/membridge-test-definitely-missing-zz'), 'E_NOT_FOUND');
});

test('size validation: 2^32+16, 1.9, -1, 0, NaN, > cap -> E_SIZE_INVALID', () => {
  const name = uniqueName();
  try {
    assertThrowsCode(() => open(name, 2 ** 32 + 16), 'E_SIZE_INVALID');
    assertThrowsCode(() => open(name, 1.9), 'E_SIZE_INVALID');
    assertThrowsCode(() => open(name, -1), 'E_SIZE_INVALID');
    assertThrowsCode(() => open(name, 0), 'E_SIZE_INVALID');
    assertThrowsCode(() => open(name, Number.NaN), 'E_SIZE_INVALID');
    assertThrowsCode(() => open(name, 256 * 1024 * 1024 + 1), 'E_SIZE_INVALID', (e) => {
      assert.ok(e.message.includes('MEMBRIDGE_MAX_SEGMENT_BYTES'));
    });
    // the env override lifts the cap for big-but-legal sizes
    process.env.MEMBRIDGE_MAX_SEGMENT_BYTES = String(300 * 1024 * 1024);
    const big = uniqueName();
    try {
      const s = open(big, 256 * 1024 * 1024 + 1);
      assert.strictEqual(s.byteLength, 256 * 1024 * 1024 + 1);
    } finally {
      unlinkQuietly(big);
      delete process.env.MEMBRIDGE_MAX_SEGMENT_BYTES;
    }
  } finally {
    unlinkQuietly(name);
  }
});

test('name validation -> E_NAME_INVALID', () => {
  assertThrowsCode(() => open('no-slash', 16), 'E_NAME_INVALID');
  assertThrowsCode(() => open('/double//slash', 16), 'E_NAME_INVALID');
  assertThrowsCode(() => open('/', 16), 'E_NAME_INVALID');
  assertThrowsCode(() => open(`/${'x'.repeat(300)}`, 16), 'E_NAME_INVALID');
});

test("mode 'create' throws E_EXISTS; 'join' missing throws E_NOT_FOUND", () => {
  const name = uniqueName();
  try {
    open(name, 256, { mode: 'create' });
    assertThrowsCode(() => open(name, 256, { mode: 'create' }), 'E_EXISTS', (e) => {
      assert.strictEqual(e.segmentName, name);
    });
    open(name, 256, { mode: 'join' }); // existing is fine
    const missing = uniqueName();
    assertThrowsCode(() => open(missing, 256, { mode: 'join' }), 'E_NOT_FOUND');
  } finally {
    unlinkQuietly(name);
  }
});

test("size policy 'exact' mismatch -> E_SIZE_MISMATCH with structured fields", () => {
  const name = uniqueName();
  try {
    open(name, 4096);
    assertThrowsCode(() => open(name, 8192), 'E_SIZE_MISMATCH', (e) => {
      assert.strictEqual(e.segmentName, name);
      assert.strictEqual(e.requested, 8192);
      assert.strictEqual(e.existing, 4096);
    });
  } finally {
    unlinkQuietly(name);
  }
});

test("size policy 'at-least': smaller request maps a prefix; larger throws", () => {
  const name = uniqueName();
  try {
    const full = open(name, 8192);
    new Int32Array(full)[2047] = 55;
    const prefix = open(name, 4096, { sizePolicy: 'at-least' });
    assert.strictEqual(prefix.byteLength, 4096);
    assert.strictEqual(new Int32Array(prefix)[0], 0); // same memory, prefix view
    assertThrowsCode(() => open(name, 16384, { sizePolicy: 'at-least' }), 'E_SIZE_MISMATCH');
    // full view over the same segment still works
    const full2 = open(name, 8192);
    assert.strictEqual(new Int32Array(full2)[2047], 55);
  } finally {
    unlinkQuietly(name);
  }
});

test("size policy 'grow': extends the segment; old SABs stay valid; attached keep smaller views", () => {
  const name = uniqueName();
  try {
    const small = open(name, 4096);
    new Int32Array(small)[0] = 7;
    const grown = open(name, 8192, { sizePolicy: 'grow' });
    assert.strictEqual(grown.byteLength, 8192);
    assert.strictEqual(new Int32Array(grown)[0], 7, 'data preserved across grow');
    new Int32Array(grown)[2047] = 8;
    assert.strictEqual(new Int32Array(small).length, 1024, 'old SAB keeps its smaller length');
    assert.strictEqual(new Int32Array(small)[0], 7);
    // grow to no smaller than existing is an at-least reuse
    const again = open(name, 4096, { sizePolicy: 'grow' });
    assert.strictEqual(again.byteLength, 4096);
  } finally {
    unlinkQuietly(name);
  }
});

test('open(name, opts) joins at the existing size', () => {
  const name = uniqueName();
  try {
    open(name, 4096);
    const sab = open(name, { mode: 'join' });
    assert.strictEqual(sab.byteLength, 4096);
    // size-less create is invalid
    assertThrowsCode(() => open(uniqueName(), { mode: 'create' }), 'E_SIZE_INVALID');
    // size-less create-or-join on a missing name cannot conjure a size
    assertThrowsCode(() => open(uniqueName(), {}), 'E_NOT_FOUND');
  } finally {
    unlinkQuietly(name);
  }
});

test('raw: true — no header, exact size, foreign-compatible', () => {
  const name = uniqueName();
  try {
    const sab = open(name, 4096, { raw: true });
    assert.strictEqual(sab.byteLength, 4096);
    new Int32Array(sab)[0] = 3;
    assert.strictEqual(new Int32Array(open(name, 4096, { raw: true, mode: 'join' }))[0], 3);
    assertThrowsCode(() => open(name, 8192, { raw: true, mode: 'join' }), 'E_SIZE_MISMATCH');
  } finally {
    unlinkQuietly(name);
  }
});

test('worker_threads share one mapping (open from another isolate)', async (t) => {
  const name = makeTrackedName(t);
  const main = open(name, 4096);
  const code = `
    const { parentPort, workerData } = require('worker_threads');
    const { open } = require(workerData.pkg);
    const sab = open(workerData.name, 4096);
    const i32 = new Int32Array(sab);
    Atomics.add(i32, 0, 41);
    parentPort.postMessage({ seen: i32[1] });
  `;
  new Int32Array(main)[1] = 99;
  const seen: { seen: number } = await new Promise((resolve, reject) => {
    const w = new Worker(code, {
      eval: true,
      workerData: { name, pkg: require.resolve('../src/core') },
    });
    w.on('message', resolve);
    w.on('error', reject);
  });
  assert.strictEqual(seen.seen, 99, 'worker sees main-thread writes');
  assert.strictEqual(new Int32Array(main)[0], 41, 'main sees worker writes');
});

function unlinkQuietly(name: string): void {
  try {
    unlink(name);
  } catch {
    /* E_NOT_FOUND is fine in cleanup */
  }
}
