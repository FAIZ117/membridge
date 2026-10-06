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
// Parked flags (review P3): word 1 shares head's cache line, word 17 tail's.
// The other side notifies only when the flag is set, so an unparked
// data-plane operation costs zero FUTEX_WAKEs. A crashed waiter leaves its
// flag set — the cost is one extra wake, never correctness.
const CONSUMER_PARKED = 1;   // consumer waits on HEAD
const PRODUCER_PARKED = 17;  // producer waits on TAIL

const sleepSliceMs = 250;
const nowMs = (): number => performance.now();  // monotonic (review F35)
const sliceOf = (deadline: number): number =>
  Math.max(1, Math.min(sleepSliceMs, deadline - nowMs()));

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
  private claimed = false;
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
    b.ringClaimRole(name, view, true);
    const p = new RingProducer(name, view, cap, mm);
    p.claimed = true;
    if (capacity !== undefined && Atomics.load(view, CAPACITY) === 0) {
      Atomics.store(view, CAPACITY, cap);
      Atomics.store(view, MAX_MESSAGE, mm);
    }
    return p;
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
    if (this.pending !== null) {
      throw new MembridgeError('E_RING_STATE', 'previous reserve not committed', {
        segmentName: this.name,
      });
    }
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
    const framed = align8(4 + n);
    const deadline = opts?.timeoutMs !== undefined ? nowMs() + opts.timeoutMs : Infinity;
    const nonBlocking = opts?.timeoutMs === 0;
    let parked = false;
    try {
      for (;;) {
        const head = this.head();
        const tail = this.tail();
        const used = (head - tail) >>> 0;
        const pos = head & (this.capacity - 1);
        if (pos + framed <= this.capacity) {
          if (this.capacity - used >= framed) {
            this.pending = {
              headSnapshot: head,
              pos,
              framed,
              payload: this.data.subarray(pos + 4, pos + 4 + n),
            };
            return this.pending.payload;
          }
        } else {
          // SKIP: advance past the end once the consumer drained up to `pos`.
          // The marker is visible to the consumer through the head advance.
          if (used <= pos && pos - used >= framed) {
            Atomics.store(this.view, DATA_WORD + pos / 4, -1); // u32 SKIP_MARKER
            Atomics.store(this.view, HEAD, head + (this.capacity - pos));
            this.wakeConsumer();
            continue; // re-loop: now at offset 0
          }
        }
        if (nonBlocking || (opts?.timeoutMs !== undefined && nowMs() >= deadline)) {
          throw new MembridgeError('E_TIMEOUT', 'ring full: reserve timed out', {
            segmentName: this.name,
          });
        }
        // Park on TAIL: announce, re-check, then wait (§8.2 + review P3).
        Atomics.store(this.view, PRODUCER_PARKED, 1);
        parked = true;
        const tailNow = this.tail();
        if (tailNow !== tail) continue;  // space appeared while announcing
        this.b.syncWait(this.view, TAIL, tailNow, sliceOf(deadline));
        if (opts?.timeoutMs !== undefined && nowMs() >= deadline) {
          throw new MembridgeError('E_TIMEOUT', 'ring full: reserve timed out', {
            segmentName: this.name,
          });
        }
      }
    } finally {
      if (parked) Atomics.store(this.view, PRODUCER_PARKED, 0);
    }
  }

  /** Publish the reserved message: length field lands first, then the
   * release store on head + notify (§8.2). */
  commit(): void {
    if (this.pending === null) {
      throw new MembridgeError('E_RING_STATE', 'no reserve to commit', { segmentName: this.name });
    }
    const { headSnapshot, pos, framed, payload } = this.pending;
    this.pending = null;
    Atomics.store(this.view, DATA_WORD + pos / 4, payload.length);
    Atomics.store(this.view, HEAD, headSnapshot + framed);
    this.wakeConsumer();
  }

  private wakeConsumer(): void {
    if (Atomics.load(this.view, CONSUMER_PARKED) === 1) {
      this.b.syncNotify(this.view, HEAD, 1);
    }
  }

  private wakeProducer(): void {
    if (Atomics.load(this.view, PRODUCER_PARKED) === 1) {
      this.b.syncNotify(this.view, TAIL, 1);
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
    const deadline = opts?.timeoutMs !== undefined ? nowMs() + opts.timeoutMs : Infinity;
    for (;;) {
      try {
        return this.reserve(n, { timeoutMs: 0 });  // non-blocking attempt
      } catch (e) {
        if (!(e instanceof MembridgeError) || e.code !== 'E_TIMEOUT') throw e;
      }
      if (opts?.timeoutMs !== undefined && nowMs() >= deadline) {
        throw new MembridgeError('E_TIMEOUT', 'ring full: reserveAsync timed out', {
          segmentName: this.name,
        });
      }
      const tail = this.tail();
      await waitAsync(this.view, TAIL, tail, sliceOf(deadline));
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
  private claimed = false;
  private pending: Peeked | null = null;

  private constructor(name: string, view: Int32Array, capacity: number) {
    this.name = name;
    this.view = view;
    this.data = new Uint8Array(view.buffer, RING_HEADER_BYTES, capacity);
    this.capacity = capacity;
    this.b = nativeOrThrow();
  }

  /** Join as the consumer (SPSC: one live consumer per ring). */
  static open(name: string, opts?: { winGlobal?: boolean }): RingConsumer {
    const b = nativeOrThrow();
    const sab = open(name, { kind: 'ring', winGlobal: opts?.winGlobal });
    const capacity = sab.byteLength - RING_HEADER_BYTES;
    if (capacity < MIN_CAPACITY || (capacity & (capacity - 1)) !== 0) {
      throw new MembridgeError('E_INCOMPATIBLE', 'segment is not a membridge ring', {
        segmentName: name,
      });
    }
    const view = new Int32Array(sab);
    b.ringClaimRole(name, view, false);
    const c = new RingConsumer(name, view, capacity);
    c.claimed = true;
    return c;
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
    if (this.pending === null) {
      throw new MembridgeError('E_RING_STATE', 'no peek to release', { segmentName: this.name });
    }
    const { framed, tailSnapshot } = this.pending;
    this.pending = null;
    Atomics.compareExchange(this.view, TAIL, tailSnapshot, tailSnapshot + framed);
    this.wakeProducer();
  }

  private wakeProducer(): void {
    if (Atomics.load(this.view, PRODUCER_PARKED) === 1) {
      this.b.syncNotify(this.view, TAIL, 1);
    }
  }

  /** Async variant of {@link peek} (§8.2). Resolves null on timeout. */
  async peekAsync(opts?: ReserveOptions): Promise<Uint8Array | null> {
    const deadline = opts?.timeoutMs !== undefined ? nowMs() + opts.timeoutMs : Infinity;
    for (;;) {
      const msg = this.peek({ timeoutMs: 0 });
      if (msg !== null) return msg;
      if (opts?.timeoutMs !== undefined && nowMs() >= deadline) return null;
      const head = this.head();
      await waitAsync(this.view, HEAD, head, sliceOf(deadline));
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

