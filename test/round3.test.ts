// round3.test.ts — regressions for the round-3 re-verification
// (docs/review/2026-10-07T1236-round3-reverification.md, findings C1–C24).
// Each test names the finding it pins. Multi-thread / multi-process races run
// several rounds to give the old bugs a realistic chance to show; every one
// of them reproduced reliably at 248f731.

import { test } from 'node:test';
import { spawn, spawnSync } from 'node:child_process';
import { Worker } from 'node:worker_threads';
import { open } from '../src/core';
import { Mutex, MUTEX_DATA_BYTES, SLOT_LAYOUT } from '../src/mutex';
import { RingProducer, RingConsumer, RING_HEADER_BYTES } from '../src/ringbuffer';
import { stat } from '../src/ops';
import { assert, assertThrowsCode, uniqueName, holdForTest, unlinkQuietly } from './helpers';

const CORE = require.resolve('../src/core');
const MUTEX = require.resolve('../src/mutex');
const RING = require.resolve('../src/ringbuffer');
const NATIVE = require.resolve('../src/native');
const OPS = require.resolve('../src/ops');
const POSIX = process.platform !== 'win32';
const LINUX = process.platform === 'linux';  // byte-crafting (/dev/shm) and grow (darwin refuses, U2)

/** Run a node -e script; returns { status, signal, out } with trimmed stdout. */
function runNode(code: string, flags: string[] = [], env: NodeJS.ProcessEnv = process.env): { status: number | null; signal: string | null; out: string; err: string } {
  const r = spawnSync(process.execPath, [...flags, '-e', code], { encoding: 'utf8', timeout: 60_000, env });
  return { status: r.status, signal: r.signal, out: (r.stdout ?? '').trim(), err: (r.stderr ?? '').trim() };
}

function lastJson(out: string): any {
  const lines = out.split('\n').filter((l) => l.startsWith('{'));
  return JSON.parse(lines[lines.length - 1]!);
}

// ---- C1: a sibling instance's close/GC must not release another's lock ------

test('C1: closing one Mutex instance does not release a lock its sibling holds', () => {
  const name = uniqueName();
  try {
    const a = Mutex.open(name);
    a.lock();
    a.unlock();
    const b = Mutex.open(name);
    assert.strictEqual(b.lock().ownerDied, false);
    a.close();
    const other = runNode(
      `const {Mutex}=require(${JSON.stringify(MUTEX)});const m=Mutex.open(${JSON.stringify(name)});` +
        `try{m.lock({timeoutMs:300});console.log('ACQUIRED')}catch(e){console.log(e.code)}`,
    );
    assert.strictEqual(other.out, 'E_TIMEOUT', 'another process must not get in while B holds');
    b.unlock();  // B still owns it: no E_NOT_OWNER
    const after = runNode(
      `const {Mutex}=require(${JSON.stringify(MUTEX)});const m=Mutex.open(${JSON.stringify(name)});` +
        `const r=m.lock({timeoutMs:2000});console.log(JSON.stringify(r));m.unlock()`,
    );
    assert.deepStrictEqual(lastJson(after.out), { ownerDied: false });
  } finally {
    unlinkQuietly(name);
  }
});

test('C1: a garbage-collected sibling does not release the holder\'s lock', () => {
  const name = uniqueName();
  try {
    const r = runNode(
      `const {Mutex}=require(${JSON.stringify(MUTEX)});const {Worker}=require('node:worker_threads');
       (async()=>{
         let a=Mutex.open(${JSON.stringify(name)}); a.lock(); a.unlock();
         const b=Mutex.open(${JSON.stringify(name)}); b.lock();
         a=null;
         for(let i=0;i<20;i++){ globalThis.gc(); await new Promise(r=>setImmediate(r)); }
         const w=new Worker(\`const {Mutex}=require(\${JSON.stringify(${JSON.stringify(MUTEX)})});
           const m=Mutex.open(\${JSON.stringify(${JSON.stringify(name)})});
           let res; try{m.lock({timeoutMs:300});res='ACQUIRED'}catch(e){res=e.code}
           require('node:worker_threads').parentPort.postMessage(res);\`,{eval:true});
         const seen=await new Promise(r=>w.once('message',r));
         let unlock='ok'; try{b.unlock()}catch(e){unlock=e.code}
         console.log(JSON.stringify({seen,unlock})); process.exit(0);
       })();`,
      ['--expose-gc'],
    );
    assert.strictEqual(r.signal, null, r.err);
    assert.deepStrictEqual(lastJson(r.out), { seen: 'E_TIMEOUT', unlock: 'ok' });
  } finally {
    unlinkQuietly(name);
  }
});

