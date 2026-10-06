'use strict';
// F1 (PLAN §2): Atomics.notify does not wake waiters in another process —
// V8 keeps its waiter list per process and never consults a shared kernel
// futex. This is why membridge needs native cross-process wait/notify (§6).
//
// PASS = the parent's Atomics.notify returns 0 (V8 sees no waiter in its own
// process) and the child times out at ~3000 ms instead of waking at ~200 ms.

const { fork } = require('child_process');
const { addon, probeName, unlinkQuietly } = require('./lib');

const a = addon();

if (process.env.MEMBRIDGE_PROBE_ROLE === 'child') {
  const sab = a.open(process.env.MEMBRIDGE_PROBE_NAME, 4096);
  const i32 = new Int32Array(sab);
  process.send({ type: 'ready' });
  const t0 = process.hrtime.bigint();
  const r = Atomics.wait(i32, 0, 0, 3000);
  process.send({
    type: 'result',
    result: r,
    elapsedMs: Number(process.hrtime.bigint() - t0) / 1e6,
  });
  process.exit(0);
}

const name = probeName('f1');
a.open(name, 4096); // create before forking

const child = fork(__filename, {
  env: { ...process.env, MEMBRIDGE_PROBE_ROLE: 'child', MEMBRIDGE_PROBE_NAME: name },
  silent: true,
});
child.stderr.on('data', d => process.stderr.write('[child] ' + d));

const guard = setTimeout(() => {
  console.error('FAIL f1-cross-process-notify: timed out');
  unlinkQuietly(a, name);
  process.exit(1);
}, 15000);

child.on('message', async m => {
  if (m.type !== 'ready') return;
  await new Promise(r => setTimeout(r, 200));
  const i32 = new Int32Array(a.open(name, 4096));
  const notified = Atomics.notify(i32, 0, 1);
  child.on('message', m2 => {
    if (m2.type !== 'result') return;
    const out = {
      notifyReturnedWoken: notified,
      childResult: m2.result,
      childElapsedMs: Math.round(m2.elapsedMs),
    };
    console.log('F1 measured:', JSON.stringify(out));
    const pass = notified === 0 && m2.result === 'timed-out' && m2.elapsedMs > 2500;
    console.log(pass
      ? 'PASS f1-cross-process-notify: Atomics.notify does not wake other processes'
      : 'FAIL f1-cross-process-notify: fact does not hold as stated');
    clearTimeout(guard);
    unlinkQuietly(a, name);
    process.exitCode = pass ? 0 : 1;
    child.kill();
  });
});
