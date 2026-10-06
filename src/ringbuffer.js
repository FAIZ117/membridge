"use strict";
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
Object.defineProperty(exports, "__esModule", { value: true });
exports.RingConsumer = exports.RingProducer = exports.MAX_CAPACITY = exports.MIN_CAPACITY = exports.SKIP_MARKER = exports.RING_HEADER_BYTES = void 0;
const errors_1 = require("./errors");
const native_1 = require("./native");
const core_1 = require("./core");
exports.RING_HEADER_BYTES = 256;
exports.SKIP_MARKER = 0xffffffff;
exports.MIN_CAPACITY = 4096;
exports.MAX_CAPACITY = 1024 * 1024 * 1024;
// word offsets
const HEAD = 0;
const TAIL = 16;
const PRODUCER_TOKEN = 32;
const CONSUMER_TOKEN = 33;
const CAPACITY = 34;
const MAX_MESSAGE = 35;
const DATA_WORD = exports.RING_HEADER_BYTES / 4; // 64
const sleepSliceMs = 250;
function align8(n) {
    return (n + 7) & ~7;
}
function assertCapacity(name, capacity, maxMessage) {
    if (!Number.isSafeInteger(capacity) || capacity < exports.MIN_CAPACITY || capacity > exports.MAX_CAPACITY ||
        (capacity & (capacity - 1)) !== 0) {
        throw new errors_1.MembridgeError('E_SIZE_INVALID', `capacity must be a power of two between ${exports.MIN_CAPACITY} and ${exports.MAX_CAPACITY}`, { segmentName: name, requested: capacity });
    }
    const cap = maxMessage ?? Math.floor(capacity / 2) - 8;
    if (!Number.isSafeInteger(cap) || cap < 1 || cap > Math.floor(capacity / 2) - 8) {
        throw new errors_1.MembridgeError('E_SIZE_INVALID', `maxMessage must be <= capacity/2 - 8 (= ${Math.floor(capacity / 2) - 8})`, { segmentName: name, requested: cap });
    }
    return cap;
}
class RingProducer {
    name;
    view;
    data;
    capacity;
    maxMessage;
    b;
    claimed = false;
    pending = null;
    constructor(name, view, capacity, maxMessage) {
        this.name = name;
        this.view = view;
        this.data = new Uint8Array(view.buffer, exports.RING_HEADER_BYTES, capacity);
        this.capacity = capacity;
        this.maxMessage = maxMessage;
        this.b = (0, native_1.nativeOrThrow)();
    }
    /** Create or join as the producer (SPSC: one live producer per ring). */
    static open(name, opts) {
        const b = (0, native_1.nativeOrThrow)();
        let capacity;
        let maxMessage;
        if (opts?.capacity !== undefined) {
            capacity = opts.capacity;
            maxMessage = assertCapacity(name, capacity, opts.maxMessage);
            if (capacity > exports.MAX_CAPACITY) {
                throw new errors_1.MembridgeError('E_SIZE_INVALID', 'capacity too large', { segmentName: name });
            }
        }
        const sab = capacity !== undefined
            ? (0, core_1.open)(name, exports.RING_HEADER_BYTES + capacity, { kind: 'ring', winGlobal: opts?.winGlobal })
            : (0, core_1.open)(name, { kind: 'ring', winGlobal: opts?.winGlobal });
        const view = new Int32Array(sab);
        const cap = capacity ?? sab.byteLength - exports.RING_HEADER_BYTES;
        const mm = maxMessage ?? Math.floor(cap / 2) - 8;
        b.ringClaimRole(name, view, true);
        const p = new RingProducer(name, view, cap, mm);
        p.claimed = true;
        if (capacity !== undefined && Atomics.load(view, CAPACITY) === 0) {
            Atomics.store(view, CAPACITY, cap);
            Atomics.store(view, MAX_MESSAGE, mm);
        }
        return p;
    }
    head() {
        return Atomics.load(this.view, HEAD);
    }
    tail() {
        return Atomics.load(this.view, TAIL);
    }
    /** Two-phase write: returns a Uint8Array view straight into shared memory.
     * Call {@link commit} to publish. Throws E_RING_STATE if already reserved. */
    reserve(n, opts) {
        if (this.pending !== null) {
            throw new errors_1.MembridgeError('E_RING_STATE', 'previous reserve not committed', {
                segmentName: this.name,
            });
        }
        if (!Number.isSafeInteger(n) || n < 0) {
            throw new errors_1.MembridgeError('E_SIZE_INVALID', 'message size must be a safe integer >= 0', {
                segmentName: this.name,
            });
        }
        if (n > this.maxMessage) {
            throw new errors_1.MembridgeError('E_MESSAGE_TOO_LARGE', `message of ${n} bytes exceeds maxMessage of ${this.maxMessage}`, {
                segmentName: this.name,
                requested: n,
                existing: this.maxMessage,
            });
        }
        const framed = align8(4 + n);
        const deadline = opts?.timeoutMs !== undefined ? Date.now() + opts.timeoutMs : Infinity;
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
            }
            else {
                // SKIP: advance past the end once the consumer drained up to `pos`.
                // The marker is visible to the consumer through the head advance.
                if (used <= pos && pos - used >= framed) {
                    Atomics.store(this.view, DATA_WORD + pos / 4, -1); // u32 SKIP_MARKER
                    Atomics.store(this.view, HEAD, head + (this.capacity - pos));
                    this.b.syncNotify(this.view, HEAD, 1);
                    continue; // re-loop: now at offset 0
                }
            }
            if (opts?.timeoutMs !== undefined && Date.now() >= deadline) {
                throw new errors_1.MembridgeError('E_TIMEOUT', 'ring full: reserve timed out', {
                    segmentName: this.name,
                });
            }
            // wait for the consumer to release (§8.2: producer waits on tail)
            this.b.syncWait(this.view, TAIL, tail, sleepSliceMs);
            if (opts?.timeoutMs !== undefined && Date.now() >= deadline) {
                throw new errors_1.MembridgeError('E_TIMEOUT', 'ring full: reserve timed out', {
                    segmentName: this.name,
                });
            }
        }
    }
    /** Publish the reserved message: length field lands first, then the
     * release store on head + notify (§8.2). */
    commit() {
        if (this.pending === null) {
            throw new errors_1.MembridgeError('E_RING_STATE', 'no reserve to commit', { segmentName: this.name });
        }
        const { headSnapshot, pos, framed, payload } = this.pending;
        this.pending = null;
        Atomics.store(this.view, DATA_WORD + pos / 4, payload.length);
        Atomics.store(this.view, HEAD, headSnapshot + framed);
        this.b.syncNotify(this.view, HEAD, 1);
    }
    /** Convenience: reserve + copy + commit (one copy). */
    write(bytes, opts) {
        const view = this.reserve(bytes.length, opts);
        view.set(bytes);
        this.commit();
    }
    get capacityBytes() {
        return this.capacity;
    }
    get maxMessageBytes() {
        return this.maxMessage;
    }
}
exports.RingProducer = RingProducer;
class RingConsumer {
    name;
    view;
    data;
    capacity;
    b;
    claimed = false;
    pending = null;
    constructor(name, view, capacity) {
        this.name = name;
        this.view = view;
        this.data = new Uint8Array(view.buffer, exports.RING_HEADER_BYTES, capacity);
        this.capacity = capacity;
        this.b = (0, native_1.nativeOrThrow)();
    }
    /** Join as the consumer (SPSC: one live consumer per ring). */
    static open(name, opts) {
        const b = (0, native_1.nativeOrThrow)();
        const sab = (0, core_1.open)(name, { kind: 'ring', winGlobal: opts?.winGlobal });
        const capacity = sab.byteLength - exports.RING_HEADER_BYTES;
        if (capacity < exports.MIN_CAPACITY || (capacity & (capacity - 1)) !== 0) {
            throw new errors_1.MembridgeError('E_INCOMPATIBLE', 'segment is not a membridge ring', {
                segmentName: name,
            });
        }
        const view = new Int32Array(sab);
        b.ringClaimRole(name, view, false);
        const c = new RingConsumer(name, view, capacity);
        c.claimed = true;
        return c;
    }
    head() {
        return Atomics.load(this.view, HEAD);
    }
    tail() {
        return Atomics.load(this.view, TAIL);
    }
    /** Two-phase read: view into shared memory, valid until {@link release}.
     * Returns null on timeout. Throws E_RING_STATE if a peek is already open. */
    peek(opts) {
        if (this.pending !== null) {
            throw new errors_1.MembridgeError('E_RING_STATE', 'previous peek not released', {
                segmentName: this.name,
            });
        }
        const deadline = opts?.timeoutMs !== undefined ? Date.now() + opts.timeoutMs : Infinity;
        for (;;) {
            const head = this.head();
            const tail = this.tail();
            if (head === tail) {
                if (opts?.timeoutMs !== undefined && Date.now() >= deadline)
                    return null;
                this.b.syncWait(this.view, HEAD, head, sleepSliceMs);
                if (opts?.timeoutMs !== undefined && Date.now() >= deadline &&
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
                this.b.syncNotify(this.view, TAIL, 1);
                continue;
            }
            const framed = align8(4 + lenWord);
            if (head - tail < framed) {
                // length visible but payload not fully committed yet
                if (opts?.timeoutMs !== undefined && Date.now() >= deadline)
                    return null;
                this.b.syncWait(this.view, HEAD, head, sleepSliceMs);
                if (opts?.timeoutMs !== undefined && Date.now() >= deadline)
                    return null;
                continue;
            }
            this.pending = {
                framed,
                tailSnapshot: tail,
                payload: this.data.subarray(pos + 4, pos + 4 + lenWord),
            };
            return this.pending.payload;
        }
    }
    /** Only now does tail advance (§8.2). */
    release() {
        if (this.pending === null) {
            throw new errors_1.MembridgeError('E_RING_STATE', 'no peek to release', { segmentName: this.name });
        }
        const { framed, tailSnapshot } = this.pending;
        this.pending = null;
        Atomics.compareExchange(this.view, TAIL, tailSnapshot, tailSnapshot + framed);
        this.b.syncNotify(this.view, TAIL, 1);
    }
    /** Convenience: peek + copy + release. Returns null on timeout. */
    read(opts) {
        const view = this.peek(opts);
        if (view === null)
            return null;
        const copy = new Uint8Array(view.length);
        copy.set(view);
        this.release();
        return copy;
    }
    get capacityBytes() {
        return this.capacity;
    }
}
exports.RingConsumer = RingConsumer;
//# sourceMappingURL=ringbuffer.js.map