export declare const MUTEX_DATA_BYTES: number;
export interface LockOptions {
    /** Total time to wait before E_TIMEOUT. Default: wait indefinitely. */
    timeoutMs?: number;
}
export interface LockResult {
    /** A previous holder died while holding the lock; the protected data may
     * be half-updated and only the caller can repair it (§7.3, like pthread
     * EOWNERDEAD). */
    ownerDied: boolean;
}
export interface MutexOptions {
    /** Windows: use the Global\ prefix. */
    winGlobal?: boolean;
}
export declare class Mutex {
    private readonly name;
    private readonly view;
    private readonly b;
    private claimed;
    private held;
    private constructor();
    /** Open (create or join) a mutex segment. Zero-initialized by open(). */
    static open(name: string, opts?: MutexOptions): Mutex;
    /** Remove the mutex segment (POSIX shm_unlink). */
    static unlink(name: string): void;
    private claim;
    private lockWord;
    private isMyToken;
    /** Acquire the lock, blocking. */
    lock(opts?: LockOptions): LockResult;
    /** Acquire without blocking. Returns false when the lock is held (even by a
     * dead owner — tryLock does not steal, §7.3). */
    tryLock(): boolean;
    /** Acquire asynchronously; waits ride the membridge waiter threads (§6),
     * never the libuv pool. Aborts with the signal between wait slices. */
    lockAsync(opts?: LockOptions & {
        signal?: AbortSignal;
    }): Promise<LockResult>;
    /** Release. Throws E_NOT_OWNER when this instance does not hold the lock. */
    unlock(): void;
    /** Run fn while holding the lock; fn receives { ownerDied } (§7.3). */
    withLock<T>(fn: (lock: LockResult) => T, opts?: LockOptions): T;
    /** Async variant of {@link withLock}. */
    withLockAsync<T>(fn: (lock: LockResult) => T, opts?: LockOptions & {
        signal?: AbortSignal;
    }): Promise<T>;
    /** Diagnostics: current seq counter (acquisitions so far). */
    get sequence(): number;
    identityWords(): {
        pid: number;
        tid: number;
        start: number;
        ns: number;
    };
}
export declare const SLOT_LAYOUT: {
    SLOTS: number;
    SLOT_WORDS: number;
    SLOT_COUNT: number;
    PID: number;
    TID: number;
    START_LO: number;
    START_HI: number;
    NS_LO: number;
    NS_HI: number;
    GEN: number;
    STATE: number;
    STATE_ACTIVE: number;
    STATE_FREE: number;
};
