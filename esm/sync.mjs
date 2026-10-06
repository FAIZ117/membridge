// Thin ESM wrapper over the CJS build (single module registry per process).
import sync from '../dist/sync.js';

export const { wait, waitAsync, notify } = sync;
export default sync;