// ---- C2: threads of one process never share a slot / overlap ---------------

test('C2: concurrent worker claims get distinct slots and mutual exclusion holds', async (t) => {
  for (let round = 0; round < 4; round++) {
    const name = uniqueName();
    const flagName = uniqueName();
    const workers: Worker[] = [];
    try {
      holdForTest(t, Mutex.open(name));
      const flags = new Int32Array(open(flagName, 64, { mode: 'create' }));
      const N = 8;
      const results: Array<Promise<{ overlaps: number }>> = [];
      for (let i = 0; i < N; i++) {
        const w = new Worker(
          `const { workerData, parentPort } = require('node:worker_threads');
           const { Mutex } = require(workerData.MUTEX);
           const { open } = require(workerData.CORE);
           const f = new Int32Array(open(workerData.flagName, 64, { mode: 'join' }));
           const m = Mutex.open(workerData.name);
           while (Atomics.load(f, 1) === 0) {}            // start barrier: claims race
           let overlaps = 0;
           for (let k = 0; k < 400; k++) {
             m.lock();
             if (Atomics.exchange(f, 0, 1) !== 0) overlaps++;
             for (let s = 0; s < 50; s++) Atomics.add(f, 2, 1);
             Atomics.store(f, 0, 0);
             m.unlock();
           }
           parentPort.postMessage({ overlaps });
           parentPort.once('message', () => process.exit(0));`,
          { eval: true, workerData: { MUTEX, CORE, name, flagName } },
        );
        workers.push(w);
        results.push(new Promise((r) => w.once('message', r)));
      }
      await new Promise((r) => setTimeout(r, 150));
      Atomics.store(flags, 1, 1);
      const rs = await Promise.all(results);
      assert.strictEqual(rs.reduce((s, r) => s + r.overlaps, 0), 0, `round ${round}: overlaps`);
      // Every live worker holds its own Active slot (distinct tids => N slots).
      const v = new Int32Array(open(name, MUTEX_DATA_BYTES, { mode: 'join', kind: 'mutex' }));
      const tids = new Set<number>();
      let active = 0;
      for (let s = 0; s < SLOT_LAYOUT.SLOT_COUNT; s++) {
        const base = SLOT_LAYOUT.SLOTS + s * SLOT_LAYOUT.SLOT_WORDS;
        if (Atomics.load(v, base + SLOT_LAYOUT.STATE) !== SLOT_LAYOUT.STATE_ACTIVE) continue;
        if (Atomics.load(v, base + SLOT_LAYOUT.PID) !== process.pid) continue;
        active++;
        tids.add(Atomics.load(v, base + SLOT_LAYOUT.TID));
      }
      assert.strictEqual(active, N, `round ${round}: one Active slot per worker`);
      assert.strictEqual(tids.size, N, `round ${round}: no two workers share a slot`);
      for (const w of workers) w.postMessage('exit');
      await Promise.all(workers.map((w) => new Promise((r) => w.once('exit', r))));
    } finally {
      // a failed assert above skips the 'exit' handshake; live workers would
      // hold the file's event loop open until the runner's timeout
      await Promise.all(workers.map((w) => w.terminate()));
      unlinkQuietly(name);
      unlinkQuietly(flagName);
    }
  }
});

// ---- C3: cross-process reclaim of dead slots is exclusive --------------------

