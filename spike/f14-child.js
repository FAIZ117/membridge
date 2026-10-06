'use strict';
// F14 child: join the segment, then block on a shared futex via raw syscall.
//   mode=wait  -> futexWait(word 0, expected 0)
//   mode=waitv -> futexWaitv([word 0, word 1]) — waits on BOTH words; the
//                 parent chooses which one to wake, and the raw return value
//                 pins the syscall's semantics (index of woken entry vs count).

const { addon } = require('./lib');
const a = addon();
const [name, mode, , timeoutMs] = process.argv.slice(2);
const sab = a.open(name, 4096);
const i32 = new Int32Array(sab);
process.send({ type: 'ready' });
const t0 = process.hrtime.bigint();
let code;
if (mode === 'wait') {
  code = a.futexWait(i32, 0, 0, Number(timeoutMs));
} else {
  code = a.futexWaitv([[i32, 0, 0], [i32, 1, 0]], Number(timeoutMs));
}
process.send({
  type: 'result',
  code,
  elapsedMs: Number(process.hrtime.bigint() - t0) / 1e6,
});
process.exit(0);
