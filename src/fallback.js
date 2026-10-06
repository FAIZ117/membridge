"use strict";
// fallback.ts — opt-in in-process fallback (§5.6). It never "silently works":
// without MEMBRIDGE_ALLOW_FALLBACK=1 or allowFallback, a missing addon is
// E_NATIVE_UNAVAILABLE. The fallback maps names to JS SharedArrayBuffers in
// THIS PROCESS ONLY — a cross-process bug if misused as if it were shared
// memory. It still checks sizes (shmbuf ignored size on re-open). Mutex and
// RingBuffer refuse it unless explicitly opted in.
Object.defineProperty(exports, "__esModule", { value: true });
exports.fallbackEnabled = fallbackEnabled;
exports.fallbackOpen = fallbackOpen;
exports.fallbackUnlink = fallbackUnlink;
const errors_1 = require("./errors");
const segments = new Map();
function fallbackEnabled(opts) {
    if (opts?.allowFallback === true)
        return true;
    return process.env.MEMBRIDGE_ALLOW_FALLBACK === '1';
}
function fallbackOpen(name, size, opts) {
    const existing = segments.get(name);
    if (opts.mode === 'create' && existing !== undefined) {
        throw new errors_1.MembridgeError('E_EXISTS', `segment '${name}' already exists`, { segmentName: name });
    }
    if (existing === undefined) {
        if (size === undefined) {
            throw new errors_1.MembridgeError('E_NOT_FOUND', `segment '${name}' does not exist`, { segmentName: name });
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
                throw new errors_1.MembridgeError('E_SIZE_MISMATCH', `requested ${size} vs existing ${existing.dataBytes}`, {
                    segmentName: name,
                    requested: size,
                    existing: existing.dataBytes,
                });
            }
            return existing.sab;
        case 'at-least':
            if (size > existing.dataBytes) {
                throw new errors_1.MembridgeError('E_SIZE_MISMATCH', `requested ${size} vs existing ${existing.dataBytes}`, {
                    segmentName: name,
                    requested: size,
                    existing: existing.dataBytes,
                });
            }
            // A SAB cannot be a prefix view (F15) — the fallback hands back the
            // full buffer; it is single-process anyway.
            return existing.sab;
        case 'grow':
            throw new errors_1.MembridgeError('E_GROW_UNSUPPORTED', 'grow is unsupported in the fallback', {
                segmentName: name,
            });
    }
}
function fallbackUnlink(name) {
    if (!segments.delete(name)) {
        throw new errors_1.MembridgeError('E_NOT_FOUND', `segment '${name}' does not exist`, { segmentName: name });
    }
}
//# sourceMappingURL=fallback.js.map