test('C3: processes reclaiming dead slots concurrently never overlap', { skip: POSIX ? false : 'POSIX' }, async () => {
  for (let round = 0; round < 3; round++) {
    const name = uniqueName();
    const flagName = uniqueName();
    try {
      Mutex.open(name);
      const flags = new Int32Array(open(flagName, 64, { mode: 'create' }));
      const dead = spawnSync(process.execPath, ['-e', '0']);
      const deadPid = dead.pid ?? 999_999;
      const v = new Int32Array(open(name, MUTEX_DATA_BYTES, { mode: 'join', kind: 'mutex' }));
      for (let s = 0; s < SLOT_LAYOUT.SLOT_COUNT; s++) {
        const base = SLOT_LAYOUT.SLOTS + s * SLOT_LAYOUT.SLOT_WORDS;
        Atomics.store(v, base + SLOT_LAYOUT.PID, deadPid);
        Atomics.store(v, base + SLOT_LAYOUT.TID, s + 1);
        Atomics.store(v, base + SLOT_LAYOUT.START_LO, 777 + s);
        Atomics.store(v, base + SLOT_LAYOUT.START_HI, 0);
        Atomics.store(v, base + SLOT_LAYOUT.GEN, s + 1);
        Atomics.store(v, base + SLOT_LAYOUT.STATE, SLOT_LAYOUT.STATE_ACTIVE);
      }
      const P = 6;
      const procs = [];
      for (let i = 0; i < P; i++) {
        procs.push(new Promise<{ code: number | null; out: string }>((resolve) => {
          const c = spawn(process.execPath, ['-e', `
            const {Mutex}=require(${JSON.stringify(MUTEX)});const {open}=require(${JSON.stringify(CORE)});
            const f=new Int32Array(open(${JSON.stringify(flagName)},64,{mode:'join'}));
            const m=Mutex.open(${JSON.stringify(name)});
            while(Atomics.load(f,1)===0){}
            let overlaps=0;
            for(let k=0;k<300;k++){ m.lock(); if(Atomics.exchange(f,0,1)!==0) overlaps++;
              for(let s=0;s<50;s++) Atomics.add(f,2,1); Atomics.store(f,0,0); m.unlock(); }
            console.log(JSON.stringify({overlaps}));`], { stdio: ['ignore', 'pipe', 'pipe'] });
          let out = '';
          c.stdout.on('data', (d) => { out += d; });
          c.on('exit', (code) => resolve({ code, out }));
        }));
      }
      await new Promise((r) => setTimeout(r, 400));
      Atomics.store(flags, 1, 1);
      const rs = await Promise.all(procs);
      for (const r of rs) {
        assert.strictEqual(r.code, 0, r.out);
        assert.strictEqual(lastJson(r.out).overlaps, 0, `round ${round}: overlap across processes`);
      }
    } finally {
      unlinkQuietly(name);
      unlinkQuietly(flagName);
    }
  }
});

// ---- C4: ring role claims race between threads -------------------------------

test('C4: exactly one of N racing worker threads becomes the consumer', async (t) => {
  for (let round = 0; round < 40; round++) {
    const name = uniqueName();
    const workers: Worker[] = [];
    try {
      holdForTest(t, open(name, RING_HEADER_BYTES + 4096, { kind: 'ring', mode: 'create' }));
      const barrier = new Int32Array(new SharedArrayBuffer(4));
      const N = 6;
      const results: Array<Promise<string>> = [];
      for (let i = 0; i < N; i++) {
        const w = new Worker(
          `const { workerData, parentPort } = require('node:worker_threads');
           const { RingConsumer } = require(workerData.RING);
           while (Atomics.load(workerData.barrier, 0) === 0) {}
           let res, c;
           try { c = RingConsumer.open(workerData.name); res = 'OK'; } catch (e) { res = e.code; }
           parentPort.postMessage(res);
           parentPort.once('message', () => { if (c) c.close(); process.exit(0); });`,
          { eval: true, workerData: { RING, name, barrier } },
        );
        workers.push(w);
        results.push(new Promise((r) => w.once('message', r)));
      }
      await new Promise((r) => setTimeout(r, 100));
      Atomics.store(barrier, 0, 1);
      const rs = await Promise.all(results);
      assert.strictEqual(rs.filter((r) => r === 'OK').length, 1, `round ${round}: ${rs.join(',')}`);
      assert.ok(rs.every((r) => r === 'OK' || r === 'E_ROLE_TAKEN'), rs.join(','));
      for (const w of workers) w.postMessage('exit');
      await Promise.all(workers.map((w) => new Promise((r) => w.once('exit', r))));
    } finally {
      await Promise.all(workers.map((w) => w.terminate())); // see C2
      unlinkQuietly(name);
    }
  }
});

