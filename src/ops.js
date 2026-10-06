"use strict";
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
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
exports.capacity = capacity;
exports.stat = stat;
exports.list = list;
exports.reap = reap;
const node_fs_1 = __importDefault(require("node:fs"));
const errors_1 = require("./errors");
const native_1 = require("./native");
const core_1 = require("./core");
// Header constants — mirror cc/header.h.
const FLAG_ATTACH_OVERFLOW = 1;
const FLAG_UNLINKED = 2;
const KIND_MASK = 0xff << 8;
const MAGIC = 0x424d454d;
function kindOf(flags) {
    const k = flags & KIND_MASK;
    if (k === 1 << 8)
        return 'mutex';
    if (k === 2 << 8)
        return 'ring';
    return 'plain';
}
/** Free/total of the shm backing store. Linux only (§9). */
function capacity() {
    if (process.platform !== 'linux') {
        throw new errors_1.MembridgeError('E_UNSUPPORTED', `capacity() is Linux-only: ${process.platform} has no bounded shm filesystem`);
    }
    const s = node_fs_1.default.statfsSync('/dev/shm');
    return {
        totalBytes: Number(s.blocks) * Number(s.bsize),
        freeBytes: Number(s.bavail) * Number(s.bsize),
    };
}
/** Header contents of one segment: kind, sizes, attach table with liveness. */
function stat(name) {
    (0, core_1.validateName)(name);
    const b = (0, native_1.nativeOrThrow)();
    const h = b.readHeader(name, 128);
    if (h.magic !== MAGIC) {
        throw new errors_1.MembridgeError('E_INCOMPATIBLE', `'${name}' is not a membridge segment`, {
            segmentName: name,
        });
    }
    const identity = b.selfIdentity();
    const attachSlots = [];
    for (const row of h.attach) {
        if (row.refcount <= 0 || row.pid === 0)
            continue;
        // §7.1 liveness: kill(pid,0) (success or EPERM = alive) within our own
        // pid namespace; foreign namespaces read 'unknown' and block reap (§9).
        let alive = 'unknown';
        if (row.pidNsInode > 0 && identity.pidNsInode > 0 && row.pidNsInode !== identity.pidNsInode) {
            alive = 'unknown';
        }
        else if (process.platform === 'win32') {
            alive = true; // OpenProcess liveness lands with the Windows CI pass
        }
        else {
            try {
                process.kill(row.pid, 0);
                alive = true;
            }
            catch (e) {
                alive = e.code === 'EPERM';
            }
        }
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
function list() {
    if (process.platform !== 'linux') {
        throw new errors_1.MembridgeError('E_UNSUPPORTED', `list() is Linux-only: POSIX shm cannot be enumerated on ${process.platform}`);
    }
    const out = [];
    for (const entry of node_fs_1.default.readdirSync('/dev/shm')) {
        // /dev/shm entries are bare object names; foreign files live there too,
        // so the membridge magic — not a name prefix — decides (§9).
        const path = `/dev/shm/${entry}`;
        try {
            const fd = node_fs_1.default.openSync(path, 'r');
            try {
                const probe = Buffer.alloc(8);
                const n = node_fs_1.default.readSync(fd, probe, 0, 8, 0);
                if (n >= 8 && probe.readUInt32LE(0) === MAGIC) {
                    out.push(`/${entry}`);
                }
            }
            finally {
                node_fs_1.default.closeSync(fd);
            }
        }
        catch {
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
function reap(opts) {
    const names = opts?.name !== undefined ? [opts.name] : list();
    const results = [];
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
                    (0, core_1.unlink)(name);
                }
                catch {
                    // lost the race with another reaper/unlink: fine
                }
            }
            results.push({ name, attachers: 0, reaped: true });
        }
        catch (e) {
            if (e instanceof errors_1.MembridgeError && (e.code === 'E_NOT_FOUND' || e.code === 'E_INCOMPATIBLE')) {
                results.push({ name, attachers: 0, reaped: false });
                continue;
            }
            throw e;
        }
    }
    return results;
}
//# sourceMappingURL=ops.js.map