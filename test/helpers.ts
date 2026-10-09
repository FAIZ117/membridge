// helpers.ts — machinery every suite shares (§13): the segment-name generator
// (AGENTS.md guardrail 3), a cleanup harness that unlinks in `finally` even
// when a test fails mid-way, a per-OS kill helper, the byte-level §5.2 header
// builder/parser (outside cross-check of cc/header.h), and assertThrowsCode.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import { spawnSync } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { unlink } from '../src/core';
import { ShmBridgeError } from '../src/errors';
import type { TestContext } from 'node:test';

let nameCounter = 0;

/**
 * /shm-bridge-test-<unique> — caller (or makeTrackedName) unlinks it.
 * darwin caps shm names at 31 bytes (PSHMNAMLEN, fact U2), so the suffix is
 * compressed there; the /shm-bridge-test- prefix rule (AGENTS.md hygiene) and
 * cross-process uniqueness (pid + counter + random) hold on every platform.
 */
export function uniqueName(): string {
  nameCounter++;
  if (process.platform === 'darwin') {
    // 17 (prefix) + pid base36 (~5) + '-' + counter base36 (1-2) + 6 random
    // chars = 30-31. A longer counter only loses trailing random chars to the
    // slice; the counter itself (the uniqueness within a process) survives.
    const pid = process.pid.toString(36);
    const ctr = nameCounter.toString(36);
    const rnd = randomBytes(6).toString('base64url').slice(0, 6);
    return `/shm-bridge-test-${pid}-${ctr}${rnd}`.slice(0, 31);
  }
  return `/shm-bridge-test-${process.pid}-${Date.now().toString(36)}-${nameCounter}-${randomBytes(4).toString('hex')}`;
}

/** Name plus an auto-cleanup hook on the test context (runs on failure too). */
export function makeTrackedName(t: TestContext): string {
  const name = uniqueName();
  t.after(() => unlinkQuietly(name));
  return name;
}

/**
 * Keep `value` (a SAB, Mutex, or ring handle) reachable until the test ends.
 * A segment is mapped only while something references it: on Windows the
 * named section dies with its last handle (PLAN §5.4), and on every OS this
 * process's attach row goes with the GC'd SAB. A creator whose open() result
 * is discarded can be collected before a child or worker joins — the joiner
 * then sees E_NOT_FOUND, or a fresh zeroed object. Hold every handle a later
 * joiner, or an assertion about our attach row, depends on.
 */
export function holdForTest<T>(t: TestContext, value: T): T {
  t.after(() => {
    void value; // the hook's closure is the reference
  });
  return value;
}

export function unlinkQuietly(name: string): void {
  try {
    unlink(name);
  } catch {
    // already gone
  }
}

/** Error-code assert (§13): assertThrowsCode(fn, 'E_SIZE_MISMATCH'). */
export function assertThrowsCode(
  fn: () => unknown,
  code: string,
  fieldChecks?: (err: ShmBridgeError) => void,
): void {
  assert.throws(fn, (err: unknown) => {
    assert.ok(err instanceof ShmBridgeError, `expected ShmBridgeError, got ${String(err)}`);
    const e = err as ShmBridgeError;
    assert.strictEqual(e.code, code, `expected ${code}, got ${e.code} (${e.message})`);
    if (fieldChecks !== undefined) fieldChecks(e);
    return true;
  });
}

/** Per-OS hard kill: POSIX SIGKILL; Windows taskkill /F. */
export function killHard(pid: number): void {
  if (process.platform === 'win32') {
    spawnSync('taskkill', ['/F', '/T', '/PID', String(pid)]);
    return;
  }
  try {
    process.kill(pid, 'SIGKILL');
  } catch {
    // already dead
  }
}

// ---- §5.2 header byte contract (mirrors cc/header.h) ------------------------

export const HEADER_MAGIC = 0x424d4853; // "SHMB" little-endian
export const LAYOUT_VERSION = 1;
export const MIN_HEADER_BYTES = 4096;