// ---- C5: attach-row unwind after a grow re-mapped the handle -----------------

test('C5: a throw after a winning grow releases the row through the new mapping', { skip: LINUX ? false : 'grow is POSIX-only' }, () => {
  const name = uniqueName();
  try {
    const keep = open(name, 4096);
    const r = runNode(
      `const {open}=require(${JSON.stringify(CORE)});const {nativeOrThrow}=require(${JSON.stringify(NATIVE)});
       const {stat}=require(${JSON.stringify(OPS)});
       nativeOrThrow().debugFailAfterGrow();
       let code='none'; try{ open(${JSON.stringify(name)}, 1<<20, {sizePolicy:'grow'}); }catch(e){ code=e.code; }
       const mine=stat(${JSON.stringify(name)}).attachSlots.filter(s=>s.pid===process.pid).length;
       console.log(JSON.stringify({code, mine}));`,
      [], { ...process.env, SHM_BRIDGE_TEST_HOOKS: '1' },
    );
    assert.strictEqual(r.signal, null, `child crashed: ${r.err}`);
    assert.deepStrictEqual(lastJson(r.out), { code: 'E_SYSTEM', mine: 0 });
    assert.ok(keep.byteLength === 4096);
  } finally {
    unlinkQuietly(name);
  }
});

// ---- C6 (+C19/C20): async ring paths never lose a wake ----------------------

test('C6: async ping-pong never stalls a wait slice', async () => {
  const req = uniqueName();
  const resp = uniqueName();
  try {
    const p = RingProducer.open(req, { capacity: 4096 });
    RingProducer.open(resp, { capacity: 4096 }).close();  // size the reply ring
    const c = RingConsumer.open(resp);
    const w = new Worker(
      `const { workerData, parentPort } = require('node:worker_threads');
       const { RingProducer, RingConsumer } = require(workerData.RING);
       (async () => {
         const c = RingConsumer.open(workerData.req);
         const p = RingProducer.open(workerData.resp);
         parentPort.postMessage('ready');
         for (;;) {
           const m = await c.peekAsync();
           const v = await p.reserveAsync(m.length);
           v.set(m); c.release(); p.commit();
           if (m[0] === 255) break;
         }
         p.close(); c.close();
       })();`,
      { eval: true, workerData: { RING, req, resp } },
    );
    await new Promise((r) => w.once('message', r));
    let worst = 0;
    const N = 3000;
    for (let i = 0; i < N; i++) {
      const t0 = performance.now();
      const last = i === N - 1;
      const v = await p.reserveAsync(8);
      v.fill(last ? 255 : i & 0x7f);
      p.commit();
      const m = await c.peekAsync();
      assert.ok(m !== null);
      c.release();
      worst = Math.max(worst, performance.now() - t0);
    }
    await new Promise((r) => w.once('exit', r));
    assert.ok(worst < 150, `worst async round trip ${worst.toFixed(1)} ms (a lost wake costs a 250 ms slice)`);
    p.close();
    c.close();
  } finally {
    unlinkQuietly(req);
    unlinkQuietly(resp);
  }
});

