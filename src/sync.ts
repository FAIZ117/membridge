// sync.ts — §6 cross-process wait/notify on i32 words of a membridge segment
// (public: users building their own lock-free structures on a membridge SAB
// hit F1 — Atomics.notify cannot wake another process — just the same).
//
// Waitable words are i32 only (futex/os_sync/semaphore constraint, §6).
// Sync waits block the calling JS thread like Atomics.wait; async waits run
// on membridge-owned waiter threads, never the libuv pool.

import { MembridgeError } from './errors';
import { nativeOrThrow } from './native';

export type WaitResult = 'ok' | 'not-equal' | 'timed-out';

// Loop pinning: a pending native wait does not keep the event loop alive by
// itself (uv_ref proved unreliable across Node versions), so sync.ts holds a
// ref'd timer while any waitAsync is outstanding — same semantics as a
// pending setTimeout.
let outstandingWaits = 0;
let keepAlive: NodeJS.Timeout | null = null;

function pinLoopWhile(p: Promise<WaitResult>): Promise<WaitResult> {
  outstandingWaits++;
  if (keepAlive === null) {
    keepAlive = setInterval(() => {}, 0x7fffffff);
  }
  // Two-armed .then (not .finally): a .finally-derived promise would carry
  // rejections with no handler and fail the surrounding test.
  const release = () => {
    if (--outstandingWaits <= 0) {
      outstandingWaits = 0;
      if (keepAlive !== null) {
        clearInterval(keepAlive);
        keepAlive = null;
      }
    }
  };
  void p.then(release, release);
  return p;
}

function wordAddrCheck(view: Int32Array, index: number): void {
  if (!(view instanceof Int32Array)) {
    throw new MembridgeError('E_NAME_INVALID', 'wait/notify need an Int32Array (waitable words are i32 only)');
  }
  if (!Number.isSafeInteger(index) || index < 0 || index >= view.length) {
    throw new MembridgeError('E_NAME_INVALID', `word index ${index} out of range`);
  }
}

// Timeouts are clamped to 2^31 ms (~24.8 days): beyond that the value cannot
// be represented as a timespec deadline without int64 overflow, and no
// plausible caller means a literal >24-day wait (review F9 — 1e300 used to
// spin the multiplexer at 100% CPU on an EINVAL loop). NaN stays invalid.
const MAX_TIMEOUT_MS = 2 ** 31;

function timeoutCheck(timeoutMs: number | undefined): void {
  if (timeoutMs !== undefined) {
    if (Number.isNaN(timeoutMs) || timeoutMs <= 0) {
      throw new MembridgeError('E_NAME_INVALID', 'timeoutMs must be a positive number');
    }
    if (timeoutMs > MAX_TIMEOUT_MS) {
      throw new MembridgeError(
        'E_NAME_INVALID',
        `timeoutMs must be <= ${MAX_TIMEOUT_MS} (~24.8 days); use no timeout for longer waits`,
      );
    }
  }
}

/**
 * Block until another process/thread calls {@link notify} on the same word
 * (or any futex wake on it), the word no longer equals `expected`, or the
 * timeout expires. Blocks the calling JS thread like `Atomics.wait`.
 */
export function wait(
  view: Int32Array,
  index: number,
  expected: number,
  timeoutMs?: number,
): WaitResult {
  wordAddrCheck(view, index);
  timeoutCheck(timeoutMs);
  if (!Number.isSafeInteger(expected)) {
    throw new MembridgeError('E_NAME_INVALID', 'expected must be an i32 value');
  }
  const b = nativeOrThrow();
  return b.syncWait(view, index, expected, timeoutMs === undefined ? -1 : timeoutMs) as WaitResult;
}

/** Wake up to `count` waiters on the word (default: all). */
export function notify(view: Int32Array, index: number, count?: number): void {
  wordAddrCheck(view, index);
  const b = nativeOrThrow();
  b.syncNotify(view, index, count === undefined ? -1 : count);
}

/**
 * Like {@link wait} but returns a Promise; resolves `'not-equal'` when the
 * word differs from `expected` before the wait parks (same contract as the
 * synchronous {@link wait}). The wait runs on a membridge-owned
 * waiter thread (never the libuv pool threadpool). Isolate teardown (worker
 * exit or `.terminate()`) cancels the wait and resolves `'timed-out'`.
 */
export function waitAsync(
  view: Int32Array,
  index: number,
  expected: number,
  timeoutMs?: number,
): Promise<WaitResult> {
  wordAddrCheck(view, index);
  timeoutCheck(timeoutMs);
  if (!Number.isSafeInteger(expected)) {
    throw new MembridgeError('E_NAME_INVALID', 'expected must be an i32 value');
  }
  const b = nativeOrThrow();
  return pinLoopWhile(
    b.syncWaitAsync(view, index, expected, timeoutMs === undefined ? -1 : timeoutMs) as Promise<WaitResult>,
  );
}
