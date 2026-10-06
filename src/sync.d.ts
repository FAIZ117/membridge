export type WaitResult = 'ok' | 'not-equal' | 'timed-out';
/**
 * Block until another process/thread calls {@link notify} on the same word
 * (or any futex wake on it), the word no longer equals `expected`, or the
 * timeout expires. Blocks the calling JS thread like `Atomics.wait`.
 */
export declare function wait(view: Int32Array, index: number, expected: number, timeoutMs?: number): WaitResult;
/** Wake up to `count` waiters on the word (default: all). */
export declare function notify(view: Int32Array, index: number, count?: number): void;
/**
 * Like {@link wait} but returns a Promise; the wait runs on a membridge-owned
 * waiter thread (never the libuv pool threadpool). Isolate teardown (worker
 * exit or `.terminate()`) cancels the wait and resolves `'timed-out'`.
 */
export declare function waitAsync(view: Int32Array, index: number, expected: number, timeoutMs?: number): Promise<WaitResult>;
