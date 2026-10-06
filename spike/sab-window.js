'use strict';
// Spike: SAB window over shm — the §3 shape.
// Checks: SAB type, byteLength == dataBytes, zero-init, Atomics ops work, an
// offset window lands in the same memory, and the GC deleter frees the Mapping
// (registry goes dead) once the last SAB is collected. Run with --expose-gc.

const assert = require('assert');
const { addon, probeName, cleanupAll } = require('./lib');

const a = addon();
const name = probeName('sab');
let failed = false;

try {
  const sab = a.open(name, 4096);
  assert.ok(sab instanceof SharedArrayBuffer, 'instanceof SharedArrayBuffer');
  assert.strictEqual(sab.byteLength, 4096, 'byteLength == dataBytes (SAB is a window, F15)');

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
  assert.strictEqual(i32[512 / 4], 99, 'window writes land in the same memory');

  // Re-open: registry reuse, fresh BackingStore, same memory.
  const sab2 = a.open(name, 4096);
  assert.strictEqual(new Int32Array(sab2)[0], 7, 're-open sees the same data');

  if (typeof global.gc === 'function') {
    const gcName = probeName('sab-gc');
    try {
      let s = a.open(gcName, 1024);
      new Int32Array(s)[0] = 1;
      s = null;
      // SAB instances above still keep the other mapping alive; this one has
      // no references left — GC must run the deleter and free the Mapping.
      for (let i = 0; i < 6; i++) global.gc();
      assert.strictEqual(a.alive(gcName), false, 'Mapping freed after last SAB GC (deleter ran)');
      console.log('PASS sab-gc-lifecycle: mapping freed after last SAB collected');
    } finally {
      cleanupAll(a, [gcName]);
    }
  } else {
    console.log('SKIP sab-gc-lifecycle: run with --expose-gc');
  }

  console.log('PASS sab-window');
} catch (e) {
  failed = true;
  console.error('FAIL sab-window:', e.stack);
} finally {
  cleanupAll(a, [name]);
}
process.exitCode = failed ? 1 : 0;
