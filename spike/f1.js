'use strict';
// F1: Atomics.notify cannot wake a waiter in another process.
// Parent notifies after 200 ms; if the fact holds, notify() returns 0 (V8 sees
// no waiter in this process) and the child times out at ~3000 ms.

const { fork } = require('child_process');
const { addon, probeName } = require('./lib');

const a = addon();
const name = probeName('f1');
a.open(name, 4096); // create before forking

const child = fork(__dirname + '/f1-child.js', [name], { silent: true });
child.stderr.on('data', d => process.stderr.write('[child] ' + d));

const done = setTimeout(() => {
  console.error('FAIL f1-cross-process-notify: timed out waiting for child');
  try { a.unlink(name); } catch {}
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
    clearTimeout(done);
    try { a.unlink(name); } catch {}
    process.exitCode = pass ? 0 : 1;
    child.kill();
  });
});
