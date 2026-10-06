// core.ts — open/close/unlink/isNative (§5.1). Validation happens here AND in
// the native layer (defense in depth); the size is a JS number handled as a
// double end to end, never Uint32Value (F5).

import { MembridgeError } from './errors';
import { isMembridgeErrorCode } from './errors';
import { native, nativeOrThrow, nativeErrorCode } from './native';
import { fallbackEnabled, fallbackOpen, fallbackUnlink } from './fallback';

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

export const DEFAULT_MAX_SEGMENT_BYTES = 256 * 1024 * 1024;

function maxSegmentBytes(): number {
  const raw = process.env.MEMBRIDGE_MAX_SEGMENT_BYTES;
  if (raw === undefined || raw === '') return DEFAULT_MAX_SEGMENT_BYTES;
  const v = Number(raw);
  if (!Number.isSafeInteger(v) || v <= 0) return DEFAULT_MAX_SEGMENT_BYTES;
  return v;
}

export function validateSize(name: string, size: number | undefined): void {
  if (size === undefined) return;
  if (!Number.isSafeInteger(size) || size < 1 || size > 4 * 1024 * 1024 * 1024) {
    throw new MembridgeError('E_SIZE_INVALID', `size must be a safe integer >= 1 (got ${size})`, {
      segmentName: name,
      requested: size,
    });
  }
  const cap = maxSegmentBytes();
  if (size > cap) {
    throw new MembridgeError(
      'E_SIZE_INVALID',
      `size ${size} exceeds MEMBRIDGE_MAX_SEGMENT_BYTES (${cap})`,
      { segmentName: name, requested: size },
    );
  }
}

// §5.5 — mirrors cc/segment.cc ValidateName.
export function validateName(name: string): void {
  const platform = process.platform;
  const maxLen = platform === 'darwin' ? 31 : 250;
  if (
    typeof name !== 'string' ||
    name.length < 2 ||
    !name.startsWith('/') ||
    name.indexOf('/', 1) !== -1 ||
    Buffer.byteLength(name, 'utf8') > maxLen
  ) {
    throw new MembridgeError(
      'E_NAME_INVALID',
      `segment name must start with '/', contain no other '/', and be <= ${maxLen} bytes on ${platform}`,
      { segmentName: name },
    );
  }
}

function toNativeOpts(opts: OpenOptions): Record<string, unknown> {
  const o: Record<string, unknown> = {};
  if (opts.mode !== undefined) o.mode = opts.mode;
  if (opts.sizePolicy !== undefined) o.sizePolicy = opts.sizePolicy;
  if (opts.reserve !== undefined) o.reserve = opts.reserve;
  if (opts.permissions !== undefined) o.permissions = opts.permissions;
  if (opts.initTimeoutMs !== undefined) o.initTimeoutMs = opts.initTimeoutMs;
  if (opts.raw !== undefined) o.raw = opts.raw;
  if (opts.winGlobal !== undefined) o.winGlobal = opts.winGlobal;
  if (opts.kind !== undefined) {
    o.kind = opts.kind === 'mutex' ? 1 : opts.kind === 'ring' ? 2 : 0;
  }
  return o;
}

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
export function open(name: string, size: number, opts?: OpenOptions): SharedArrayBuffer;
export function open(name: string, opts?: OpenOptions & { size?: undefined }): SharedArrayBuffer;
export function open(
  name: string,
  sizeOrOpts?: number | OpenOptions,
  maybeOpts?: OpenOptions,
): SharedArrayBuffer {
  let size: number | undefined;
  let opts: OpenOptions;
  if (typeof sizeOrOpts === 'number') {
    size = sizeOrOpts;
    opts = maybeOpts ?? {};
  } else {
    opts = (sizeOrOpts ?? {}) as OpenOptions;
  }

  validateName(name);
  validateSize(name, size);

  const nativeBinding = native();
  if (nativeBinding !== undefined) {
    return nativeBinding.open(name, size, toNativeOpts(opts));
  }
  if (fallbackEnabled(opts)) {
    return fallbackOpen(name, size, opts);
  }
  return nativeOrThrow().open(name, size, toNativeOpts(opts));
}

/**
 * Compatibility no-op (§5.4): it cannot unmap memory that live SABs still
 * reference, and the registry holds only weak references, so there is no
 * per-isolate state to release. shmbuf call sites keep working. Memory is
 * released when the last SAB is GC'd; the name is removed by {@link unlink}.
 * Invalid names still throw E_NAME_INVALID.
 */
export function close(name: string): void {
  validateName(name);
  const nativeBinding = native();
  if (nativeBinding !== undefined) {
    nativeBinding.close(name);
    return;
  }
  // fallback: nothing to release (no-op, like the native side)
}

/**
 * Remove the segment name (POSIX shm_unlink). On Windows the section
 * disappears with the last handle; unlink there only prevents new joins via
 * membridge (marks the header UNLINKED). Throws E_NOT_FOUND when missing.
 */
export function unlink(name: string): void {
  validateName(name);
  const nativeBinding = native();
  if (nativeBinding !== undefined) {
    nativeBinding.unlink(name);
    return;
  }
  fallbackUnlink(name);
}

/** True when the native addon is loaded in this process. */
export function isNative(): boolean {
  return native() !== undefined;
}

/** Undocumented test hook: is a live mapping cached in the process registry? */
export function debugRegistryHas(name: string): boolean {
  return nativeOrThrow().debugRegistryHas(name);
}
