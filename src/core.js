"use strict";
// core.ts — open/close/unlink/isNative (§5.1). Validation happens here AND in
// the native layer (defense in depth); the size is a JS number handled as a
// double end to end, never Uint32Value (F5).
Object.defineProperty(exports, "__esModule", { value: true });
exports.DEFAULT_MAX_SEGMENT_BYTES = void 0;
exports.validateSize = validateSize;
exports.validateName = validateName;
exports.open = open;
exports.close = close;
exports.unlink = unlink;
exports.isNative = isNative;
exports.debugRegistryHas = debugRegistryHas;
const errors_1 = require("./errors");
const native_1 = require("./native");
const fallback_1 = require("./fallback");
exports.DEFAULT_MAX_SEGMENT_BYTES = 256 * 1024 * 1024;
function maxSegmentBytes() {
    const raw = process.env.MEMBRIDGE_MAX_SEGMENT_BYTES;
    if (raw === undefined || raw === '')
        return exports.DEFAULT_MAX_SEGMENT_BYTES;
    const v = Number(raw);
    if (!Number.isSafeInteger(v) || v <= 0)
        return exports.DEFAULT_MAX_SEGMENT_BYTES;
    return v;
}
function validateSize(name, size) {
    if (size === undefined)
        return;
    if (!Number.isSafeInteger(size) || size < 1 || size > 4 * 1024 * 1024 * 1024) {
        throw new errors_1.MembridgeError('E_SIZE_INVALID', `size must be a safe integer >= 1 (got ${size})`, {
            segmentName: name,
            requested: size,
        });
    }
    const cap = maxSegmentBytes();
    if (size > cap) {
        throw new errors_1.MembridgeError('E_SIZE_INVALID', `size ${size} exceeds MEMBRIDGE_MAX_SEGMENT_BYTES (${cap})`, { segmentName: name, requested: size });
    }
}
// §5.5 — mirrors cc/segment.cc ValidateName.
function validateName(name) {
    const platform = process.platform;
    const maxLen = platform === 'darwin' ? 31 : 250;
    if (typeof name !== 'string' ||
        name.length < 2 ||
        !name.startsWith('/') ||
        name.indexOf('/', 1) !== -1 ||
        Buffer.byteLength(name, 'utf8') > maxLen) {
        throw new errors_1.MembridgeError('E_NAME_INVALID', `segment name must start with '/', contain no other '/', and be <= ${maxLen} bytes on ${platform}`, { segmentName: name });
    }
}
function toNativeOpts(opts) {
    const o = {};
    if (opts.mode !== undefined)
        o.mode = opts.mode;
    if (opts.sizePolicy !== undefined)
        o.sizePolicy = opts.sizePolicy;
    if (opts.reserve !== undefined)
        o.reserve = opts.reserve;
    if (opts.permissions !== undefined)
        o.permissions = opts.permissions;
    if (opts.initTimeoutMs !== undefined)
        o.initTimeoutMs = opts.initTimeoutMs;
    if (opts.raw !== undefined)
        o.raw = opts.raw;
    if (opts.winGlobal !== undefined)
        o.winGlobal = opts.winGlobal;
    if (opts.unlinkWhenUnused !== undefined)
        o.unlinkWhenUnused = opts.unlinkWhenUnused;
    if (opts.kind !== undefined) {
        o.kind = opts.kind === 'mutex' ? 1 : opts.kind === 'ring' ? 2 : 0;
    }
    return o;
}
function open(name, sizeOrOpts, maybeOpts) {
    let size;
    let opts;
    if (typeof sizeOrOpts === 'number') {
        size = sizeOrOpts;
        opts = maybeOpts ?? {};
    }
    else {
        opts = (sizeOrOpts ?? {});
    }
    validateName(name);
    validateSize(name, size);
    const nativeBinding = (0, native_1.native)();
    if (nativeBinding !== undefined) {
        return nativeBinding.open(name, size, toNativeOpts(opts));
    }
    if ((0, fallback_1.fallbackEnabled)(opts)) {
        return (0, fallback_1.fallbackOpen)(name, size, opts);
    }
    return (0, native_1.nativeOrThrow)().open(name, size, toNativeOpts(opts));
}
/**
 * Compatibility no-op (§5.4): it cannot unmap memory that live SABs still
 * reference, and the registry holds only weak references, so there is no
 * per-isolate state to release. shmbuf call sites keep working. Memory is
 * released when the last SAB is GC'd; the name is removed by {@link unlink}.
 * Invalid names still throw E_NAME_INVALID.
 */
function close(name) {
    validateName(name);
    const nativeBinding = (0, native_1.native)();
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
function unlink(name) {
    validateName(name);
    const nativeBinding = (0, native_1.native)();
    if (nativeBinding !== undefined) {
        nativeBinding.unlink(name);
        return;
    }
    (0, fallback_1.fallbackUnlink)(name);
}
/** True when the native addon is loaded in this process. */
function isNative() {
    return (0, native_1.native)() !== undefined;
}
/** Undocumented test hook: is a live mapping cached in the process registry? */
function debugRegistryHas(name) {
    return (0, native_1.nativeOrThrow)().debugRegistryHas(name);
}
//# sourceMappingURL=core.js.map