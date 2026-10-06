// fallback.ts — opt-in in-process fallback (§5.6). It never "silently works":
// without MEMBRIDGE_ALLOW_FALLBACK=1 or allowFallback, a missing addon is
// E_NATIVE_UNAVAILABLE. The fallback maps names to JS SharedArrayBuffers in
// THIS PROCESS ONLY — a cross-process bug if misused as if it were shared
// memory. It still checks sizes (shmbuf ignored size on re-open). Mutex and
// RingBuffer always require the native addon (E_NATIVE_UNAVAILABLE): the
// fallback serves plain data segments only.
//
// Scope (review F36): the name table is per ISOLATE — each worker_threads
// isolate loads a fresh copy of this module, and a plain JS Map cannot be
// shared across realms (globalThis is per-worker; only a SharedArrayBuffer
// could carry the table, and a serialized table over Atomics would be
// re-implementing membridge in JS). Cross-isolate sharing is exactly what the
// native addon provides; the fallback is a single-isolate convenience and
// says so.

import { MembridgeError } from './errors';
import type { OpenOptions } from './core';

interface FallbackEntry {
  sab: SharedArrayBuffer;
  dataBytes: number;
}

const segments = new Map<string, FallbackEntry>();  // per isolate (see scope note)

export function fallbackEnabled(opts?: OpenOptions): boolean {
  if (opts?.allowFallback === true) return true;
  return process.env.MEMBRIDGE_ALLOW_FALLBACK === '1';
}

export function fallbackOpen(name: string, size: number | undefined, opts: OpenOptions): SharedArrayBuffer {
  const existing = segments.get(name);
  if (opts.mode === 'create' && existing !== undefined) {
    throw new MembridgeError('E_EXISTS', `segment '${name}' already exists`, { segmentName: name });
  }
  if (existing === undefined) {
    if (size === undefined || opts.mode === 'join') {
      // join on a missing name never creates (native parity, review F36)
      throw new MembridgeError('E_NOT_FOUND', `segment '${name}' does not exist`, { segmentName: name });
    }
    const sab = new SharedArrayBuffer(size);
    segments.set(name, { sab, dataBytes: size });
    return sab;
  }
  if (size === undefined) {
    return existing.sab;
  }
  switch (opts.sizePolicy ?? 'exact') {
    case 'exact':
      if (size !== existing.dataBytes) {
        throw new MembridgeError('E_SIZE_MISMATCH', `requested ${size} vs existing ${existing.dataBytes}`, {
          segmentName: name,
          requested: size,
          existing: existing.dataBytes,
        });
      }
      return existing.sab;
    case 'at-least':
      if (size > existing.dataBytes) {
        throw new MembridgeError('E_SIZE_MISMATCH', `requested ${size} vs existing ${existing.dataBytes}`, {
          segmentName: name,
          requested: size,
          existing: existing.dataBytes,
        });
      }
      // A SAB cannot be a prefix view (F15) — the fallback hands back the
      // full buffer; it is single-process anyway.
      return existing.sab;
    case 'grow':
      throw new MembridgeError('E_GROW_UNSUPPORTED', 'grow is unsupported in the fallback', {
        segmentName: name,
      });
  }
}

export function fallbackUnlink(name: string): void {
  if (!segments.delete(name)) {
    throw new MembridgeError('E_NOT_FOUND', `segment '${name}' does not exist`, { segmentName: name });
  }
}
