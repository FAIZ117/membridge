'use strict';
// Shared helpers for the M1 spike scripts.

const path = require('path');
const { randomBytes } = require('crypto');

const ADDON = path.join(__dirname, 'build', 'Release', 'membridge_spike.node');

function addon() {
  return require(ADDON);
}

const runSuffix = randomBytes(5).toString('hex');

// /dev/shm hygiene (AGENTS.md guardrail 3): /membridge-probe-<unique>, always
// unlinked by the caller's cleanup path.
function probeName(tag) {
  return `/membridge-probe-${tag}-${process.pid}-${runSuffix}-${Date.now().toString(36)}`;
}

function cleanupAll(a, names) {
  for (const n of names) {
    try {
      a.unlink(n);
    } catch {
      // already gone
    }
  }
}

module.exports = { ADDON, addon, probeName, cleanupAll };
