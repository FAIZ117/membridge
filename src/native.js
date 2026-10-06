"use strict";
// native.ts — loads the plain-V8 addon via node-gyp-build (PLAN §12): the
// per-ABI prebuild first, then build/Release, then the source-build fallback
// (node-gyp runs from the package root, which is why binding.gyp lives there).
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
exports.native = native;
exports.isNative = isNative;
exports.nativeOrThrow = nativeOrThrow;
exports.nativeErrorCode = nativeErrorCode;
const node_path_1 = __importDefault(require("node:path"));
const errors_1 = require("./errors");
let binding;
let loadAttempted = false;
function load() {
    if (loadAttempted)
        return binding;
    loadAttempted = true;
    try {
        // dist/src/native.js -> package root (binding.gyp, prebuilds/, build/Release)
        const loadAddon = require('node-gyp-build');
        binding = loadAddon(node_path_1.default.join(__dirname, '..', '..'));
        if (binding && typeof binding.setMembridgeErrorCtor === 'function') {
            binding.setMembridgeErrorCtor(errors_1.MembridgeError);
        }
    }
    catch {
        binding = undefined; // fallback path handles this (§5.6)
    }
    return binding;
}
function native() {
    const b = load();
    if (!b)
        return undefined;
    return b;
}
/** True when the native addon is available in this process. */
function isNative() {
    return load() !== undefined;
}
/** The native binding; throws E_NATIVE_UNAVAILABLE when absent. */
function nativeOrThrow() {
    const b = load();
    if (!b) {
        throw new errors_1.MembridgeError('E_NATIVE_UNAVAILABLE', 'the membridge native addon is not available; set MEMBRIDGE_ALLOW_FALLBACK=1 ' +
            'or pass allowFallback to opt into the in-process fallback');
    }
    return b;
}
function nativeErrorCode(err) {
    if (err !== null && typeof err === 'object' && typeof err.code === 'string') {
        return err.code;
    }
    return undefined;
}
//# sourceMappingURL=native.js.map