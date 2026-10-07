// ringbuffer.ts — §8 zero-copy SPSC ring buffer with variable-length messages.
//
// Layout (word offsets into the data region; mirrors cc/mutex.h ring constants):
//   word  0: i32 head (producer's committed count, free-running, cache line 0)
//   word 16: i32 tail (consumer's released count, cache line 1)
//   word 32: i32 producer token   word 33: i32 consumer token (role claims)
//   word 34: u32 capacity (diagnostic — capacity is derived from the segment
//            size: dataBytes - RING_HEADER_BYTES)  word 35: u32 maxMessage
//   word 36..51: two §7-style participant slots (producer 0, consumer 1)
//   byte 256: data[capacity]
//
// Framing: each message = u32 length + payload, total padded to 8 bytes. A
// message that does not fit before the end of the buffer is preceded by a
// SKIP marker (u32 0xFFFFFFFF) and starts at offset 0. maxMessage <=
// capacity/2 - 8 guarantees a message always fits contiguously once the
// buffer drains, so a skip can never deadlock.
//
// Crash safety by construction: data becomes visible only through commit()'s
// store to head; a producer that dies mid-reserve leaves nothing visible; a
// consumer that dies mid-peek leaves the message unreleased, so the next
// consumer sees it again (at-least-once, §8.2).

import { MembridgeError } from './errors';
import { nativeOrThrow } from './native';
import { open, type OpenOptions } from './core';
import { waitAsync } from './sync';

export const RING_HEADER_BYTES = 256;
export const SKIP_MARKER = 0xffffffff;
export const MIN_CAPACITY = 4096;
export const MAX_CAPACITY = 1024 * 1024 * 1024;

// word offsets
const HEAD = 0;
const TAIL = 16;
const PRODUCER_TOKEN = 32;
const CONSUMER_TOKEN = 33;
const CAPACITY = 34;
const MAX_MESSAGE = 35;
const DATA_WORD = RING_HEADER_BYTES / 4; // 64
const CONSUMER_SLOT = 1;  // role slot index (producer 0, consumer 1)
// Parked flags (review P3): word 1 shares head's cache line, word 17 tail's.
// The other side notifies only when the flag is set, so an unparked
// data-plane operation costs zero FUTEX_WAKEs. A crashed waiter leaves its
// flag set — the cost is one extra wake, never correctness.
const CONSUMER_PARKED = 1;   // consumer waits on HEAD
const PRODUCER_PARKED = 17;  // producer waits on TAIL

const PRODUCER_SLOT = 0;

const sleepSliceMs = 250;
const nowMs = (): number => performance.now();  // monotonic (review F35)
const sliceOf = (deadline: number): number =>
  Math.max(1, Math.min(sleepSliceMs, deadline - nowMs()));

// Same timeout contract as membridge/sync and Mutex (PLAN §7.3/§8.2), except
// that 0 is valid here and means "non-blocking". NaN used to wait forever
// (round-3 C14).
const MAX_TIMEOUT_MS = 2 ** 31;
function checkTimeoutMs(timeoutMs: number | undefined, who: string): void {
  if (timeoutMs !== undefined &&
      (!Number.isFinite(timeoutMs) || timeoutMs < 0 || timeoutMs > MAX_TIMEOUT_MS)) {
    throw new MembridgeError('E_NAME_INVALID',
      `${who}: timeoutMs must be a finite number in [0, ${MAX_TIMEOUT_MS}] (got ${timeoutMs})`);
  }
}

// R15b + round-3 C10/C24: when a ring instance (producer or consumer) is
// collected, drop its role reference so a replacement can open on this thread
// without waiting for thread exit. Producers no longer need to stay pinned:
// RingProducer.open hands out the thread's live instance for a mapping, so
// there is never a live sibling whose writes a replacement could interleave.
const roleRegistry = new FinalizationRegistry<{ data: Int32Array; slot: number }>((v) => {
  try {
    nativeOrThrow().mutexUnregisterRole(v.data, v.slot);
  } catch {
    // isolate tearing down: the env-cleanup hook covers the remainder
  }
});

// One live producer instance per (thread, name, mapping) — round-3 C24: two
// producer objects on one thread each kept their own pending reserve, so
// interleaved async reserve/commit corrupted the ring.
const liveProducers = new Map<string, WeakRef<RingProducer>>();

