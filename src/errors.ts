// errors.ts — ShmBridgeError with E_* codes (PLAN §10). A new code goes into
// PLAN §10 and this file in the same change (AGENTS.md guardrail 5).

export type ShmBridgeErrorCode =
  | 'E_NAME_INVALID'
  | 'E_SIZE_INVALID'
  | 'E_SIZE_MISMATCH'
  | 'E_EXISTS'
  | 'E_NOT_FOUND'
  | 'E_INCOMPATIBLE'
  | 'E_INIT_TIMEOUT'
  | 'E_NO_SPACE'
  | 'E_GROW_UNSUPPORTED'
  | 'E_NOT_OWNER'
  | 'E_DEADLOCK'
  | 'E_TIMEOUT'
  | 'E_ROLE_TAKEN'
  | 'E_RING_STATE'
  | 'E_CLOSED'
  | 'E_MESSAGE_TOO_LARGE'
  | 'E_TOO_MANY_WAITERS'
  | 'E_NATIVE_UNAVAILABLE'
  | 'E_UNSUPPORTED'
  | 'E_SYSTEM';

export interface ShmBridgeErrorFields {
  /** The segment the error is about. */
  readonly segmentName?: string;
  /** Requested size for size errors. */
  readonly requested?: number;
  /** Existing size for size errors. */
  readonly existing?: number;
  /** Syscall name for E_SYSTEM. */
  readonly syscall?: string;
  /** errno (or GetLastError) for E_SYSTEM. */
  readonly errno?: number;
}

const CODES: ReadonlySet<string> = new Set<string>([
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
  'E_CLOSED',
  'E_MESSAGE_TOO_LARGE',
  'E_TOO_MANY_WAITERS',
  'E_NATIVE_UNAVAILABLE',
  'E_UNSUPPORTED',
  'E_SYSTEM',
]);

export class ShmBridgeError extends Error {
  readonly code: ShmBridgeErrorCode;
  readonly segmentName?: string;
  readonly requested?: number;
  readonly existing?: number;
  readonly syscall?: string;
  readonly errno?: number;

  constructor(code: ShmBridgeErrorCode, message: string, fields: ShmBridgeErrorFields = {}) {
    super(message);
    this.name = 'ShmBridgeError';
    this.code = code;
    if (fields.segmentName !== undefined) this.segmentName = fields.segmentName;
    if (fields.requested !== undefined) this.requested = fields.requested;
    if (fields.existing !== undefined) this.existing = fields.existing;
    if (fields.syscall !== undefined) this.syscall = fields.syscall;
    if (fields.errno !== undefined) this.errno = fields.errno;
  }
}

export function isShmBridgeErrorCode(code: string): code is ShmBridgeErrorCode {
  return CODES.has(code);
}
