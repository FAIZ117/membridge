export type MembridgeErrorCode = 'E_NAME_INVALID' | 'E_SIZE_INVALID' | 'E_SIZE_MISMATCH' | 'E_EXISTS' | 'E_NOT_FOUND' | 'E_INCOMPATIBLE' | 'E_INIT_TIMEOUT' | 'E_NO_SPACE' | 'E_GROW_UNSUPPORTED' | 'E_NOT_OWNER' | 'E_DEADLOCK' | 'E_TIMEOUT' | 'E_ROLE_TAKEN' | 'E_RING_STATE' | 'E_MESSAGE_TOO_LARGE' | 'E_TOO_MANY_WAITERS' | 'E_NATIVE_UNAVAILABLE' | 'E_UNSUPPORTED' | 'E_SYSTEM';
export interface MembridgeErrorFields {
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
export declare class MembridgeError extends Error {
    readonly code: MembridgeErrorCode;
    readonly segmentName?: string;
    readonly requested?: number;
    readonly existing?: number;
    readonly syscall?: string;
    readonly errno?: number;
    constructor(code: MembridgeErrorCode, message: string, fields?: MembridgeErrorFields);
}
export declare function isMembridgeErrorCode(code: string): code is MembridgeErrorCode;
