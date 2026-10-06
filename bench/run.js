'use strict';
// bench/run.js — runs every bench and prints one table. Numbers are
// informational only: shared CI runners are noisy and benches are NEVER
// pass/fail gates (PLAN §11, F10).

const path = require('node:path');

async function main() {
  const results = [];
  for (const name of ['contention', 'mutex', 'ring']) {
    const mod = require(path.join(__dirname, `${name}.js`));
    process.stdout.write(`running bench/${name}.js ...\n`);
    const rows = await mod.run();
    results.push({ name, rows });
  }
  process.stdout.write('\n');
  for (const { name, rows } of results) {
    process.stdout.write(`== ${name} ==\n`);
    for (const r of rows) process.stdout.write(`  ${r.label}: ${r.value}\n`);
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
