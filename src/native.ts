// native.ts — loads the plain-V8 addon via node-gyp-build (PLAN §12): the
// per-ABI prebuild first, then build/Release, then the source-build fallback
// (node-gyp runs from the package root, which is why binding.gyp lives there).

import path from 'node:path';
import { MembridgeError } from './errors';
import type { MembridgeErrorCode } from './errors';

/* eslint-disable @typescript-eslint/no-explicit-any */
type NativeBinding = any;

let binding: NativeBinding | undefined;
let loadAttempted = false;

/** Raw native surface; shaped like the C++ exports (cc/addon.cc). */
export interface Native {
  open(name: string, size: number | undefined, opts: Record<string, unknown> | undefined): SharedArrayBuffer;
  unlink(name: string): void;
  close(name: string): void;
  isNative(): boolean;
  // §6 sync primitive (-1 = no timeout on the wait calls)
  syncWait(view: Int32Array, index: number, expected: number, timeoutMs: number): string;
  syncNotify(view: Int32Array, index: number, count: number): number;
  syncWaitAsync(view: Int32Array, index: number, expected: number, timeoutMs: number): Promise<string>;
  // §7 mutex support (CAS protocol in JS; native does liveness + teardown)
  mutexClaimSlot(name: string, view: Int32Array): { slot: number; gen: number; token: number };
  mutexOwnerAlive(view: Int32Array, token: number): boolean;
  mutexRegisterPin(name: string, view: Int32Array, slot: number, token: number, sab: SharedArrayBuffer): void;
  mutexUnregisterClaim(view: Int32Array, slot: number): void;
  // §8 ring role claims (producer slot 0 / consumer slot 1)
  ringClaimRole(name: string, view: Int32Array, isProducer: boolean): number;
  // §7.1 liveness for §9 stat
  checkLiveness(pid: number, startTime: number, pidNsInode: number): 'alive' | 'dead' | 'unknown';
  // §9 stat support (header read without mapping the data region)
  readHeader(name: string, maxAttach?: number): {
    magic: number;
    layoutVersion: number;
    initState: number;
    headerBytes: number;
    flags: number;
    dataBytes: number;
    attach: Array<{ pid: number; threadId: number; startTime: number; pidNsInode: number; refcount: number }>;
  };
  // Undocumented test/debug hooks (not re-exported from index.ts).
  debugRegistryHas(name: string): boolean;
  selfIdentity(): { pid: number; threadId: number; startTime: number; pidNsInode: number };
}

function packageRoot(): string {
  // The compiled layout varies (flat dist/ for publish, dist/src/ for the
  // test build), so walk up to the directory that actually owns the addon
  // artifacts (prebuilds/ or build/) instead of counting directory levels.
  let dir = __dirname;
  for (let i = 0; i < 5; i++) {
    const fs = require('node:fs');
    if (
      fs.existsSync(path.join(dir, 'package.json')) &&
      (fs.existsSync(path.join(dir, 'prebuilds')) ||
        fs.existsSync(path.join(dir, 'build')) ||
        fs.existsSync(path.join(dir, 'binding.gyp')))
    ) {
      return dir;
    }
    const parent = path.dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }
  return path.join(__dirname, '..');
}

function load(): NativeBinding | undefined {
  if (loadAttempted) return binding;
  loadAttempted = true;
  try {
    const loadAddon = require('node-gyp-build');
    binding = loadAddon(packageRoot()) as NativeBinding;
    if (binding && typeof binding.setMembridgeErrorCtor === 'function') {
      binding.setMembridgeErrorCtor(MembridgeError);
    }
  } catch {
    binding = undefined; // fallback path handles this (§5.6)
  }
  return binding;
}

export function native(): Native | undefined {
  const b = load();
  if (!b) return undefined;
  return b as Native;
}

/** True when the native addon is available in this process. */
export function isNative(): boolean {
  return load() !== undefined;
}

/** The native binding; throws E_NATIVE_UNAVAILABLE when absent. */
export function nativeOrThrow(): Native {
  const b = load();
  if (!b) {
    throw new MembridgeError(
      'E_NATIVE_UNAVAILABLE',
      'the membridge native addon is not available; set MEMBRIDGE_ALLOW_FALLBACK=1 ' +
        'or pass allowFallback to opt into the in-process fallback',
    );
  }
  return b as Native;
}

export function nativeErrorCode(err: unknown): MembridgeErrorCode | undefined {
  if (err !== null && typeof err === 'object' && typeof (err as { code?: unknown }).code === 'string') {
    return (err as { code: string }).code as MembridgeErrorCode;
  }
  return undefined;
}
