// ops.ts — §9 ops utilities with the per-platform matrix.
//   capacity()         free/total of the shm backing store (Linux only)
//   stat(name)         header contents: kind, sizes, attach table + liveness
//   list()             Linux: readdir /dev/shm filtered by membridge magic
//   reap({ dryRun })   unlink segments whose every attach row is dead
//   open(..., { unlinkWhenUnused: true })  the last detacher unlinks (§9)
//
// `list` filters by header magic, not a name prefix (§9): users choose names;
// the header is the reliable marker. Unknown liveness (foreign pid namespace)
// counts as alive and is never reaped.

import fs from 'node:fs';
import { MembridgeError } from './errors';
import { nativeOrThrow } from './native';
import { unlink, validateName } from './core';
import { nativeOrThrow as binding } from './native';

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

// Header constants — mirror cc/header.h.
const FLAG_ATTACH_OVERFLOW = 1;
const FLAG_UNLINKED = 2;
const KIND_MASK = 0xff << 8;
const MAGIC = 0x424d454d;

/** Native §7.1 liveness: 'alive' | 'dead' | 'unknown' (never-steal). */
function livenessOf(pid: number, startTime: number, pidNsInode: number): boolean | 'unknown' {
  const r = binding().checkLiveness(pid, startTime, pidNsInode);
  return r === 'alive' ? true : r === 'dead' ? false : 'unknown';
}

function kindOf(flags: number): 'plain' | 'mutex' | 'ring' {
  const k = flags & KIND_MASK;
  if (k === 1 << 8) return 'mutex';
  if (k === 2 << 8) return 'ring';
  return 'plain';
}

/** Free/total of the shm backing store. Linux only (§9). */
export function capacity(): Capacity {
  if (process.platform !== 'linux') {
    throw new MembridgeError(
      'E_UNSUPPORTED',
      `capacity() is Linux-only: ${process.platform} has no bounded shm filesystem`,
    );
  }
  const s = fs.statfsSync('/dev/shm');
  return {
    totalBytes: Number(s.blocks) * Number(s.bsize),
    freeBytes: Number(s.bavail) * Number(s.bsize),
  };
}

/** Header contents of one segment: kind, sizes, attach table with liveness. */
export function stat(name: string): SegmentStat {
  validateName(name);
  const b = nativeOrThrow();
  const h = b.readHeader(name);
  if (h.magic !== MAGIC) {
    throw new MembridgeError('E_INCOMPATIBLE', `'${name}' is not a membridge segment`, {
      segmentName: name,
    });
  }
  const attachSlots: SegmentStat['attachSlots'] = [];
  for (const row of h.attach) {
    if (row.refcount <= 0 || row.pid === 0) continue;
    // §7.1 liveness through the native check (review F34: the old JS path was
    // a bare kill(pid,0) — no zombie detection, no start-time match, and
    // Windows always said alive).
    // Every platform has a native §7.1 check (Windows: OpenProcess +
    // GetExitCodeProcess + creation time) — round-3 F34: Windows used to be
    // hard-coded 'unknown', so reap() could never collect there.
    const alive = livenessOf(row.pid, row.startTime, row.pidNsInode);
    attachSlots.push({
      pid: row.pid,
      startTime: row.startTime,
      pidNsInode: row.pidNsInode,
      alive,
      refcount: row.refcount,
    });
  }
  return {
    name,
    kind: kindOf(h.flags),
    dataBytes: h.dataBytes,
    headerBytes: h.headerBytes,
    attachSlots,
    attachOverflow: (h.flags & FLAG_ATTACH_OVERFLOW) !== 0,
    unlinked: (h.flags & FLAG_UNLINKED) !== 0,
    initState: h.initState,
  };
}

/** All membridge segments in /dev/shm (Linux only; magic-filtered, §9). */
export function list(): string[] {
  if (process.platform !== 'linux') {
    throw new MembridgeError(
      'E_UNSUPPORTED',
      `list() is Linux-only: POSIX shm cannot be enumerated on ${process.platform}`,
    );
  }
  const out: string[] = [];
  for (const entry of fs.readdirSync('/dev/shm')) {
    // /dev/shm entries are bare object names; foreign files live there too,
    // so the membridge magic — not a name prefix — decides (§9).
    const path = `/dev/shm/${entry}`;
    try {
      // Non-blocking, no symlinks, regular files only (round-4 S4-2/S4-7):
      // /dev/shm is world-writable, so any local user can plant a FIFO
      // (a blocking open would hang list()/reap() forever) or a symlink.
      const { O_RDONLY, O_NONBLOCK, O_NOFOLLOW } = fs.constants;
      const fd = fs.openSync(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
      try {
        if (!fs.fstatSync(fd).isFile()) continue;
        const probe = Buffer.alloc(8);
        const n = fs.readSync(fd, probe, 0, 8, 0);
        if (n >= 8 && probe.readUInt32LE(0) === MAGIC) {
          out.push(`/${entry}`);
        }
      } finally {
        fs.closeSync(fd);
      }
    } catch {
      // raced unlink or unreadable foreign file: not ours
    }
  }
  return out.sort();
}

/**
 * Unlink segments whose every attach row is dead (or empty). Unknown
 * liveness counts as alive and blocks the reap (§9); a full attach table
 * (ATTACH_OVERFLOW) blocks it too — it could hide a live attacher.
 * `reap(name)` targets one segment on every platform; the scan form is
 * Linux-only (needs list()).
 */
export function reap(opts?: ReapOptions): ReapResult[] {
  const names = opts?.name !== undefined ? [opts.name] : list();
  const results: ReapResult[] = [];
  for (const name of names) {
    try {
      const st = stat(name);
      if (st.attachOverflow) {
        results.push({ name, attachers: st.attachSlots.length, reaped: false });
        continue;
      }
      const blocking = st.attachSlots.filter((s) => s.alive !== false);
      if (blocking.length > 0) {
        results.push({ name, attachers: blocking.length, reaped: false });
        continue;
      }
      if (!opts?.dryRun) {
        try {
          unlink(name);
        } catch {
          // lost the race with another reaper/unlink: fine
        }
      }
      results.push({ name, attachers: 0, reaped: true });
    } catch (e) {
      if (e instanceof MembridgeError && (e.code === 'E_NOT_FOUND' || e.code === 'E_INCOMPATIBLE')) {
        results.push({ name, attachers: 0, reaped: false });
        continue;
      }
      throw e;
    }
  }
  return results;
}
