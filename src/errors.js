"use strict";
// errors.ts — MembridgeError with E_* codes (PLAN §10). A new code goes into
// PLAN §10 and this file in the same change (AGENTS.md guardrail 5).
Object.defineProperty(exports, "__esModule", { value: true });
exports.MembridgeError = void 0;
exports.isMembridgeErrorCode = isMembridgeErrorCode;
const CODES = new Set([
    'E_NAME_INVALID',
    'E_SIZE_INVALID',
    'E_SIZE_MISMATCH',
    'E_EXISTS',
    'E_NOT_FOUND',
    'E_INCOMPATIBLE',
    'E_INIT_TIMEOUT',
    'E_NO_SPACE',
    'E_GROW_UNSUPPORTED',
    'E_NOT_OWNER',
    'E_DEADLOCK',
    'E_TIMEOUT',
    'E_ROLE_TAKEN',
    'E_RING_STATE',
    'E_MESSAGE_TOO_LARGE',
    'E_TOO_MANY_WAITERS',
    'E_NATIVE_UNAVAILABLE',
    'E_UNSUPPORTED',
    'E_SYSTEM',
]);
class MembridgeError extends Error {
    code;
    segmentName;
    requested;
    existing;
    syscall;
    errno;
    constructor(code, message, fields = {}) {
        super(message);
        this.name = 'MembridgeError';
        this.code = code;
        if (fields.segmentName !== undefined)
            this.segmentName = fields.segmentName;
        if (fields.requested !== undefined)
            this.requested = fields.requested;
        if (fields.existing !== undefined)
            this.existing = fields.existing;
        if (fields.syscall !== undefined)
            this.syscall = fields.syscall;
        if (fields.errno !== undefined)
            this.errno = fields.errno;
    }
}
exports.MembridgeError = MembridgeError;
function isMembridgeErrorCode(code) {
    return CODES.has(code);
}
//# sourceMappingURL=errors.js.map