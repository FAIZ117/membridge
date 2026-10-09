'use strict';
// Shared helpers for probes/ — locate the spike addon, generate unique
// /shm-bridge-probe-* names, clean up. (AGENTS.md guardrail 3: segments are
// always under /shm-bridge-probe- and always unlinked.)
//
// The addon is ABI-bound (plain V8, not N-API), so if the cached build does
// not match the running Node, lib.js rebuilds it for the current version via
// node-gyp (headers come from the local cache or nodejs.org).

const path = require('path');
const fs = require('fs');
const { spawnSync } = require('child_process');

const ADDON = process.env.SHM_BRIDGE_SPIKE_ADDON ||
  path.join(__dirname, '..', 'spike', 'build', 'Release', 'shm_bridge_spike.node');

function rebuildForCurrentNode() {
  console.error(`probes: rebuilding spike addon for Node ${process.versions.node} ...`);
  const r = spawnSync('npx', ['-y', 'node-gyp', 'rebuild', `--target=${process.versions.node}`], {
    cwd: path.join(__dirname, '..', 'spike'),
    stdio: 'inherit',
  });
  return r.status === 0 && fs.existsSync(ADDON);
}

function addon() {
  if (!fs.existsSync(ADDON) && !rebuildForCurrentNode()) {
    console.error(`spike addon not available at ${ADDON}`);
    console.error(`  cd spike && npx node-gyp rebuild --target=${process.versions.node}`);
    process.exit(2);
  }
  try {
    return require(ADDON);
  } catch (e) {
    // ABI mismatch (ERR_DLOPEN_FAILED / NODE_MODULE_VERSION) -> rebuild once.
    if (rebuildForCurrentNode()) return require(ADDON);
    console.error(`spike addon at ${ADDON} failed to load:`, e.message);
    process.exit(2);
  }
}

const runSuffix = `${process.pid}-${Math.random().toString(36).slice(2, 8)}`;

function probeName(tag) {
  return `/shm-bridge-probe-${tag}-${runSuffix}-${Date.now().toString(36)}`;
}

function unlinkQuietly(a, names) {
  for (const n of [].concat(names)) {
    try {
      a.unlink(n);
    } catch {
      // already gone
    }
  }
}

module.exports = { ADDON, addon, probeName, unlinkQuietly };