function align8(n: number): number {
  return (n + 7) & ~7;
}

function assertCapacity(name: string, capacity: number, maxMessage: number | undefined): number {
  if (!Number.isSafeInteger(capacity) || capacity < MIN_CAPACITY || capacity > MAX_CAPACITY ||
      (capacity & (capacity - 1)) !== 0) {
    throw new MembridgeError(
      'E_SIZE_INVALID',
      `capacity must be a power of two between ${MIN_CAPACITY} and ${MAX_CAPACITY}`,
      { segmentName: name, requested: capacity },
    );
  }
  const cap = maxMessage ?? Math.floor(capacity / 2) - 8;
  if (!Number.isSafeInteger(cap) || cap < 1 || cap > Math.floor(capacity / 2) - 8) {
    throw new MembridgeError(
      'E_SIZE_INVALID',
      `maxMessage must be <= capacity/2 - 8 (= ${Math.floor(capacity / 2) - 8})`,
      { segmentName: name, requested: cap },
    );
  }
  return cap;
}

export interface ProducerOptions {
  /** Power of two, 4 KiB .. 1 GiB. Omit: join at the existing size. */
  capacity?: number;
  /** <= capacity/2 - 8. Default: capacity/2 - 8. */
  maxMessage?: number;
  winGlobal?: boolean;
}

export interface ReserveOptions {
  timeoutMs?: number;
}

interface Reserved {
  headSnapshot: number;
  pos: number;
  framed: number;
  payload: Uint8Array;
}

export class RingProducer {
  readonly name: string;
  private readonly view: Int32Array;
  private readonly data: Uint8Array;
  private readonly capacity: number;
  private readonly maxMessage: number;
  private readonly b: ReturnType<typeof nativeOrThrow>;
  private closed = false;
  private pending: Reserved | null = null;

  private constructor(name: string, view: Int32Array, capacity: number, maxMessage: number) {
    this.name = name;
    this.view = view;
    this.data = new Uint8Array(view.buffer, RING_HEADER_BYTES, capacity);
    this.capacity = capacity;
    this.maxMessage = maxMessage;
    this.b = nativeOrThrow();
  }

  /** Create or join as the producer (SPSC: one live producer per ring). */
  static open(name: string, opts?: ProducerOptions & OpenOptions): RingProducer {
    const b = nativeOrThrow();
    let capacity: number | undefined;
    let maxMessage: number | undefined;
    if (opts?.capacity !== undefined) {
      capacity = opts.capacity;
      maxMessage = assertCapacity(name, capacity, opts.maxMessage);
      if (capacity > MAX_CAPACITY) {
        throw new MembridgeError('E_SIZE_INVALID', 'capacity too large', { segmentName: name });
      }
    }
    const sab =
      capacity !== undefined
        ? open(name, RING_HEADER_BYTES + capacity, { kind: 'ring', winGlobal: opts?.winGlobal })
        : open(name, { kind: 'ring', winGlobal: opts?.winGlobal });
    const view = new Int32Array(sab);
    const cap = capacity ?? sab.byteLength - RING_HEADER_BYTES;
    const mm = maxMessage ?? Math.floor(cap / 2) - 8;
    // §8.2 init: a joiner's capacity/maxMessage must match the header —
    // validated BEFORE the role claim so a mismatch never steals the seat.
    if (cap < MIN_CAPACITY || cap > MAX_CAPACITY || (cap & (cap - 1)) !== 0) {
      throw new MembridgeError(
        'E_SIZE_INVALID',
        `ring capacity must be a power of two between ${MIN_CAPACITY} and ${MAX_CAPACITY}`,
        { segmentName: name, requested: cap },
      );
    }
    const hdrCap = Atomics.load(view, CAPACITY);
    if (hdrCap !== 0 && hdrCap !== cap) {
      throw new MembridgeError('E_SIZE_MISMATCH',
        `ring capacity ${cap} does not match the existing ${hdrCap}`, {
          segmentName: name, requested: cap, existing: hdrCap,
        });
    }
    const hdrMax = Atomics.load(view, MAX_MESSAGE);
    if (hdrMax !== 0 && hdrMax !== mm) {
      throw new MembridgeError('E_SIZE_MISMATCH',
        `ring maxMessage ${mm} does not match the existing ${hdrMax}`, {
          segmentName: name, requested: mm, existing: hdrMax,
        });
    }
    // This thread's live producer for the same mapping IS the producer.
    const live = liveProducers.get(name)?.deref();
    if (live !== undefined && !live.closed && b.sameMemory(live.view, view)) {
      if (live.capacity !== cap || live.maxMessage !== mm) {
        throw new MembridgeError('E_SIZE_MISMATCH',
          'ring capacity/maxMessage differ from the producer already open on this thread', {
            segmentName: name, requested: cap, existing: live.capacity,
          });
      }
      return live;
    }
    b.ringClaimRole(name, view, true);
    // A fresh role holder is not parked: clear a flag a crashed predecessor
    // left set, or every consumer release would pay a futile FUTEX_WAKE
    // (round-3 C13 — R15a never landed before).
    Atomics.store(view, PRODUCER_PARKED, 0);
    const p = new RingProducer(name, view, cap, mm);
    roleRegistry.register(p, { data: view, slot: PRODUCER_SLOT }, p);
    liveProducers.set(name, new WeakRef(p));
    if (capacity !== undefined && Atomics.load(view, CAPACITY) === 0) {
      Atomics.store(view, CAPACITY, cap);
      Atomics.store(view, MAX_MESSAGE, mm);
    }
    return p;
  }

