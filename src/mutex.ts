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

// Monotonic clock (Date.now() jumps with NTP — review F35).
const nowMs = (): number => performance.now();
// Wait slices clamp to the remaining timeout so a lock({timeoutMs: 1}) ends
// after ~1 ms, not one full 250 ms slice (review P4).
const sliceOf = (deadline: number): number =>
  Math.max(1, Math.min(sleepSliceMs, deadline - nowMs()));

const sleepSliceMs = 250; // dead-owner detection cadence (§7.3)

// R20: NaN/Infinity/negative timeouts used to slip through as an infinite
// park with no liveness re-check.
function checkTimeoutMs(timeoutMs: number | undefined, who: string): void {
  if (timeoutMs !== undefined && (!Number.isFinite(timeoutMs) || timeoutMs <= 0)) {
    throw new MembridgeError('E_SIZE_INVALID',
      `${who}: timeoutMs must be a finite positive number (got ${timeoutMs})`);
  }
}

export interface LockOptions {
  /** Total time to wait before E_TIMEOUT (<= 2^31 ms). Default: wait indefinitely. */
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

// R12: when a Mutex instance is collected, drop its native entry (and the
// BackingStore pin that keeps an unlinked segment's pages + fd alive).
const claimRegistry = new FinalizationRegistry<{ data: Int32Array; slot: number }>((v) => {
  try {
    nativeOrThrow().mutexUnregisterClaim(v.data, v.slot);
  } catch {
    // isolate tearing down: the env hook covers the remainder
  }
});

export class Mutex {
  private readonly name: string;
  private readonly view: Int32Array;
  private readonly b: ReturnType<typeof nativeOrThrow>;
  private claimed: Slot | null = null;
  private held = false;
  private registered = false;

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
    if (!this.registered) {
      // R11/R12: pin the SAB's own BackingStore with the native entry, and
      // unregister when this instance is collected (or closed).
      this.b.mutexRegisterPin(this.name, this.view, r.slot, r.token,
        this.view.buffer as SharedArrayBuffer);
      claimRegistry.register(this, { data: this.view, slot: r.slot }, this);
      this.registered = true;
    }
    return this.claimed;
  }

  /** Release this instance's native claim (pin + teardown entry). Idempotent. */
  close(): void {
    if (this.claimed !== null && this.registered) {
      claimRegistry.unregister(this);
      this.b.mutexUnregisterClaim(this.view, this.claimed.slot);
      this.registered = false;
    }
  }

  private lockWord(): number {
    return Atomics.load(this.view, LOCK_WORD);
  }

  private isMyToken(tokenBits: number): boolean {
    return tokenBits === (this.claimed?.token ?? -1);
  }

