export type OpenMode = 'create-or-join' | 'create' | 'join';
export type SizePolicy = 'exact' | 'at-least' | 'grow';
export interface OpenOptions {
    mode?: OpenMode;
    sizePolicy?: SizePolicy;
    /** Linux: fallocate the data region at create. Default true. */
    reserve?: boolean;
    /** POSIX mode bits at create. Default 0o600. */
    permissions?: number;
    /** How long a joiner waits for the creator to finish. Default 5000. */
    initTimeoutMs?: number;
    /** No header: foreign-process interop, loses all header features. Default false. */
    raw?: boolean;
    /** Windows: use the Global\ prefix (needs SeCreateGlobalPrivilege). Default Local\. */
    winGlobal?: boolean;
    /** Header kind marker (§5.2): 'plain' (default), 'mutex', or 'ring'. Joining a
     * segment whose kind differs from a non-plain request throws E_INCOMPATIBLE. */
    kind?: 'plain' | 'mutex' | 'ring';
    /** Opt into the in-process fallback when the native addon is missing. */
    allowFallback?: boolean;
    /** The last process to detach unlinks the name (§9). */
    unlinkWhenUnused?: boolean;
}
export declare const DEFAULT_MAX_SEGMENT_BYTES: number;
export declare function validateSize(name: string, size: number | undefined): void;
export declare function validateName(name: string): void;
/**
 * Open (create or join) a shared-memory segment. Returns a
 * {@link SharedArrayBuffer} covering the data region only — the header page
 * (§5.2) is native-only memory.
 *
 * - `open(name, size, opts?)` — create-or-join at `size` (default policy
 *   `exact`: a joiner whose size differs from the segment throws
 *   E_SIZE_MISMATCH).
 * - `open(name, opts?)` — join at the existing size (throws E_NOT_FOUND if
 *   the segment does not exist; mode `create` without a size throws
 *   E_SIZE_INVALID).
 */
export declare function open(name: string, size: number, opts?: OpenOptions): SharedArrayBuffer;
export declare function open(name: string, opts?: OpenOptions & {
    size?: undefined;
}): SharedArrayBuffer;
/**
 * Compatibility no-op (§5.4): it cannot unmap memory that live SABs still
 * reference, and the registry holds only weak references, so there is no
 * per-isolate state to release. shmbuf call sites keep working. Memory is
 * released when the last SAB is GC'd; the name is removed by {@link unlink}.
 * Invalid names still throw E_NAME_INVALID.
 */
export declare function close(name: string): void;
/**
 * Remove the segment name (POSIX shm_unlink). On Windows the section
 * disappears with the last handle; unlink there only prevents new joins via
 * membridge (marks the header UNLINKED). Throws E_NOT_FOUND when missing.
 */
export declare function unlink(name: string): void;
/** True when the native addon is loaded in this process. */
export declare function isNative(): boolean;
/** Undocumented test hook: is a live mapping cached in the process registry? */
export declare function debugRegistryHas(name: string): boolean;