  /** Release the producer role (idempotent). An uncommitted reserve is
   * dropped (nothing becomes visible). Any later call throws E_CLOSED. */
  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.pending = null;
    roleRegistry.unregister(this);
    if (liveProducers.get(this.name)?.deref() === this) liveProducers.delete(this.name);
    this.b.mutexUnregisterRole(this.view, PRODUCER_SLOT);
  }

  private assertOpen(): void {
    if (this.closed) {
      throw new MembridgeError('E_CLOSED', 'ring producer is closed', { segmentName: this.name });
    }
  }

  private head(): number {
    return Atomics.load(this.view, HEAD);
  }
  private tail(): number {
    return Atomics.load(this.view, TAIL);
  }

  /** Two-phase write: returns a Uint8Array view straight into shared memory.
   * Call {@link commit} to publish. Throws E_RING_STATE if already reserved. */
  reserve(n: number, opts?: ReserveOptions): Uint8Array {
    const framed = this.checkReserve(n, opts?.timeoutMs, 'reserve');
    const deadline = opts?.timeoutMs !== undefined ? nowMs() + opts.timeoutMs : Infinity;
    let parked = false;
    try {
      for (;;) {
        const tailSeen = this.tail();
        const view = this.tryReserve(n, framed);
        if (view !== null) return view;
        if (opts?.timeoutMs === 0 || (opts?.timeoutMs !== undefined && nowMs() >= deadline)) {
          throw this.fullError('reserve');
        }
        // Park on TAIL: announce, re-check against the value the failed
        // attempt saw, then wait on it (§8.2 + review P3).
        Atomics.store(this.view, PRODUCER_PARKED, 1);
        parked = true;
        if (this.tail() !== tailSeen) continue;  // space appeared meanwhile
        this.b.syncWait(this.view, TAIL, tailSeen, sliceOf(deadline));
        if (opts?.timeoutMs !== undefined && nowMs() >= deadline) throw this.fullError('reserve');
      }
    } finally {
      if (parked) Atomics.store(this.view, PRODUCER_PARKED, 0);
    }
  }

  private assertNoPending(): void {
    if (this.pending !== null) {
      throw new MembridgeError('E_RING_STATE', 'previous reserve not committed', {
        segmentName: this.name,
      });
    }
  }

  private checkReserve(n: number, timeoutMs: number | undefined, who: string): number {
    this.assertOpen();
    checkTimeoutMs(timeoutMs, who);
    this.assertNoPending();
    if (!Number.isSafeInteger(n) || n < 0) {
      throw new MembridgeError('E_SIZE_INVALID', 'message size must be a safe integer >= 0', {
        segmentName: this.name,
      });
    }
    if (n > this.maxMessage) {
      throw new MembridgeError('E_MESSAGE_TOO_LARGE',
        `message of ${n} bytes exceeds maxMessage of ${this.maxMessage}`, {
          segmentName: this.name,
          requested: n,
          existing: this.maxMessage,
        });
    }
    return align8(4 + n);
  }

  private fullError(who: string): MembridgeError {
    return new MembridgeError('E_TIMEOUT', `ring full: ${who} timed out`, { segmentName: this.name });
  }

  // One non-blocking attempt (round-3 C19: no exception on the full path —
  // reserveAsync used to pay ~11 µs per failed attempt building an Error).
  private tryReserve(n: number, framed: number): Uint8Array | null {
    for (;;) {
      const head = this.head();
      const tail = this.tail();
      const used = (head - tail) >>> 0;
      const pos = head & (this.capacity - 1);
      if (pos + framed <= this.capacity) {
        if (this.capacity - used < framed) return null;
        this.pending = {
          headSnapshot: head,
          pos,
          framed,
          payload: this.data.subarray(pos + 4, pos + 4 + n),
        };
        return this.pending.payload;
      }
      // SKIP: advance past the end once the consumer drained up to `pos`.
      // The marker is visible to the consumer through the head advance.
      if (used <= pos && pos - used >= framed) {
        Atomics.store(this.view, DATA_WORD + pos / 4, -1); // u32 SKIP_MARKER
        Atomics.store(this.view, HEAD, head + (this.capacity - pos));
        this.wakeConsumer();
        continue; // re-loop: now at offset 0
      }
      return null;
    }
  }

  /** Publish the reserved message: length field lands first, then the
   * release store on head + notify (§8.2). */
  commit(): void {
    this.assertOpen();
    if (this.pending === null) {
      throw new MembridgeError('E_RING_STATE', 'no reserve to commit', { segmentName: this.name });
    }
    const { headSnapshot, pos, framed, payload } = this.pending;
    this.pending = null;
    Atomics.store(this.view, DATA_WORD + pos / 4, payload.length);
    Atomics.store(this.view, HEAD, headSnapshot + framed);
    this.wakeConsumer();
  }

  // A plain load, deliberately NOT an exchange (round-3 C20 was tried and
  // reverted): a notifier's flag check is not tied to the counter value it
  // published, so a LATE exchange from an earlier commit could consume the
  // peer's NEW park announcement — its wake spent before the peer slept —
  // and the next commit then saw the flag clear and woke nobody (a full
  // 250 ms slice, reproduced by test/round3.test.ts). The waiter clears its
  // own flag on exit; the cost is extra wakes while it is parked, never a
  // lost one.
  private wakeConsumer(): void {
    if (Atomics.load(this.view, CONSUMER_PARKED) === 1) {
      this.b.syncNotify(this.view, HEAD, 1);
    }
  }

  /** Convenience: reserve + copy + commit (one copy). */
  write(bytes: Uint8Array, opts?: ReserveOptions): void {
    const view = this.reserve(bytes.length, opts);
    view.set(bytes);
    this.commit();
  }

  /** Async variant of {@link reserve} (§8.2): waits ride the §6 waiter
   * threads; resolves with the same zero-copy view. */
  async reserveAsync(n: number, opts?: ReserveOptions): Promise<Uint8Array> {
    const framed = this.checkReserve(n, opts?.timeoutMs, 'reserveAsync');
    const deadline = opts?.timeoutMs !== undefined ? nowMs() + opts.timeoutMs : Infinity;
    let parked = false;
    try {
      for (;;) {
        // Snapshot BEFORE the attempt (round-3 C6): a release landing between
        // the failed attempt and the flag store saw no flag and did not
        // notify, so the re-check must compare against what the attempt saw —
        // comparing two back-to-back reads missed it and parked a full slice.
        const tailSeen = this.tail();
        this.assertOpen();
        // Round-4 E4-2: another reserve on this instance (a concurrent
        // reserveAsync, or a sync reserve whose commit spans an await) may
        // have taken the region while we were parked — never overwrite it.
        this.assertNoPending();
        const view = this.tryReserve(n, framed);
        if (view !== null) return view;
        if (opts?.timeoutMs === 0 || (opts?.timeoutMs !== undefined && nowMs() >= deadline)) {
          throw this.fullError('reserveAsync');
        }
        Atomics.store(this.view, PRODUCER_PARKED, 1);
        parked = true;
        if (this.tail() !== tailSeen) continue;
        await waitAsync(this.view, TAIL, tailSeen, sliceOf(deadline));
        if (opts?.timeoutMs !== undefined && nowMs() >= deadline) {
          this.assertOpen();
          this.assertNoPending();
          const last = this.tryReserve(n, framed);  // last look before giving up
          if (last !== null) return last;
          throw this.fullError('reserveAsync');
        }
      }
    } finally {
      if (parked && !this.closed) Atomics.store(this.view, PRODUCER_PARKED, 0);
    }
  }

  get capacityBytes(): number {
    return this.capacity;
  }

  get maxMessageBytes(): number {
    return this.maxMessage;
  }
}

