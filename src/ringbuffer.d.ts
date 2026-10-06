import { type OpenOptions } from './core';
export declare const RING_HEADER_BYTES = 256;
export declare const SKIP_MARKER = 4294967295;
export declare const MIN_CAPACITY = 4096;
export declare const MAX_CAPACITY: number;
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
export declare class RingProducer {
    readonly name: string;
    private readonly view;
    private readonly data;
    private readonly capacity;
    private readonly maxMessage;
    private readonly b;
    private claimed;
    private pending;
    private constructor();
    /** Create or join as the producer (SPSC: one live producer per ring). */
    static open(name: string, opts?: ProducerOptions & OpenOptions): RingProducer;
    private head;
    private tail;
    /** Two-phase write: returns a Uint8Array view straight into shared memory.
     * Call {@link commit} to publish. Throws E_RING_STATE if already reserved. */
    reserve(n: number, opts?: ReserveOptions): Uint8Array;
    /** Publish the reserved message: length field lands first, then the
     * release store on head + notify (§8.2). */
    commit(): void;
    /** Convenience: reserve + copy + commit (one copy). */
    write(bytes: Uint8Array, opts?: ReserveOptions): void;
    get capacityBytes(): number;
    get maxMessageBytes(): number;
}
export declare class RingConsumer {
    readonly name: string;
    private readonly view;
    private readonly data;
    private readonly capacity;
    private readonly b;
    private claimed;
    private pending;
    private constructor();
    /** Join as the consumer (SPSC: one live consumer per ring). */
    static open(name: string, opts?: {
        winGlobal?: boolean;
    }): RingConsumer;
    private head;
    private tail;
    /** Two-phase read: view into shared memory, valid until {@link release}.
     * Returns null on timeout. Throws E_RING_STATE if a peek is already open. */
    peek(opts?: ReserveOptions): Uint8Array | null;
    /** Only now does tail advance (§8.2). */
    release(): void;
    /** Convenience: peek + copy + release. Returns null on timeout. */
    read(opts?: ReserveOptions): Uint8Array | null;
    get capacityBytes(): number;
}