  /** Acquire the lock, blocking. */
  lock(opts?: LockOptions): LockResult {
    checkTimeoutMs(opts?.timeoutMs, 'lock');
    const deadline = opts?.timeoutMs !== undefined ? nowMs() + opts.timeoutMs : Infinity;
    const token = this.claim().token;
    let waited = false;  // once we have parked, acquire PRESERVES HAS_WAITERS
    for (;;) {
      const lw = this.lockWord();
      const tokenBits = lw & TOKEN_MASK;
      if (tokenBits === 0) {
        // Acquire with a whole-word CAS: it clears a stale HAS_WAITERS left
        // by a lost race — but once WE have waited we keep the bit, because
        // other waiters may still be parked on the word we are about to own
        // (Drepper's mutex2; review F10/P2).
        const acq = waited ? token | HAS_WAITERS : token;
        if (Atomics.compareExchange(this.view, LOCK_WORD, lw, acq) === lw) {
          Atomics.add(this.view, SEQ, 1);
          this.held = true;
          return { ownerDied: Atomics.exchange(this.view, OWNER_DIED, 0) === 1 };
        }
        continue;
      }
      if (tokenBits === token) {
        throw new MembridgeError('E_DEADLOCK', 'mutex already locked by this thread', {
          segmentName: this.name,
        });
      }
      // Dead owner? Steal: single CAS deadToken -> myToken (§7.3). ownerDied
      // is reported to the STEALER only — storing the flag here made the
      // NEXT acquire report it a second time (review F22).
      if (!this.b.mutexOwnerAlive(this.view, tokenBits)) {
        const stolen = Atomics.compareExchange(
          this.view,
          LOCK_WORD,
          lw,
          token | (lw & HAS_WAITERS),
        );
        if (stolen === lw) {
          Atomics.add(this.view, SEQ, 1);
          this.held = true;
          return { ownerDied: true };
        }
        continue; // lost the race: retry
      }
      // Alive holder: ALWAYS OR the bit in (review R13: skipping the OR when
      // the first read had it set left a window where a newcomer's bare-token
      // acquire cleared it and we parked on a no-bit word — 250-500 ms
      // stalls). The OR returns the pre-OR word: if it was free, retry
      // immediately; otherwise park on prev|HAS_WAITERS, exactly what the
      // word holds now.
      const prev = Atomics.or(this.view, LOCK_WORD, HAS_WAITERS);
      if ((prev & TOKEN_MASK) === 0) {
        waited = true;
        continue;
      }
      if (opts?.timeoutMs !== undefined && nowMs() >= deadline) {
        throw new MembridgeError('E_TIMEOUT', 'mutex lock timed out', { segmentName: this.name });
      }
      this.b.syncWait(this.view, LOCK_WORD, prev | HAS_WAITERS, sliceOf(deadline));
      waited = true;
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
    // preserve the bit: parked waiters must not lose their wake source
    if (Atomics.compareExchange(this.view, LOCK_WORD, lw, token | (lw & HAS_WAITERS)) === lw) {
      Atomics.add(this.view, SEQ, 1);
      this.held = true;
      return true;
    }
    return false;
  }

  /** Acquire asynchronously; waits ride the membridge waiter threads (§6),
   * never the libuv pool. Aborts with the signal between wait slices. */
  async lockAsync(opts?: LockOptions & { signal?: AbortSignal }): Promise<LockResult> {
    const signal = opts?.signal;
    checkTimeoutMs(opts?.timeoutMs, 'lockAsync');
    const deadline = opts?.timeoutMs !== undefined ? nowMs() + opts.timeoutMs : Infinity;
    const token = this.claim().token;
    let waited = false;
    for (;;) {
      const lw = this.lockWord();
      const tokenBits = lw & TOKEN_MASK;
      if (tokenBits === 0) {
        const acq = waited ? token | HAS_WAITERS : token;
        if (Atomics.compareExchange(this.view, LOCK_WORD, lw, acq) === lw) {
          Atomics.add(this.view, SEQ, 1);
          this.held = true;
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
          Atomics.add(this.view, SEQ, 1);
          this.held = true;
          return { ownerDied: true };
        }
        continue;
      }
      const prev = Atomics.or(this.view, LOCK_WORD, HAS_WAITERS);
      if ((prev & TOKEN_MASK) === 0) {
        waited = true;
        continue;
      }
      if (signal?.aborted) throw signal.reason ?? new MembridgeError('E_TIMEOUT', 'aborted');
      if (opts?.timeoutMs !== undefined && nowMs() >= deadline) {
        throw new MembridgeError('E_TIMEOUT', 'mutex lock timed out', { segmentName: this.name });
      }
      const waitP = waitAsync(this.view, LOCK_WORD, prev | HAS_WAITERS, sliceOf(deadline));
      const abortP =
        signal !== undefined
          ? new Promise<never>((_, rej) => {
              const onAbort = () => rej(signal.reason ?? new MembridgeError('E_TIMEOUT', 'aborted'));
              signal.addEventListener('abort', onAbort, { once: true });
              // two-armed: a .finally-derived promise would carry rejections
              // unhandled and kill the process (review F11)
              void waitP.then(() => signal.removeEventListener('abort', onAbort),
                              () => signal.removeEventListener('abort', onAbort));
            })
          : null;
      await (abortP !== null ? Promise.race([waitP, abortP]) : waitP);
      waited = true;
      if (signal?.aborted) throw signal.reason ?? new MembridgeError('E_TIMEOUT', 'aborted');
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
        throw new MembridgeError('E_NOT_OWNER', 'mutex was stolen after owner death', {
          segmentName: this.name,
        });
      }
      if (Atomics.compareExchange(this.view, LOCK_WORD, lw, 0) === lw) {
        this.held = false;
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