interface Peeked {
  framed: number;
  tailSnapshot: number;
  payload: Uint8Array;
}

export class RingConsumer {
  readonly name: string;
  private readonly view: Int32Array;
  private readonly data: Uint8Array;
  private readonly capacity: number;
  private readonly b: ReturnType<typeof nativeOrThrow>;
  private closed = false;
  private pending: Peeked | null = null;

  private constructor(name: string, view: Int32Array, capacity: number) {
    this.name = name;
    this.view = view;
    this.data = new Uint8Array(view.buffer, RING_HEADER_BYTES, capacity);
    this.capacity = capacity;
    this.b = nativeOrThrow();
  }

  /** Join as the consumer (SPSC: one live consumer per ring). */
  static open(name: string, opts?: { winGlobal?: boolean; initTimeoutMs?: number }): RingConsumer {
    const b = nativeOrThrow();
    const sab = open(name, { kind: 'ring', winGlobal: opts?.winGlobal, initTimeoutMs: opts?.initTimeoutMs });
    const capacity = sab.byteLength - RING_HEADER_BYTES;
    if (capacity < MIN_CAPACITY || (capacity & (capacity - 1)) !== 0) {
      throw new MembridgeError('E_INCOMPATIBLE', 'segment is not a membridge ring', {
        segmentName: name,
      });
    }
    const view = new Int32Array(sab);
    // Cross-check the capacity word a producer recorded (§8.2): a segment of
    // ring kind whose size disagrees with its own header is not usable.
    const hdrCap = Atomics.load(view, CAPACITY);
    if (hdrCap !== 0 && hdrCap !== capacity) {
      throw new MembridgeError('E_INCOMPATIBLE',
        `ring header capacity ${hdrCap} does not match the segment size (${capacity})`, {
          segmentName: name, requested: capacity, existing: hdrCap,
        });
    }
    b.ringClaimRole(name, view, false);
    Atomics.store(view, CONSUMER_PARKED, 0);  // round-3 C13: see RingProducer.open
    const c = new RingConsumer(name, view, capacity);
    roleRegistry.register(c, { data: view, slot: CONSUMER_SLOT }, c);
    return c;
  }