test('C6: an async producer on a full ring is woken by each release', async () => {
  const name = uniqueName();
  try {
    const p = RingProducer.open(name, { capacity: 4096 });
    const w = new Worker(
      `const { workerData, parentPort } = require('node:worker_threads');
       const { RingConsumer } = require(workerData.RING);
       const c = RingConsumer.open(workerData.name);
       parentPort.postMessage('ready');
       let n = 0;
       for (;;) { const m = c.read(); n++; if (m[0] === 255) break; }
       parentPort.postMessage(n); c.close();`,
      { eval: true, workerData: { RING, name } },
    );
    await new Promise((r) => w.once('message', r));
    const N = 20000;
    let worst = 0;
    for (let i = 0; i < N; i++) {
      const t0 = performance.now();
      const v = await p.reserveAsync(200);
      v.fill(i === N - 1 ? 255 : 1);
      p.commit();
      worst = Math.max(worst, performance.now() - t0);
    }
    const got = await new Promise((r) => w.once('message', r));
    assert.strictEqual(got, N);
    assert.ok(worst < 150, `worst reserveAsync ${worst.toFixed(1)} ms`);
    p.close();
  } finally {
    unlinkQuietly(name);
  }
});

// ---- C9 (R19): a failed identity read is retried, not cached ----------------

test('C9: a thread whose first identity read hit EMFILE recovers once fds free up', { skip: process.platform === 'linux' ? false : 'Linux /proc identity' }, () => {
  const name = uniqueName();
  try {
    Mutex.open(name);
    const r = runNode(`
      const fs=require('node:fs');const {Worker}=require('node:worker_threads');
      const {Mutex}=require(${JSON.stringify(MUTEX)});
      Mutex.open(${JSON.stringify(name)});            // main thread: registry entry
      const w=new Worker(\`const {parentPort}=require('node:worker_threads');
        const {Mutex}=require(\${JSON.stringify(${JSON.stringify(MUTEX)})});
        let m=null;
        parentPort.on('message',(cmd)=>{
          if(cmd==='open'){ m=Mutex.open(\${JSON.stringify(${JSON.stringify(name)})}); parentPort.postMessage('opened'); return; }
          let res; try{ m.lock({timeoutMs:500}); m.unlock(); res='OK'; }catch(e){ res=e.code; }
          parentPort.postMessage(res);
        });\`,{eval:true});
      const ask=(c)=>new Promise(r=>{w.once('message',r);w.postMessage(c);});
      (async()=>{
        await ask('open');                              // reuse path: no identity read yet
        const fds=[]; try{ for(;;) fds.push(fs.openSync('/dev/null','r')); }catch{}
        const first=await ask('lock');                   // first identity read: EMFILE
        for(const fd of fds) fs.closeSync(fd);
        const second=await ask('lock');                  // must retry and succeed
        console.log(JSON.stringify({first,second})); process.exit(0);
      })();`);
    assert.strictEqual(r.signal, null, r.err);
    const res = lastJson(r.out);
    assert.strictEqual(res.second, 'OK', JSON.stringify(res));
    assert.ok(res.first === 'E_SYSTEM' || res.first === 'OK', JSON.stringify(res));
  } finally {
    unlinkQuietly(name);
  }
});

// ---- C10: close() disables the instance ---------------------------------------

