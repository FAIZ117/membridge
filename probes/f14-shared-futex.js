'use strict';
// F14 (PLAN §2): Linux shared futexes, reachable only via raw syscalls (glibc
// exports no futex symbols). Proves the M3 mechanism end to end:
//   1. FUTEX_WAIT/FUTEX_WAKE **without** FUTEX_PRIVATE_FLAG wakes a waiter in
//      another process (shared futexes are keyed by the underlying page).
//   2. futex_waitv (kernel >= 5.16): support check (ENOSYS => M3 falls back to
//      one thread per wait), cross-process wake, and its return-value
//      semantics: it returns the INDEX of the woken entry, not a count
//      (waking entry 1 returns 1).
//
// PASS = (1) wakes cross-process, and (2) either wakes cross-process with
// index semantics or reports ENOSYS cleanly.

const { fork } = require('child_process');
const { addon, probeName, unlinkQuietly } = require('./lib');

const a = addon();

if (process.env.SHM_BRIDGE_PROBE_ROLE === 'child') {
  const [name, mode, timeoutMs] = [process.env.SHM_BRIDGE_PROBE_NAME, process.env.SHM_BRIDGE_PROBE_MODE, process.env.SHM_BRIDGE_PROBE_TIMEOUT];
  const sab = a.open(name, 4096);
  const i32 = new Int32Array(sab);
  process.send({ type: 'ready' });
  const t0 = process.hrtime.bigint();
  const code = mode === 'waitv'
    ? a.futexWaitv([[i32, 0, 0], [i32, 1, 0]], Number(timeoutMs))
    : a.futexWait(i32, 0, 0, Number(timeoutMs));
  process.send({ type: 'result', code, elapsedMs: Number(process.hrtime.bigint() - t0) / 1e6 });
  process.exit(0);
}

const name = probeName('f14');
a.open(name, 4096); // create before forking

const ERRNOS = { 1: 'EPERM', 4: 'EINTR', 11: 'EAGAIN', 14: 'EFAULT', 22: 'EINVAL', 38: 'ENOSYS', 110: 'ETIMEDOUT' };
const label = c => (c >= 0 ? String(c) : `${ERRNOS[-c] || 'errno'}(-${c})`);

function forkChild(mode, timeoutMs) {
  const c = fork(__filename, {
    env: { ...process.env, SHM_BRIDGE_PROBE_ROLE: 'child', SHM_BRIDGE_PROBE_NAME: name, SHM_BRIDGE_PROBE_MODE: mode, SHM_BRIDGE_PROBE_TIMEOUT: String(timeoutMs) },
    silent: true,
  });
  c.stderr.on('data', d => process.stderr.write('[child] ' + d));
  return c;
}

function once(c, type) {
  return new Promise(res => c.on('message', function h(m) {
    if (m.type === type) { c.removeListener('message', h); res(m); }
  }));
}

let pass = true;

async function run() {
  const i32 = new Int32Array(a.open(name, 4096));

  // 1) plain shared futex, cross-process
  {
    const c = forkChild('wait', 3000);
    await once(c, 'ready');
    await new Promise(r => setTimeout(r, 200));
    const woke = a.futexWake(i32, 0, 1);
    const m = await once(c, 'result');
    const elapsed = Math.round(m.elapsedMs);
    console.log(`plain futex: FUTEX_WAKE returned ${woke}; child ${label(m.code)} after ${elapsed} ms`);
    const ok = woke === 1 && m.code === 0 && elapsed < 2000;
    console.log(ok ? '  -> shared FUTEX_WAIT/FUTEX_WAKE wakes across processes' : '  -> FAIL: no cross-process wake');
    if (!ok) pass = false;
  }

  // 2) futex_waitv: support, cross-process wake, index semantics
  for (const wakeWord of [0, 1]) {
    const c = forkChild('waitv', 3000);
    await once(c, 'ready');
    await new Promise(r => setTimeout(r, 200));
    const woke = a.futexWake(i32, wakeWord, 1);
    const m = await once(c, 'result');
    const elapsed = Math.round(m.elapsedMs);
    if (m.code === -38) {
      console.log(`futex_waitv: ENOSYS -> kernel < 5.16; M3 uses the one-thread-per-wait fallback (PLAN §6)`);
      break;
    }
    console.log(`futex_waitv: wake word ${wakeWord} -> FUTEX_WAKE returned ${woke}; child ${label(m.code)} after ${elapsed} ms`);
    const ok = woke === 1 && m.code === wakeWord && elapsed < 2000;
    if (!ok) pass = false;
    if (wakeWord === 1) {
      console.log('  -> return value is the INDEX of the woken entry (waking word 1 returned 1)');
    }
  }

  console.log(pass ? 'PASS f14-shared-futex' : 'FAIL f14-shared-futex');
  process.exitCode = pass ? 0 : 1;
}

const guard = setTimeout(() => {
  console.error('FAIL f14-shared-futex: timed out');
  process.exit(1);
}, 25000);

run()
  .catch(e => { console.error('FAIL f14-shared-futex:', e); process.exitCode = 1; })
  .finally(() => {
    clearTimeout(guard);
    unlinkQuietly(a, name);
  });