  /** Release this instance's consumer role (idempotent). An open peek is
   * abandoned unreleased, so the message is redelivered to the next consumer
   * (at-least-once). Any later call throws E_CLOSED (round-3 C10: a closed
   * consumer used to keep reading alongside its replacement). */
  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.pending = null;
    roleRegistry.unregister(this);
    this.b.mutexUnregisterRole(this.view, CONSUMER_SLOT);
  }

  private assertOpen(): void {
    if (this.closed) {
      throw new MembridgeError('E_CLOSED', 'ring consumer is closed', { segmentName: this.name });
    }
  }

  private head(): number {
    return Atomics.load(this.view, HEAD);
  }
  private tail(): number {
    return Atomics.load(this.view, TAIL);
  }

  /** Two-phase read: view into shared memory, valid until {@link release}.
   * Returns null on timeout. Throws E_RING_STATE if a peek is already open. */
  peek(opts?: ReserveOptions): Uint8Array | null {
    this.assertOpen();
    checkTimeoutMs(opts?.timeoutMs, 'peek');
    if (this.pending !== null) {
      throw new MembridgeError('E_RING_STATE', 'previous peek not released', {
        segmentName: this.name,
      });
    }
    const deadline = opts?.timeoutMs !== undefined ? nowMs() + opts.timeoutMs : Infinity;
    let parked = false;
    try {
      for (;;) {
        const head = this.head();
        const tail = this.tail();
        if (head === tail) {
          if (opts?.timeoutMs !== undefined && nowMs() >= deadline) return null;
          if (opts?.timeoutMs === 0) return null;
          // Park on HEAD: announce, re-check, then wait (review P3).
          Atomics.store(this.view, CONSUMER_PARKED, 1);
          parked = true;
          const headNow = this.head();
          if (headNow !== head) continue;
          this.b.syncWait(this.view, HEAD, headNow, sliceOf(deadline));
          if (opts?.timeoutMs !== undefined && nowMs() >= deadline &&
              this.head() === this.tail()) {
            return null;
          }
          continue;
        }
        const pos = tail & (this.capacity - 1);
        const lenWord = Atomics.load(this.view, DATA_WORD + pos / 4);
        if (lenWord === -1) {
          // SKIP marker: the next message starts at the buffer boundary
          const next = (tail & ~(this.capacity - 1)) + this.capacity;
          Atomics.compareExchange(this.view, TAIL, tail, next);
          this.wakeProducer();
          continue;
        }
        const framed = align8(4 + lenWord);
        if (((head - tail) >>> 0) < framed) {
          // length visible but payload not fully committed yet
          if (opts?.timeoutMs !== undefined && nowMs() >= deadline) return null;
          Atomics.store(this.view, CONSUMER_PARKED, 1);
          parked = true;
          const headNow = this.head();
          if (headNow !== head) continue;
          this.b.syncWait(this.view, HEAD, headNow, sliceOf(deadline));
          if (opts?.timeoutMs !== undefined && nowMs() >= deadline) return null;
          continue;
        }
        this.pending = {
          framed,
          tailSnapshot: tail,
          payload: this.data.subarray(pos + 4, pos + 4 + lenWord),
        };
        return this.pending.payload;
      }
    } finally {
      if (parked) Atomics.store(this.view, CONSUMER_PARKED, 0);
    }
  }

  /** Only now does tail advance (§8.2). */
  release(): void {
    this.assertOpen();
    if (this.pending === null) {
      throw new MembridgeError('E_RING_STATE', 'no peek to release', { segmentName: this.name });
    }
    const { framed, tailSnapshot } = this.pending;
    this.pending = null;
    Atomics.compareExchange(this.view, TAIL, tailSnapshot, tailSnapshot + framed);
    this.wakeProducer();
  }

  // Plain load, not an exchange — see RingProducer.wakeConsumer (C20).
  private wakeProducer(): void {
    if (Atomics.load(this.view, PRODUCER_PARKED) === 1) {
      this.b.syncNotify(this.view, TAIL, 1);
    }
  }

  /** Async variant of {@link peek} (§8.2). Resolves null on timeout. */
  async peekAsync(opts?: ReserveOptions): Promise<Uint8Array | null> {
    this.assertOpen();
    checkTimeoutMs(opts?.timeoutMs, 'peekAsync');
    const deadline = opts?.timeoutMs !== undefined ? nowMs() + opts.timeoutMs : Infinity;
    let parked = false;
    try {
      for (;;) {
        // Snapshot BEFORE the attempt (round-3 C6), as in reserveAsync.
        const headSeen = this.head();
        const msg = this.peek({ timeoutMs: 0 });
        if (msg !== null) return msg;
        if (opts?.timeoutMs === 0 || (opts?.timeoutMs !== undefined && nowMs() >= deadline)) {
          return null;
        }
        Atomics.store(this.view, CONSUMER_PARKED, 1);
        parked = true;
        if (this.head() !== headSeen) continue;
        await waitAsync(this.view, HEAD, headSeen, sliceOf(deadline));
        if (opts?.timeoutMs !== undefined && nowMs() >= deadline) {
          return this.peek({ timeoutMs: 0 });  // last look before giving up
        }
      }
    } finally {
      if (parked && !this.closed) Atomics.store(this.view, CONSUMER_PARKED, 0);
    }
  }

  /** Convenience: peek + copy + release. Returns null on timeout. */
  read(opts?: ReserveOptions): Uint8Array | null {
    const view = this.peek(opts);
    if (view === null) return null;
    const copy = new Uint8Array(view.length);
    copy.set(view);
    this.release();
    return copy;
  }

  get capacityBytes(): number {
    return this.capacity;
  }
}