test('C10: closed Mutex / RingProducer / RingConsumer instances throw E_CLOSED', () => {
  const mName = uniqueName();
  const rName = uniqueName();
  try {
    const m = Mutex.open(mName);
    m.lock();
    m.close();  // unlocks normally first
    assertThrowsCode(() => m.lock(), 'E_CLOSED');
    assertThrowsCode(() => m.unlock(), 'E_CLOSED');
    const m2 = Mutex.open(mName);
    assert.strictEqual(m2.lock({ timeoutMs: 500 }).ownerDied, false, 'close() is a normal unlock');
    m2.unlock();
    m2.close();

    const p = RingProducer.open(rName, { capacity: 4096 });
    const c = RingConsumer.open(rName);
    p.write(Buffer.from([7]));
    assert.ok(c.peek({ timeoutMs: 100 }) !== null);
    c.close();  // abandons the peek: redelivered (at-least-once)
    assertThrowsCode(() => c.peek({ timeoutMs: 0 }), 'E_CLOSED');
    assertThrowsCode(() => c.release(), 'E_CLOSED');
    const c2 = RingConsumer.open(rName);
    const again = c2.read({ timeoutMs: 200 });
    assert.ok(again !== null && again[0] === 7, 'replacement consumer gets the message');
    assertThrowsCode(() => c.read({ timeoutMs: 0 }), 'E_CLOSED');
    p.close();
    assertThrowsCode(() => p.write(Buffer.from([1])), 'E_CLOSED');
    c2.close();
  } finally {
    unlinkQuietly(mName);
    unlinkQuietly(rName);
  }
});

// ---- C11/C12: takeover after a crashed initializer --------------------------

function craftCrashedInit(name: string): void {
  // initState = bare 1 (legacy word, epoch 0) with initializerSlot -1: an
  // absent initializer, as a grow that died mid-flight would leave behind.
  const r = runNode(
    `const {open}=require(${JSON.stringify(CORE)});const s=open(${JSON.stringify(name)},{raw:true});
     const d=new DataView(s); d.setInt32(8,1,true); d.setInt32(12,-1,true); console.log('ok')`,
  );
  assert.strictEqual(r.out, 'ok', r.err);
}

test('C12: a takeover keeps the crashed segment\'s kind and larger geometry', { skip: LINUX ? false : 'POSIX' }, () => {
  const plain = uniqueName();
  const mx = uniqueName();
  try {
    runNode(`require(${JSON.stringify(CORE)}).open(${JSON.stringify(plain)}, 65536)`);
    craftCrashedInit(plain);
    const r1 = runNode(
      `const s=require(${JSON.stringify(CORE)}).open(${JSON.stringify(plain)},4096,{sizePolicy:'at-least'});console.log(s.byteLength)`,
    );
    assert.strictEqual(r1.out, '4096', r1.err);
    assert.strictEqual(stat(plain).dataBytes, 65536, 'prior geometry survives the takeover');
    assert.strictEqual(stat(plain).initState, 2);

    runNode(`require(${JSON.stringify(MUTEX)}).Mutex.open(${JSON.stringify(mx)})`);
    craftCrashedInit(mx);
    const r2 = runNode(
      `const s=require(${JSON.stringify(CORE)}).open(${JSON.stringify(mx)},${MUTEX_DATA_BYTES});console.log(s.byteLength)`,
    );
    assert.strictEqual(r2.out, String(MUTEX_DATA_BYTES), r2.err);
    assert.strictEqual(stat(mx).kind, 'mutex', 'a plain takeover keeps the mutex kind');
    const m = Mutex.open(mx);
    m.lock({ timeoutMs: 1000 });
    m.unlock();
    m.close();
  } finally {
    unlinkQuietly(plain);
    unlinkQuietly(mx);
  }
});

test('C11: an opener facing a constantly re-forged dead baton is bounded by initTimeoutMs', { skip: LINUX ? false : 'POSIX' }, () => {
  const name = uniqueName();
  try {
    runNode(`require(${JSON.stringify(CORE)}).open(${JSON.stringify(name)}, 4096)`);
    const deadPid = spawnSync(process.execPath, ['-e', '0']).pid ?? 999_999;
    // Row 63 = a dead participant; the forger keeps naming it as initializer.
    const forger = spawn(process.execPath, ['-e', `
      const {open}=require(${JSON.stringify(CORE)});const s=open(${JSON.stringify(name)},{raw:true});
      const d=new DataView(s); const row=32+63*32;
      d.setInt32(row, ${deadPid}, true); d.setInt32(row+24, 1, true);
      const end=Date.now()+3000;
      while(Date.now()<end){ d.setInt32(8, 1 | (63<<8) | (5<<20), true); }`], { stdio: 'ignore' });
    try {
      const t0 = Date.now();
      const r = runNode(
        `const v=require(${JSON.stringify(CORE)});try{v.open(${JSON.stringify(name)},4096,{initTimeoutMs:400});console.log('OPENED')}catch(e){console.log(e.code)}`,
      );
      const took = Date.now() - t0;
      assert.ok(r.out === 'OPENED' || r.out === 'E_INIT_TIMEOUT', r.out + r.err);
      assert.ok(took < 2500, `open is bounded by initTimeoutMs (took ${took} ms)`);
    } finally {
      forger.kill('SIGKILL');
    }
  } finally {
    unlinkQuietly(name);
  }
});

