import type { MembridgeErrorCode } from './errors';
/** Raw native surface; shaped like the C++ exports (cc/addon.cc). */
export interface Native {
    open(name: string, size: number | undefined, opts: Record<string, unknown> | undefined): SharedArrayBuffer;
    unlink(name: string): void;
    close(name: string): void;
    isNative(): boolean;
    syncWait(view: Int32Array, index: number, expected: number, timeoutMs: number): string;
    syncNotify(view: Int32Array, index: number, count: number): number;
    syncWaitAsync(view: Int32Array, index: number, expected: number, timeoutMs: number): Promise<string>;
    mutexClaimSlot(name: string, view: Int32Array): {
        slot: number;
        gen: number;
        token: number;
    };
    mutexOwnerAlive(view: Int32Array, token: number): boolean;
    mutexTrackHeld(name: string, view: Int32Array, token: number): void;
    mutexUntrackHeld(view: Int32Array, token: number): void;
    ringClaimRole(name: string, view: Int32Array, isProducer: boolean): number;
    readHeader(name: string, maxAttach?: number): {
        magic: number;
        layoutVersion: number;
        initState: number;
        headerBytes: number;
        flags: number;
        dataBytes: number;
        attach: Array<{
            pid: number;
            threadId: number;
            startTime: number;
            pidNsInode: number;
            refcount: number;
        }>;
    };
    debugRegistryHas(name: string): boolean;
    selfIdentity(): {
        pid: number;
        threadId: number;
        startTime: number;
        pidNsInode: number;
    };
}
export declare function native(): Native | undefined;
/** True when the native addon is available in this process. */
export declare function isNative(): boolean;
/** The native binding; throws E_NATIVE_UNAVAILABLE when absent. */
export declare function nativeOrThrow(): Native;
export declare function nativeErrorCode(err: unknown): MembridgeErrorCode | undefined;
