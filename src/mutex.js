"use strict";
// mutex.ts — §7 crash-safe Mutex.
//
// Every state transition is a CAS on the single token word (§7.2): lock,
// unlock, and steal are all one compare-exchange, so a crashed holder is
// recoverable without heartbeats. The JS layer runs those CAS loops with
// Atomics over the kind=mutex data region; the native layer provides slot
// claims with liveness-based reclamation (cc/mutex.cc), owner-liveness for
// the steal path, and per-isolate teardown tracking (§7.1).
//
// No priority inheritance, no fairness guarantee (§7.3): a woken waiter
// competes with newcomers for the CAS.
Object.defineProperty(exports, "__esModule", { value: true });
exports.SLOT_LAYOUT = exports.Mutex = exports.MUTEX_DATA_BYTES = void 0;
const errors_1 = require("./errors");
const native_1 = require("./native");
const core_1 = require("./core");
const sync_1 = require("./sync"); // pinned loop while async waits pend
// Data-region layout (mirrors cc/mutex.h)
const HEADER_WORDS = 4; // lockWord, ownerDied, seq, pad
const SLOT_WORDS = 8; // pid, tid, startLo, startHi, nsLo, nsHi, gen, state
const SLOT_COUNT = 64;
exports.MUTEX_DATA_BYTES = (HEADER_WORDS + SLOT_COUNT * SLOT_WORDS) * 4;
const HAS_WAITERS = 0x80000000;
const TOKEN_MASK = 0x7fffffff;
const GEN_MASK = 0x7fff;
const LOCK_WORD = 0;
const OWNER_DIED = 1;
const SEQ = 2;
const SLOTS = HEADER_WORDS;
const sleepSliceMs = 250;
function selfIdentityWords() {
    const b = (0, native_1.nativeOrThrow)();
    const id = b.selfIdentity();
    return { pid: id.pid, tid: id.threadId, start: id.startTime, ns: id.pidNsInode };
}
class Mutex {
    name;
    view;
    b;
    claimed = null;
    held = false;
    constructor(name, view) {
        this.name = name;
        this.view = view;
        this.b = (0, native_1.nativeOrThrow)();
    }
    /** Open (create or join) a mutex segment. Zero-initialized by open(). */
    static open(name, opts) {
        // E_EXISTS when a foreign/foreign-sized segment of that name exists is
        // surfaced from open(); mutex segments have a fixed data size.
        const sab = (0, core_1.open)(name, exports.MUTEX_DATA_BYTES, { kind: 'mutex', winGlobal: opts?.winGlobal });
        return new Mutex(name, new Int32Array(sab));
    }
    /** Remove the mutex segment (POSIX shm_unlink). */
    static unlink(name) {
        (0, core_1.unlink)(name);
    }
    claim() {
        if (this.claimed !== null)
            return this.claimed;
        const r = this.b.mutexClaimSlot(this.name, this.view);
        this.claimed = { slot: r.slot, gen: r.gen, token: r.token };
        return this.claimed;
    }
    lockWord() {
        return Atomics.load(this.view, LOCK_WORD);
    }
    isMyToken(tokenBits) {
        return tokenBits === (this.claimed?.token ?? -1);
    }
    /** Acquire the lock, blocking. */
    lock(opts) {
        const deadline = opts?.timeoutMs !== undefined ? Date.now() + opts.timeoutMs : Infinity;
        const token = this.claim().token;
        for (;;) {
            const lw = this.lockWord();
            const tokenBits = lw & TOKEN_MASK;
            if (tokenBits === 0) {
                // CAS the whole word (a stale HAS_WAITERS bit from a lost race is
                // cleared by the acquisition itself)
                if (Atomics.compareExchange(this.view, LOCK_WORD, lw, token) === lw) {
                    Atomics.add(this.view, SEQ, 1);
                    this.held = true;
                    this.b.mutexTrackHeld(this.name, this.view, token);
                    return { ownerDied: Atomics.exchange(this.view, OWNER_DIED, 0) === 1 };
                }
                continue;
            }
            if (tokenBits === token) {
                throw new errors_1.MembridgeError('E_DEADLOCK', 'mutex already locked by this thread', {
                    segmentName: this.name,
                });
            }
            // Dead owner? Steal: single CAS deadToken -> myToken (§7.3).
            if (!this.b.mutexOwnerAlive(this.view, tokenBits)) {
                const stolen = Atomics.compareExchange(this.view, LOCK_WORD, lw, token | (lw & HAS_WAITERS));
                if (stolen === lw) {
                    Atomics.store(this.view, OWNER_DIED, 1);
                    Atomics.add(this.view, SEQ, 1);
                    this.held = true;
                    this.b.mutexTrackHeld(this.name, this.view, token);
                    return { ownerDied: true };
                }
                continue; // lost the race: retry
            }
            // Alive: set HAS_WAITERS and wait a bounded slice (deadlock detection
            // cadence, §7.3 — a dead owner never calls notify).
            if (!(lw & HAS_WAITERS)) {
                Atomics.or(this.view, LOCK_WORD, HAS_WAITERS);
            }
            const expected = this.lockWord();
            if (opts?.timeoutMs !== undefined && Date.now() >= deadline) {
                throw new errors_1.MembridgeError('E_TIMEOUT', 'mutex lock timed out', { segmentName: this.name });
            }
            this.b.syncWait(this.view, LOCK_WORD, expected, sleepSliceMs);
            if (opts?.timeoutMs !== undefined && Date.now() >= deadline) {
                throw new errors_1.MembridgeError('E_TIMEOUT', 'mutex lock timed out', { segmentName: this.name });
            }
        }
    }
    /** Acquire without blocking. Returns false when the lock is held (even by a
     * dead owner — tryLock does not steal, §7.3). */
    tryLock() {
        const token = this.claim().token;
        const lw = this.lockWord();
        if ((lw & TOKEN_MASK) === token) {
            throw new errors_1.MembridgeError('E_DEADLOCK', 'mutex already locked by this thread', {
                segmentName: this.name,
            });
        }
        if ((lw & TOKEN_MASK) !== 0)
            return false;
        if (Atomics.compareExchange(this.view, LOCK_WORD, lw, token) === lw) {
            Atomics.add(this.view, SEQ, 1);
            this.held = true;
            this.b.mutexTrackHeld(this.name, this.view, token);
            return true;
        }
        return false;
    }
    /** Acquire asynchronously; waits ride the membridge waiter threads (§6),
     * never the libuv pool. Aborts with the signal between wait slices. */
    async lockAsync(opts) {
        const signal = opts?.signal;
        const deadline = opts?.timeoutMs !== undefined ? Date.now() + opts.timeoutMs : Infinity;
        const token = this.claim().token;
        for (;;) {
            const lw = this.lockWord();
            const tokenBits = lw & TOKEN_MASK;
            if (tokenBits === 0) {
                if (Atomics.compareExchange(this.view, LOCK_WORD, lw, token) === lw) {
                    Atomics.add(this.view, SEQ, 1);
                    this.held = true;
                    this.b.mutexTrackHeld(this.name, this.view, token);
                    return { ownerDied: Atomics.exchange(this.view, OWNER_DIED, 0) === 1 };
                }
                continue;
            }
            if (tokenBits === token) {
                throw new errors_1.MembridgeError('E_DEADLOCK', 'mutex already locked by this thread', {
                    segmentName: this.name,
                });
            }
            if (!this.b.mutexOwnerAlive(this.view, tokenBits)) {
                const stolen = Atomics.compareExchange(this.view, LOCK_WORD, lw, token | (lw & HAS_WAITERS));
                if (stolen === lw) {
                    Atomics.store(this.view, OWNER_DIED, 1);
                    Atomics.add(this.view, SEQ, 1);
                    this.held = true;
                    this.b.mutexTrackHeld(this.name, this.view, token);
                    return { ownerDied: true };
                }
                continue;
            }
            if (!(lw & HAS_WAITERS)) {
                Atomics.or(this.view, LOCK_WORD, HAS_WAITERS);
            }
            if (signal?.aborted)
                throw signal.reason ?? new errors_1.MembridgeError('E_TIMEOUT', 'aborted');
            const expected = this.lockWord();
            const waitP = (0, sync_1.waitAsync)(this.view, LOCK_WORD, expected, sleepSliceMs);
            const abortP = signal !== undefined
                ? new Promise((_, rej) => {
                    const onAbort = () => rej(signal.reason ?? new errors_1.MembridgeError('E_TIMEOUT', 'aborted'));
                    signal.addEventListener('abort', onAbort, { once: true });
                    waitP.finally(() => signal.removeEventListener('abort', onAbort));
                })
                : null;
            const slice = (await (abortP !== null ? Promise.race([waitP, abortP]) : waitP));
            if (signal?.aborted)
                throw signal.reason ?? new errors_1.MembridgeError('E_TIMEOUT', 'aborted');
            if (opts?.timeoutMs !== undefined && Date.now() >= deadline) {
                throw new errors_1.MembridgeError('E_TIMEOUT', 'mutex lock timed out', { segmentName: this.name });
            }
            void slice;
        }
    }
    /** Release. Throws E_NOT_OWNER when this instance does not hold the lock. */
    unlock() {
        if (!this.held || this.claimed === null) {
            throw new errors_1.MembridgeError('E_NOT_OWNER', 'mutex is not locked by this instance', {
                segmentName: this.name,
            });
        }
        const token = this.claimed.token;
        for (;;) {
            const lw = this.lockWord();
            if ((lw & TOKEN_MASK) !== token) {
                // our unlock raced a steal: the stealer owns it now
                this.held = false;
                this.b.mutexUntrackHeld(this.view, token);
                throw new errors_1.MembridgeError('E_NOT_OWNER', 'mutex was stolen after owner death', {
                    segmentName: this.name,
                });
            }
            if (Atomics.compareExchange(this.view, LOCK_WORD, lw, 0) === lw) {
                this.held = false;
                this.b.mutexUntrackHeld(this.view, token);
                if (lw & HAS_WAITERS) {
                    this.b.syncNotify(this.view, LOCK_WORD, 1);
                }
                return;
            }
        }
    }
    /** Run fn while holding the lock; fn receives { ownerDied } (§7.3). */
    withLock(fn, opts) {
        const res = this.lock(opts);
        try {
            return fn(res);
        }
        finally {
            this.unlock();
        }
    }
    /** Async variant of {@link withLock}. */
    async withLockAsync(fn, opts) {
        const res = await this.lockAsync(opts);
        try {
            return await fn(res);
        }
        finally {
            this.unlock();
        }
    }
    /** Diagnostics: current seq counter (acquisitions so far). */
    get sequence() {
        return Atomics.load(this.view, SEQ);
    }
    // exposed for tests
    identityWords() {
        return selfIdentityWords();
    }
}
exports.Mutex = Mutex;
// slot word offsets (exposed for the §13 crash-crafting tests)
exports.SLOT_LAYOUT = {
    SLOTS,
    SLOT_WORDS,
    SLOT_COUNT,
    PID: 0,
    TID: 1,
    START_LO: 2,
    START_HI: 3,
    NS_LO: 4,
    NS_HI: 5,
    GEN: 6,
    STATE: 7,
    STATE_ACTIVE: 1,
    STATE_FREE: 0,
};
//# sourceMappingURL=mutex.js.map