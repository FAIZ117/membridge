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

import { MembridgeError } from './errors';
import { nativeOrThrow } from './native';
import { open, unlink } from './core';
import { waitAsync } from './sync'; // pinned loop while async waits pend

// Data-region layout (mirrors cc/mutex.h)
const HEADER_WORDS = 4; // lockWord, ownerDied, seq, pad
const SLOT_WORDS = 8; // pid, tid, startLo, startHi, nsLo, nsHi, gen, state
const SLOT_COUNT = 64;
export const MUTEX_DATA_BYTES = (HEADER_WORDS + SLOT_COUNT * SLOT_WORDS) * 4;

const HAS_WAITERS = 0x80000000;
const TOKEN_MASK = 0x7fffffff;
const GEN_MASK = 0x7fff;

const LOCK_WORD = 0;
const OWNER_DIED = 1;
const SEQ = 2;
const SLOTS = HEADER_WORDS;

const sleepSliceMs = 250;

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

interface Slot {
  slot: number;
  gen: number;
  token: number;
}

function selfIdentityWords(): { pid: number; tid: number; start: number; ns: number } {
  const b = nativeOrThrow();
  const id = b.selfIdentity();
  return { pid: id.pid, tid: id.threadId, start: id.startTime, ns: id.pidNsInode };
}

export class Mutex {
  private readonly name: string;
  private readonly view: Int32Array;
  private readonly b: ReturnType<typeof nativeOrThrow>;
  private claimed: Slot | null = null;
  private held = false;

  private constructor(name: string, view: Int32Array) {
    this.name = name;
    this.view = view;
    this.b = nativeOrThrow();
  }

  /** Open (create or join) a mutex segment. Zero-initialized by open(). */
  static open(name: string, opts?: MutexOptions): Mutex {
    // E_EXISTS when a foreign/foreign-sized segment of that name exists is
    // surfaced from open(); mutex segments have a fixed data size.
    const sab = open(name, MUTEX_DATA_BYTES, { kind: 'mutex', winGlobal: opts?.winGlobal });
    return new Mutex(name, new Int32Array(sab));
  }

  /** Remove the mutex segment (POSIX shm_unlink). */
  static unlink(name: string): void {
    unlink(name);
  }

  private claim(): Slot {
    if (this.claimed !== null) return this.claimed;
    const r = this.b.mutexClaimSlot(this.name, this.view);
    this.claimed = { slot: r.slot, gen: r.gen, token: r.token };
    return this.claimed;
  }

  private lockWord(): number {
    return Atomics.load(this.view, LOCK_WORD);
  }

  private isMyToken(tokenBits: number): boolean {
    return tokenBits === (this.claimed?.token ?? -1);
  }

  /** Acquire the lock, blocking. */
  lock(opts?: LockOptions): LockResult {
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
        throw new MembridgeError('E_DEADLOCK', 'mutex already locked by this thread', {
          segmentName: this.name,
        });
      }
      // Dead owner? Steal: single CAS deadToken -> myToken (§7.3).
      if (!this.b.mutexOwnerAlive(this.view, tokenBits)) {
        const stolen = Atomics.compareExchange(
          this.view,
          LOCK_WORD,
          lw,
          token | (lw & HAS_WAITERS),
        );
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
        throw new MembridgeError('E_TIMEOUT', 'mutex lock timed out', { segmentName: this.name });
      }
      this.b.syncWait(this.view, LOCK_WORD, expected, sleepSliceMs);
      if (opts?.timeoutMs !== undefined && Date.now() >= deadline) {
        throw new MembridgeError('E_TIMEOUT', 'mutex lock timed out', { segmentName: this.name });
      }
    }
  }

  /** Acquire without blocking. Returns false when the lock is held (even by a
   * dead owner — tryLock does not steal, §7.3). */
  tryLock(): boolean {
    const token = this.claim().token;
    const lw = this.lockWord();
    if ((lw & TOKEN_MASK) === token) {
      throw new MembridgeError('E_DEADLOCK', 'mutex already locked by this thread', {
        segmentName: this.name,
      });
    }
    if ((lw & TOKEN_MASK) !== 0) return false;
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
  async lockAsync(opts?: LockOptions & { signal?: AbortSignal }): Promise<LockResult> {
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
        throw new MembridgeError('E_DEADLOCK', 'mutex already locked by this thread', {
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
      if (signal?.aborted) throw signal.reason ?? new MembridgeError('E_TIMEOUT', 'aborted');
      const expected = this.lockWord();
      const waitP = waitAsync(this.view, LOCK_WORD, expected, sleepSliceMs);
      const abortP =
        signal !== undefined
          ? new Promise<never>((_, rej) => {
              const onAbort = () => rej(signal.reason ?? new MembridgeError('E_TIMEOUT', 'aborted'));
              signal.addEventListener('abort', onAbort, { once: true });
              waitP.finally(() => signal.removeEventListener('abort', onAbort));
            })
          : null;
      const slice = (await (abortP !== null ? Promise.race([waitP, abortP]) : waitP)) as string;
      if (signal?.aborted) throw signal.reason ?? new MembridgeError('E_TIMEOUT', 'aborted');
      if (opts?.timeoutMs !== undefined && Date.now() >= deadline) {
        throw new MembridgeError('E_TIMEOUT', 'mutex lock timed out', { segmentName: this.name });
      }
      void slice;
    }
  }

  /** Release. Throws E_NOT_OWNER when this instance does not hold the lock. */
  unlock(): void {
    if (!this.held || this.claimed === null) {
      throw new MembridgeError('E_NOT_OWNER', 'mutex is not locked by this instance', {
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
        throw new MembridgeError('E_NOT_OWNER', 'mutex was stolen after owner death', {
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
  withLock<T>(fn: (lock: LockResult) => T, opts?: LockOptions): T {
    const res = this.lock(opts);
    try {
      return fn(res);
    } finally {
      this.unlock();
    }
  }

  /** Async variant of {@link withLock}. */
  async withLockAsync<T>(fn: (lock: LockResult) => T, opts?: LockOptions & { signal?: AbortSignal }): Promise<T> {
    const res = await this.lockAsync(opts);
    try {
      return await fn(res);
    } finally {
      this.unlock();
    }
  }

  /** Diagnostics: current seq counter (acquisitions so far). */
  get sequence(): number {
    return Atomics.load(this.view, SEQ);
  }

  // exposed for tests
  identityWords(): { pid: number; tid: number; start: number; ns: number } {
    return selfIdentityWords();
  }
}

// slot word offsets (exposed for the §13 crash-crafting tests)
export const SLOT_LAYOUT = {
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
