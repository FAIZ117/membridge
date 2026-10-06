// fallback.test.ts — §5.6: the fallback is opt-in only, works in-process with
// size checks, and Mutex/RingBuffer-grade consumers must not mistake it for
// shared memory. Tested through the fallback module directly (the native
// addon is present in this environment, so core.open() prefers it).

import { test } from 'node:test';
import { fallbackOpen, fallbackUnlink, fallbackEnabled } from '../src/fallback';
import type { OpenOptions } from '../src/core';
import { assert, uniqueName, unlinkQuietly, assertThrowsCode } from './helpers';

test('fallback is opt-in (env or per-call), never on by default', () => {
  delete process.env.MEMBRIDGE_ALLOW_FALLBACK;
  assert.strictEqual(fallbackEnabled(), false);
  assert.strictEqual(fallbackEnabled({ allowFallback: true }), true);
  process.env.MEMBRIDGE_ALLOW_FALLBACK = '1';
  try {
    assert.strictEqual(fallbackEnabled(), true);
  } finally {
    delete process.env.MEMBRIDGE_ALLOW_FALLBACK;
  }
});

test('fallback create/join with size checks', () => {
  const name = uniqueName();
  try {
    const sab = fallbackOpen(name, 4096, { mode: 'create-or-join' });
    assert.ok(sab instanceof SharedArrayBuffer);
    assert.strictEqual(sab.byteLength, 4096);
    new Int32Array(sab)[0] = 5;
    assert.strictEqual(new Int32Array(fallbackOpen(name, 4096, {}))[0], 5);
    assertThrowsCode(() => fallbackOpen(name, 8192, {}), 'E_SIZE_MISMATCH');
    assertThrowsCode(() => fallbackOpen(name, 4096, { mode: 'create' }), 'E_EXISTS');
    assertThrowsCode(() => fallbackOpen(name, 8192, { sizePolicy: 'at-least' }), 'E_SIZE_MISMATCH');
    assertThrowsCode(() => fallbackOpen(name, 4096, { sizePolicy: 'grow' }), 'E_GROW_UNSUPPORTED');
    assertThrowsCode(() => fallbackOpen(uniqueName(), undefined, {}), 'E_NOT_FOUND');
  } finally {
    try {
      fallbackUnlink(name);
    } catch {
      /* ok */
    }
  }
});

test('fallback join at existing size', () => {
  const name = uniqueName();
  try {
    fallbackOpen(name, 2048, { mode: 'create-or-join' });
    const joined = fallbackOpen(name, undefined, { mode: 'join' });
    assert.strictEqual(joined.byteLength, 2048);
  } finally {
    try {
      fallbackUnlink(name);
    } catch {
      /* ok */
    }
  }
});

test('fallback unlink: missing -> E_NOT_FOUND', () => {
  assertThrowsCode(() => fallbackUnlink(uniqueName()), 'E_NOT_FOUND');
});

// Document the shape expected by §5.6 for Mutex/RingBuffer (M4/M5): they call
// E_NATIVE_UNAVAILABLE unless the fallback was explicitly opted in.
test('fallback options type accepts allowFallback (compile-level contract)', () => {
  const opts: OpenOptions = { allowFallback: true };
  assert.strictEqual(opts.allowFallback, true);
});