// ---- C13: a fresh role holder clears a stale parked flag ---------------------

test('C13: a new consumer clears the parked flag a crashed one left set', () => {
  const name = uniqueName();
  try {
    const p = RingProducer.open(name, { capacity: 4096 });
    const raw = new Int32Array(open(name, { kind: 'ring' }));
    Atomics.store(raw, 1, 1);  // CONSUMER_PARKED left set by a dead consumer
    const c = RingConsumer.open(name);
    assert.strictEqual(Atomics.load(raw, 1), 0);
    c.close();
    p.close();
  } finally {
    unlinkQuietly(name);
  }
});

// ---- C14: invalid ring timeouts are rejected ----------------------------------

test('C14: NaN / negative / >2^31 ring timeouts throw E_NAME_INVALID', async () => {
  const name = uniqueName();
  try {
    const p = RingProducer.open(name, { capacity: 4096 });
    const c = RingConsumer.open(name);
    for (const t of [NaN, -1, Infinity, 2 ** 31 + 1]) {
      assertThrowsCode(() => c.peek({ timeoutMs: t }), 'E_NAME_INVALID');
      assertThrowsCode(() => p.reserve(1, { timeoutMs: t }), 'E_NAME_INVALID');
      await assert.rejects(c.peekAsync({ timeoutMs: t }), (e: any) => e.code === 'E_NAME_INVALID');
      await assert.rejects(p.reserveAsync(1, { timeoutMs: t }), (e: any) => e.code === 'E_NAME_INVALID');
    }
    assert.strictEqual(c.peek({ timeoutMs: 0 }), null, '0 stays non-blocking');
    c.close();
    p.close();
  } finally {
    unlinkQuietly(name);
  }
});

// ---- C15: the attach row survives in whichever mapping lives longest ---------

test('C15: unlinkWhenUnused fires after the owner mapping detaches first', { skip: LINUX ? false : 'grow is POSIX-only' }, () => {
  const name = uniqueName();
  try {
    const r = runNode(`
      (async()=>{
        const {open}=require(${JSON.stringify(CORE)});const {stat}=require(${JSON.stringify(OPS)});
        let a=open(${JSON.stringify(name)},4096,{unlinkWhenUnused:true});
        let b=open(${JSON.stringify(name)},8192,{sizePolicy:'grow',unlinkWhenUnused:true});
        a=null; for(let i=0;i<10;i++){globalThis.gc(); await new Promise(r=>setImmediate(r));}
        const rowsWhileB=stat(${JSON.stringify(name)}).attachSlots.filter(s=>s.pid===process.pid).length;
        b=null; for(let i=0;i<10;i++){globalThis.gc(); await new Promise(r=>setImmediate(r));}
        let gone=false; try{ stat(${JSON.stringify(name)}); }catch(e){ gone=e.code==='E_NOT_FOUND'; }
        console.log(JSON.stringify({rowsWhileB, gone})); process.exit(0);
      })();`, ['--expose-gc']);
    assert.strictEqual(r.signal, null, r.err);
    assert.deepStrictEqual(lastJson(r.out), { rowsWhileB: 1, gone: true });
  } finally {
    unlinkQuietly(name);
  }
});

// ---- C24: one live producer per thread and mapping -----------------------------

