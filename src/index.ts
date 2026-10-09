// index.ts — shm-bridge public entry. sync/mutex/ringbuffer/ops land with
// their milestones (PLAN §14); `shm-bridge/sync` is a separate export (§6).

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
export { Mutex, type LockOptions, type LockResult, type MutexOptions, MUTEX_DATA_BYTES } from './mutex';
export { RingProducer, RingConsumer, RING_HEADER_BYTES, MIN_CAPACITY, MAX_CAPACITY } from './ringbuffer';
export { capacity, stat, list, reap, type SegmentStat, type Capacity, type ReapOptions, type ReapResult } from './ops';
export { ShmBridgeError, isShmBridgeErrorCode, type ShmBridgeErrorCode, type ShmBridgeErrorFields } from './errors';