export interface AttachSlotView {
  pid: number;
  threadId: number;
  startTime: number;
  pidNsInode: number;
  refcount: number;
}

export interface HeaderView {
  magic: number;
  layoutVersion: number;
  initState: number;
  initializerSlot: number;
  headerBytes: number;
  flags: number;
  dataBytes: number;
  attachSlots: AttachSlotView[];
}

export function buildHeader(h: Partial<HeaderView>, totalBytes = MIN_HEADER_BYTES): Buffer {
  const buf = Buffer.alloc(totalBytes);
  buf.writeUInt32LE(h.magic ?? HEADER_MAGIC, 0);
  buf.writeUInt32LE(h.layoutVersion ?? LAYOUT_VERSION, 4);
  buf.writeInt32LE(h.initState ?? 0, 8);
  buf.writeInt32LE(h.initializerSlot ?? -1, 12);
  buf.writeUInt32LE(h.headerBytes ?? totalBytes, 16);
  buf.writeUInt32LE(h.flags ?? 0, 20);
  buf.writeBigUInt64LE(BigInt(h.dataBytes ?? 0), 24);
  const slots = h.attachSlots ?? [];
  for (let i = 0; i < slots.length; i++) {
    const s = slots[i]!;
    const base = 32 + i * 32;
    buf.writeInt32LE(s.pid, base);
    buf.writeInt32LE(s.threadId, base + 4);
    buf.writeBigInt64LE(BigInt(s.startTime), base + 8);
    buf.writeBigInt64LE(BigInt(s.pidNsInode), base + 16);
    buf.writeInt32LE(s.refcount, base + 24);
    buf.writeInt32LE(0, base + 28);
  }
  return buf;
}

export function parseHeader(bytes: Buffer): HeaderView {
  const out: HeaderView = {
    magic: bytes.readUInt32LE(0),
    layoutVersion: bytes.readUInt32LE(4),
    initState: bytes.readInt32LE(8),
    initializerSlot: bytes.readInt32LE(12),
    headerBytes: bytes.readUInt32LE(16),
    flags: bytes.readUInt32LE(20),
    dataBytes: Number(bytes.readBigUInt64LE(24)),
    attachSlots: [],
  };
  const n = Math.floor((out.headerBytes - 32) / 32);
  for (let i = 0; i < n; i++) {
    const base = 32 + i * 32;
    out.attachSlots.push({
      pid: bytes.readInt32LE(base),
      threadId: bytes.readInt32LE(base + 4),
      startTime: Number(bytes.readBigInt64LE(base + 8)),
      pidNsInode: Number(bytes.readBigInt64LE(base + 16)),
      refcount: bytes.readInt32LE(base + 24),
    });
  }
  return out;
}

/** POSIX /dev/shm path for a segment name (tests read headers to craft
 * crash states); undefined off-POSIX. */
export function shmPath(name: string): string | undefined {
  if (process.platform === 'linux') return `/dev/shm${name}`;
  if (process.platform === 'darwin') return `/private/tmp${name}`; // shm objects are not listed on macOS
  return undefined;
}

/** Linux: /proc/<pid>/stat field 22 (start time) — for identity crafting. */
export function readStartTime(pid: number): number {
  if (process.platform !== 'linux') return -1;
  try {
    const stat = fs.readFileSync(`/proc/${pid}/stat`, 'utf8');
    const after = stat.slice(stat.lastIndexOf(')') + 1).trim();
    // After ')' fields continue at 3 (state); starttime is field 22.
    const fields = after.split(/\s+/);
    return Number(fields[22 - 3]);
  } catch {
    return -1;
  }
}

/** Collect garbage `times` times (no-op without --expose-gc). */
export function runGc(times = 6): void {
  const gc = (globalThis as { gc?: () => void }).gc;
  if (gc === undefined) return;
  for (let i = 0; i < times; i++) gc();
}

export { assert };