test('C24: RingProducer.open on the same thread returns the live instance', () => {
  const name = uniqueName();
  try {
    const p1 = RingProducer.open(name, { capacity: 4096 });
    const p2 = RingProducer.open(name);
    assert.strictEqual(p1, p2, 'same instance, so reserve/commit state is shared');
    p1.reserve(4);
    assertThrowsCode(() => p2.reserve(4), 'E_RING_STATE');
    p1.commit();
    p1.close();
    const p3 = RingProducer.open(name);
    assert.notStrictEqual(p3, p1, 'a closed producer is not handed out again');
    p3.close();
  } finally {
    unlinkQuietly(name);
  }
});

// ---- post-verification nits ----------------------------------------------------

test('nit 1: an own slot whose gen would mint token 0 is re-published before locking', () => {
  const name = uniqueName();
  try {
    const m = Mutex.open(name);
    const v = new Int32Array(open(name, MUTEX_DATA_BYTES, { mode: 'join', kind: 'mutex' }));
    const id = m.identityWords();
    const base = SLOT_LAYOUT.SLOTS;  // slot 0: token = (0 << 16) | gen15
    Atomics.store(v, base + SLOT_LAYOUT.PID, id.pid);
    Atomics.store(v, base + SLOT_LAYOUT.TID, id.tid);
    Atomics.store(v, base + SLOT_LAYOUT.START_LO, id.start % 2 ** 32);
    Atomics.store(v, base + SLOT_LAYOUT.START_HI, Math.floor(id.start / 2 ** 32));
    Atomics.store(v, base + SLOT_LAYOUT.NS_LO, id.ns % 2 ** 32);
    Atomics.store(v, base + SLOT_LAYOUT.NS_HI, Math.floor(id.ns / 2 ** 32));
    Atomics.store(v, base + SLOT_LAYOUT.GEN, 0x8000);  // gen15 == 0 -> token 0
    Atomics.store(v, base + SLOT_LAYOUT.STATE, SLOT_LAYOUT.STATE_ACTIVE);
    m.lock();
    assert.notStrictEqual(Atomics.load(v, 0), 0, 'a held lock word is never the free value');
    m.unlock();
    m.close();
  } finally {
    unlinkQuietly(name);
  }
});

test('nit 2: a role claim waits out a live participant caught mid-claim', { skip: POSIX ? false : 'POSIX' }, async () => {
  const CONSUMER_SLOT_STATE = 36 + 1 * 8 + 7;  // kRingSlotsWordOffset + slot 1 + state word
  for (const settles of [true, false]) {
    const name = uniqueName();
    try {
      const p = RingProducer.open(name, { capacity: 4096 });
      // A live process holds the consumer slot's Claiming word; it settles
      // (abandons) after 30 ms, or never.
      const child = spawn(process.execPath, ['-e', `
        const {open}=require(${JSON.stringify(CORE)});
        const v=new Int32Array(open(${JSON.stringify(name)},{kind:'ring'}));
        Atomics.store(v, ${CONSUMER_SLOT_STATE}, (process.pid << 2) | 2);
        console.log('set');
        if (${settles}) setTimeout(() => Atomics.store(v, ${CONSUMER_SLOT_STATE}, 0), 30);
        setTimeout(() => {}, 3000);`], { stdio: ['ignore', 'pipe', 'ignore'] });
      await new Promise<void>((r) => child.stdout.once('data', () => r()));
      const t0 = performance.now();
      try {
        if (settles) {
          const c = RingConsumer.open(name);  // used to fail fast with E_ROLE_TAKEN
          c.close();
        } else {
          assertThrowsCode(() => RingConsumer.open(name), 'E_ROLE_TAKEN');
          const took = performance.now() - t0;
          assert.ok(took >= 90 && took < 1000, `bounded settle window (took ${took.toFixed(0)} ms)`);
        }
      } finally {
        child.kill('SIGKILL');
        await new Promise((r) => child.once('exit', r));
      }
      p.close();
    } finally {
      unlinkQuietly(name);
    }
  }
});
