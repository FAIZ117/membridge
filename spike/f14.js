'use strict';
// F14: shared futexes wake across processes on shm pages, via raw syscalls
// (glibc has no futex wrapper, F14). Proves the M3 mechanism end to end:
//   1. FUTEX_WAIT/FUTEX_WAKE **without** FUTEX_PRIVATE_FLAG, cross-process.
//   2. futex_waitv (kernel >= 5.16): support check (ENOSYS -> M3 falls back to
//      one thread per wait), cross-process wake, and return-value semantics
//      (wake word 0 -> expect 0; wake word 1 -> expect 1  =>  index of the
//      woken entry, not a count).

const { fork } = require('child_process');
const { addon, probeName } = require('./lib');

const a = addon();
const name = probeName('f14');
a.open(name, 4096); // create before forking

const ERRNOS = { 1: 'EPERM', 4: 'EINTR', 11: 'EAGAIN', 14: 'EFAULT', 22: 'EINVAL', 38: 'ENOSYS', 110: 'ETIMEDOUT' };
const label = c => (c >= 0 ? String(c) : `${ERRNOS[-c] || 'errno'}(-${c})`);

function forkChild(mode, timeoutMs) {
  const c = fork(__dirname + '/f14-child.js', [name, mode, '0', String(timeoutMs)], { silent: true });
  c.stderr.on('data', d => process.stderr.write('[child] ' + d));
  return c;
}

function once(c, type) {
  return new Promise(res => c.on('message', function h(m) {
    if (m.type === type) { c.removeListener('message', h); res(m); }
  }));
}

let pass = true;

async function plainFutex() {
  const c = forkChild('wait', 3000);
  await once(c, 'ready');
  await new Promise(r => setTimeout(r, 200));
  const woke = a.futexWake(new Int32Array(a.open(name, 4096)), 0, 1);
  const m = await once(c, 'result');
  const elapsed = Math.round(m.elapsedMs);
  console.log(`plain futex: FUTEX_WAKE returned ${woke}; child ${label(m.code)} after ${elapsed} ms`);
  const ok = woke === 1 && m.code === 0 && elapsed < 2000;
  if (!ok) pass = false;
  return ok;
}

async function waitv(wakeWord, expectedIndex) {
  const c = forkChild('waitv', 3000);
  await once(c, 'ready');
  await new Promise(r => setTimeout(r, 200));
  const woke = a.futexWake(new Int32Array(a.open(name, 4096)), wakeWord, 1);
  const m = await once(c, 'result');
  const elapsed = Math.round(m.elapsedMs);
  console.log(`futex_waitv: wake word ${wakeWord} -> FUTEX_WAKE returned ${woke}; child ${label(m.code)} after ${elapsed} ms`);
  return { m, elapsed, woke };
}

async function run() {
  const i32 = new Int32Array(a.open(name, 4096));

  const r1 = await plainFutex();
  console.log(r1 ? '  -> shared FUTEX_WAIT/FUTEX_WAKE wakes across processes' : '  -> FAIL: no cross-process wake');

  const r2 = await waitv(0, 0);
  if (r2.m.code === -38) {
    console.log('  -> futex_waitv unavailable (ENOSYS): M3 uses the one-thread-per-wait fallback (PLAN §6)');
  } else {
    const ok2 = r2.woke === 1 && r2.m.code === 0 && r2.elapsed < 2000;
    console.log(ok2 ? '  -> futex_waitv wakes across processes; waking entry 0 returned 0' : '  -> FAIL');
    if (!ok2) pass = false;
    // Pin return-value semantics: waking entry 1 must return 1 (index), which
    // distinguishes index semantics from a wake count.
    const r3 = await waitv(1, 1);
    const ok3 = r3.woke === 1 && r3.m.code === 1 && r3.elapsed < 2000;
    console.log(ok3
      ? '  -> return value is the INDEX of the woken entry (r=1 when waking word 1)'
      : `  -> return semantics NOT index-as-expected: got ${label(r3.m.code)}`);
    if (!ok3) pass = false;
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
    try { a.unlink(name); } catch {}
  });
