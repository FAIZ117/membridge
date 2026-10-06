import type { OpenOptions } from './core';
export declare function fallbackEnabled(opts?: OpenOptions): boolean;
export declare function fallbackOpen(name: string, size: number | undefined, opts: OpenOptions): SharedArrayBuffer;
export declare function fallbackUnlink(name: string): void;
