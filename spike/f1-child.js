'use strict';
// F1 child: join the segment, Atomics.wait for 3 s on word 0, report.

const { addon } = require('./lib');
const a = addon();
const name = process.argv[2];
const sab = a.open(name, 4096);
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
