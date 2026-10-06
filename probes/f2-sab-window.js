'use strict';
// F2 + F15 (PLAN §2): the core technique — an mmap'd shm object wrapped as a
// SharedArrayBuffer via V8's BackingStore API (F2: N-API has no equivalent),
// with the §3 window shape (F15: a SAB's length always equals its
// BackingStore's, so the data-region window is made at the BackingStore level
// over `base + headerBytes`).
//
// Checks: SAB type, byteLength == dataBytes, zero-init, Atomics ops, an offset
// window landing in the same memory, registry reuse, and the deleter freeing
// the Mapping once the last SAB is GC'd (run with --expose-gc for that part).

const assert = require('assert');
const { addon, probeName, unlinkQuietly } = require('./lib');

const a = addon();
const name = probeName('sab');
let failed = false;

try {
  const sab = a.open(name, 4096);
  assert.ok(sab instanceof SharedArrayBuffer, 'instanceof SharedArrayBuffer');
  assert.strictEqual(sab.byteLength, 4096, 'byteLength == dataBytes (F15: window at the BackingStore level)');

  const i32 = new Int32Array(sab);
  for (let i = 0; i < i32.length; i++) assert.strictEqual(i32[i], 0, 'zero-init');
  assert.strictEqual(Atomics.add(i32, 0, 42), 0, 'Atomics.add');
  assert.strictEqual(Atomics.load(i32, 0), 42, 'Atomics.load');
  assert.strictEqual(Atomics.compareExchange(i32, 0, 42, 7), 42, 'Atomics.compareExchange');

  // Second BackingStore over the SAME mapping at a nonzero data offset.
  const w = a.window(name, 512, 256);
  assert.strictEqual(w.byteLength, 256, 'window byteLength');
  const wi = new Int32Array(w);
  wi[0] = 99;
  assert.strictEqual(i32[512 / 4], 99, 'offset window writes land in the same memory');

  // Re-open: registry reuse, fresh BackingStore, same memory.
  const sab2 = a.open(name, 4096);
  assert.strictEqual(new Int32Array(sab2)[0], 7, 're-open sees the same data');

  if (typeof global.gc === 'function') {
    const gcName = probeName('sab-gc');
    try {
      let s = a.open(gcName, 1024);
      new Int32Array(s)[0] = 1;
      s = null;
      for (let i = 0; i < 6; i++) global.gc();
      assert.strictEqual(a.alive(gcName), false, 'Mapping freed after last SAB GC (deleter ran)');
      console.log('PASS sab-gc-lifecycle: mapping freed after last SAB collected');
    } finally {
      unlinkQuietly(a, [gcName]);
    }
  } else {
    console.log('SKIP sab-gc-lifecycle: run with --expose-gc');
  }

  console.log('PASS f2-sab-window');
} catch (e) {
  failed = true;
  console.error('FAIL f2-sab-window:', e.stack);
} finally {
  unlinkQuietly(a, [name]);
}
process.exitCode = failed ? 1 : 0;
