// index.ts — membridge public entry. sync/mutex/ringbuffer/ops land with
// their milestones (PLAN §14); `membridge/sync` is a separate export (§6).

export {
  open,
  close,
  unlink,
  isNative,
  type OpenOptions,
  type OpenMode,
  type SizePolicy,
  DEFAULT_MAX_SEGMENT_BYTES,
} from './core';
export { MembridgeError, isMembridgeErrorCode, type MembridgeErrorCode, type MembridgeErrorFields } from './errors';
