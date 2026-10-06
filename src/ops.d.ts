export interface SegmentStat {
    name: string;
    kind: 'plain' | 'mutex' | 'ring';
    dataBytes: number;
    headerBytes: number;
    attachSlots: Array<{
        pid: number;
        startTime: number;
        pidNsInode: number;
        alive: boolean | 'unknown';
        refcount: number;
    }>;
    attachOverflow: boolean;
    unlinked: boolean;
    initState: number;
}
export interface Capacity {
    totalBytes: number;
    freeBytes: number;
}
export interface ReapOptions {
    dryRun?: boolean;
    /** Reap a single segment (works on every platform, unlike the scan). */
    name?: string;
}
export interface ReapResult {
    name: string;
    attachers: number;
    reaped: boolean;
}
/** Free/total of the shm backing store. Linux only (§9). */
export declare function capacity(): Capacity;
/** Header contents of one segment: kind, sizes, attach table with liveness. */
export declare function stat(name: string): SegmentStat;
/** All membridge segments in /dev/shm (Linux only; magic-filtered, §9). */
export declare function list(): string[];
/**
 * Unlink segments whose every attach row is dead (or empty). Unknown
 * liveness counts as alive and blocks the reap (§9); a full attach table
 * (ATTACH_OVERFLOW) blocks it too — it could hide a live attacher.
 * `reap(name)` targets one segment on every platform; the scan form is
 * Linux-only (needs list()).
 */
export declare function reap(opts?: ReapOptions): ReapResult[];
