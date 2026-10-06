"use strict";
// sync.ts — §6 cross-process wait/notify on i32 words of a membridge segment
// (public: users building their own lock-free structures on a membridge SAB
// hit F1 — Atomics.notify cannot wake another process — just the same).
//
// Waitable words are i32 only (futex/os_sync/semaphore constraint, §6).
// Sync waits block the calling JS thread like Atomics.wait; async waits run
// on membridge-owned waiter threads, never the libuv pool.
Object.defineProperty(exports, "__esModule", { value: true });
exports.wait = wait;
exports.notify = notify;
exports.waitAsync = waitAsync;
const errors_1 = require("./errors");
const native_1 = require("./native");
// Loop pinning: a pending native wait does not keep the event loop alive by
// itself (uv_ref proved unreliable across Node versions), so sync.ts holds a
// ref'd timer while any waitAsync is outstanding — same semantics as a
// pending setTimeout.
let outstandingWaits = 0;
let keepAlive = null;
function pinLoopWhile(p) {
    outstandingWaits++;
    if (keepAlive === null) {
        keepAlive = setInterval(() => { }, 0x7fffffff);
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
function wordAddrCheck(view, index) {
    if (!(view instanceof Int32Array)) {
        throw new errors_1.MembridgeError('E_NAME_INVALID', 'wait/notify need an Int32Array (waitable words are i32 only)');
    }
    if (!Number.isSafeInteger(index) || index < 0 || index >= view.length) {
        throw new errors_1.MembridgeError('E_NAME_INVALID', `word index ${index} out of range`);
    }
}
function timeoutCheck(timeoutMs) {
    if (timeoutMs !== undefined && (!Number.isFinite(timeoutMs) || timeoutMs <= 0)) {
        throw new errors_1.MembridgeError('E_NAME_INVALID', 'timeoutMs must be a positive number');
    }
}
/**
 * Block until another process/thread calls {@link notify} on the same word
 * (or any futex wake on it), the word no longer equals `expected`, or the
 * timeout expires. Blocks the calling JS thread like `Atomics.wait`.
 */
function wait(view, index, expected, timeoutMs) {
    wordAddrCheck(view, index);
    timeoutCheck(timeoutMs);
    if (!Number.isSafeInteger(expected)) {
        throw new errors_1.MembridgeError('E_NAME_INVALID', 'expected must be an i32 value');
    }
    const b = (0, native_1.nativeOrThrow)();
    return b.syncWait(view, index, expected, timeoutMs === undefined ? -1 : timeoutMs);
}
/** Wake up to `count` waiters on the word (default: all). */
function notify(view, index, count) {
    wordAddrCheck(view, index);
    const b = (0, native_1.nativeOrThrow)();
    b.syncNotify(view, index, count === undefined ? -1 : count);
}
/**
 * Like {@link wait} but returns a Promise; the wait runs on a membridge-owned
 * waiter thread (never the libuv pool threadpool). Isolate teardown (worker
 * exit or `.terminate()`) cancels the wait and resolves `'timed-out'`.
 */
function waitAsync(view, index, expected, timeoutMs) {
    wordAddrCheck(view, index);
    timeoutCheck(timeoutMs);
    if (!Number.isSafeInteger(expected)) {
        throw new errors_1.MembridgeError('E_NAME_INVALID', 'expected must be an i32 value');
    }
    const b = (0, native_1.nativeOrThrow)();
    return pinLoopWhile(b.syncWaitAsync(view, index, expected, timeoutMs === undefined ? -1 : timeoutMs));
}
//# sourceMappingURL=sync.js.map