// Thin ESM wrapper over the CJS build (PLAN §11: two JS module copies would
// mean two JS-side registries in one process — there is exactly one).
import mb from '../dist/index.js';

export const {
  open,
  close,
  unlink,
  isNative,
  Mutex,
  RingProducer,
  RingConsumer,
  capacity,
  stat,
  list,
  reap,
  MembridgeError,
} = mb;
export default mb;
