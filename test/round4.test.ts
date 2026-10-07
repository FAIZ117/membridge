// round4.test.ts — regressions for the round-4 review
// (docs/review/2026-10-07T1710-round4-review.md). Each test names the finding
// it pins. Crash-prone scenarios (truncated segments) run in child processes.

import { test } from 'node:test';
import fs from 'node:fs';
import { spawn, spawnSync } from 'node:child_process';
import { Worker } from 'node:worker_threads';
import { open } from '../src/core';
import { Mutex, MUTEX_DATA_BYTES, SLOT_LAYOUT } from '../src/mutex';
import { RingProducer, RingConsumer } from '../src/ringbuffer';
import { list, reap, stat } from '../src/ops';
import { MembridgeError } from '../src/errors';
import { assert, assertThrowsCode, uniqueName, unlinkQuietly } from './helpers';

const CORE = require.resolve('../src/core');
const MUTEX = require.resolve('../src/mutex');
const SYNC = require.resolve('../src/sync');
const LINUX = process.platform === 'linux';
const POSIX = process.platform !== 'win32';
const shmPath = (name: string): string => `/dev/shm${name}`;

function runNode(code: string, flags: string[] = [], timeout = 60_000): { status: number | null; signal: string | null; out: string; err: string } {
  const r = spawnSync(process.execPath, [...flags, '-e', code], { encoding: 'utf8', timeout });
  return { status: r.status, signal: r.signal, out: (r.stdout ?? '').trim(), err: (r.stderr ?? '').trim() };
}

function lastJson(out: string): any {
  const lines = out.split('\n').filter((l) => l.startsWith('{'));
  return JSON.parse(lines[lines.length - 1]!);
}

/** Our pid-namespace tag, folded exactly like cc/mutex.cc NsTag(). */
function nsTag(ns: number): number {
  if (!(ns > 0)) return 0;
  let x = BigInt(ns);
  x ^= x >> 32n;
  x ^= x >> 16n;
  x ^= x >> 8n;
  const t = Number(x & 0xffn);
  return t === 0 ? 1 : t;
}

function claimingWord(tag: number, pid: number): number {
  return (((tag & 0xff) << 24) | ((pid & 0x3fffff) << 2) | 2) | 0;
}

// ---- E4-1: close() during a pending lockAsync ---------------------------------

test('E4-1: close() while lockAsync is pending rejects with E_CLOSED and never acquires', async () => {
  const name = uniqueName();
  try {
    Mutex.open(name);
    const holder = spawn(process.execPath, ['-e', `
      const {Mutex}=require(${JSON.stringify(MUTEX)});const m=Mutex.open(${JSON.stringify(name)});
      m.lock(); console.log('locked'); setTimeout(()=>{ m.unlock(); setTimeout(()=>process.exit(0), 50); }, 300);`],
      { stdio: ['ignore', 'pipe', 'ignore'] });
    const holderExit = new Promise((r) => holder.once('exit', r));
    await new Promise<void>((r) => holder.stdout.once('data', () => r()));
    const m = Mutex.open(name);
    const pending = m.lockAsync();
    await new Promise((r) => setTimeout(r, 50));
    m.close();
    await assert.rejects(pending, (e: any) => e instanceof MembridgeError && e.code === 'E_CLOSED');
    await holderExit;
    // Not wedged, not double-held: another process acquires cleanly.
    const after = runNode(
      `const {Mutex}=require(${JSON.stringify(MUTEX)});const m=Mutex.open(${JSON.stringify(name)});` +
        `const r=m.lock({timeoutMs:1000});console.log(JSON.stringify(r));m.unlock()`,
    );
    assert.deepStrictEqual(lastJson(after.out), { ownerDied: false }, after.err);
  } finally {
    unlinkQuietly(name);
  }
});

// ---- E4-2: concurrent reserveAsync on one producer ----------------------------

test('E4-2: concurrent reserveAsync calls never share a region', async () => {
  for (let round = 0; round < 5; round++) {
    const name = uniqueName();
    try {
      const p = RingProducer.open(name, { capacity: 4096 });
      const c = RingConsumer.open(name);
      // Fill the ring with 1-byte fillers so both writers must park.
      let fillers = 0;
      for (;;) {
        try { p.write(Uint8Array.of(0x11), { timeoutMs: 0 }); fillers++; } catch { break; }
      }
      const told = new Map<string, boolean>();
      // Each writer awaits between reserve and commit (e.g. for data) — the
      // window in which another writer's wake used to overwrite its region.
      const writer = async (tag: string, len: number, byte: number) => {
        let v: Uint8Array;
        try {
          v = await p.reserveAsync(len, { timeoutMs: 2000 });
        } catch (e: any) {
          assert.strictEqual(e.code, 'E_RING_STATE', `${tag}: ${e.code}`);
          told.set(tag, false);
          return;
        }
        v.fill(byte);
        await new Promise((r) => setImmediate(r));
        p.commit();
        told.set(tag, true);
      };
      const a = writer('A', 40, 0xa1);
      const b = writer('B', 200, 0xb2);
      await new Promise((r) => setTimeout(r, 20));
      for (let i = 0; i < 40; i++) assert.ok(c.read({ timeoutMs: 100 }) !== null);
      await Promise.all([a, b]);
      // Drain and check every delivered message is intact and was promised.
      const got: Array<{ len: number; byte: number }> = [];
      for (;;) {
        const msg = c.read({ timeoutMs: 0 });
        if (msg === null) break;
        if (msg.length === 1 && msg[0] === 0x11) continue;
        assert.ok(msg.every((x) => x === msg[0]), `round ${round}: torn message`);
        got.push({ len: msg.length, byte: msg[0]! });
      }
      const expected = [
        ...(told.get('A') ? [{ len: 40, byte: 0xa1 }] : []),
        ...(told.get('B') ? [{ len: 200, byte: 0xb2 }] : []),
      ];
      assert.deepStrictEqual(got.sort((x, y) => x.len - y.len), expected, `round ${round}`);
      assert.ok(fillers > 0);
      c.close();
      p.close();
    } finally {
      unlinkQuietly(name);
    }
  }
});

// ---- E4-3: tryLock reports ownerDied ------------------------------------------

test('E4-3: tryLock reports a holder\'s death and does not leave it for the next lock()', async () => {
  const name = uniqueName();
  try {
    const m = Mutex.open(name);
    const w = new Worker(`
      const { workerData, parentPort } = require('node:worker_threads');
      const { Mutex } = require(workerData.MUTEX);
      Mutex.open(workerData.name).lock();
      parentPort.postMessage('locked');
      setInterval(() => {}, 1 << 30);`, { eval: true, workerData: { MUTEX, name } });
    await new Promise((r) => w.once('message', r));
    await w.terminate();  // env-cleanup hook releases as OWNER_DIED
    const r = m.tryLock();
    assert.ok(r !== null, 'free after the holder died');
    assert.strictEqual(r.ownerDied, true, 'tryLock reports the death');
    m.unlock();
    assert.strictEqual(m.lock().ownerDied, false, 'the next holder is not told again');
    m.unlock();
    assert.strictEqual(typeof m.tryLock(), 'object');
    m.unlock();
    m.close();
  } finally {
    unlinkQuietly(name);
  }
});

// ---- E4-4: creator reserving vs a racing joiner --------------------------------

test('E4-4: a joiner never takes over a live creator that is reserving', { skip: LINUX ? false : 'Linux fallocate' }, async () => {
  // The race needs the joiner's 50 ms grace to expire while the creator is
  // still in posix_fallocate: pin creator, joiner and a CPU hog to one core
  // (reproduced 5/5 at fdc0078 this way; without taskset it is probabilistic).
  const BIG = 192 * 1024 * 1024;
  const pinned = spawnSync('taskset', ['--version']).status === 0;
  const cpu = String(Math.max(0, require('node:os').cpus().length - 1));
  const pin = (args: string[]): [string, string[]] =>
    pinned ? ['taskset', ['-c', cpu, process.execPath, ...args]] : [process.execPath, args];
  for (let round = 0; round < 3; round++) {
    const name = uniqueName();
    try {
      const hog = spawn(...pin(['-e', 'const e=Date.now()+4000;while(Date.now()<e){}']), { stdio: 'ignore' });
      const joiner = spawn(...pin(['-e', `
        const {open}=require(${JSON.stringify(CORE)});const fs=require('node:fs');
        const p=${JSON.stringify(shmPath(name))};
        const end=Date.now()+5000;
        while(!fs.existsSync(p) && Date.now()<end){}
        let res; try{ res=open(${JSON.stringify(name)},4096,{sizePolicy:'at-least'}).byteLength; }
        catch(e){ res=e.code; }
        console.log(JSON.stringify({res}));`]), { stdio: ['ignore', 'pipe', 'ignore'] });
      let jout = '';
      joiner.stdout.on('data', (d) => { jout += d; });
      const jexit = new Promise((r) => joiner.once('exit', r));
      await new Promise((r) => setTimeout(r, 150));
      const [cmd, args] = pin(['-e',
        `const {open}=require(${JSON.stringify(CORE)});` +
          `try{const s=open(${JSON.stringify(name)},${BIG},{mode:'create'});console.log(JSON.stringify({ok:s.byteLength}))}` +
          `catch(e){console.log(JSON.stringify({err:e.code,msg:e.message}))}`]);
      const creator = spawnSync(cmd, args, { encoding: 'utf8', timeout: 60_000 });
      await jexit;
      hog.kill();
      assert.deepStrictEqual(lastJson(creator.stdout), { ok: BIG }, `round ${round}: ${creator.stdout}`);
      assert.strictEqual(stat(name).dataBytes, BIG, `round ${round}: header keeps the creator's size`);
      const j = lastJson(jout);
      assert.ok(j.res === 4096 || j.res === 'E_NOT_FOUND' || j.res === 'E_INIT_TIMEOUT', JSON.stringify(j));
    } finally {
      unlinkQuietly(name);
    }
  }
});

test('E4-4: a takeover of a dead creator\'s sized object keeps the file\'s size', { skip: LINUX ? false : 'Linux /dev/shm' }, () => {
  const name = uniqueName();
  try {
    // A creator that died right after its ftruncate: sized, zero header.
    fs.writeFileSync(shmPath(name), Buffer.alloc(4096 + 65536));
    const r = runNode(
      `const {open}=require(${JSON.stringify(CORE)});const s=open(${JSON.stringify(name)},4096,{sizePolicy:'at-least'});console.log(s.byteLength)`,
    );
    assert.strictEqual(r.out, '4096', r.err);
    assert.strictEqual(stat(name).dataBytes, 65536, 'header covers the object, not the joiner request');
  } finally {
    unlinkQuietly(name);
  }
});

// ---- S4-1: one truncated segment must not freeze other async waits ------------

test('S4-1: truncating one segment does not freeze async waits on others', { skip: LINUX ? false : 'Linux futex_waitv' }, () => {
  const a = uniqueName();
  const b = uniqueName();
  try {
    const r = runNode(`
      const {open}=require(${JSON.stringify(CORE)});const {waitAsync}=require(${JSON.stringify(SYNC)});
      const fs=require('node:fs');
      const va=new Int32Array(open(${JSON.stringify(a)},65536)), vb=new Int32Array(open(${JSON.stringify(b)},4096));
      const t0=Date.now();
      const pa=waitAsync(va, 4096, 0, 1500).then(x=>['A',x,Date.now()-t0]);
      const pb=waitAsync(vb, 0, 0, 300).then(x=>['B',x,Date.now()-t0]);
      setTimeout(()=>fs.truncateSync(${JSON.stringify(shmPath(a))}, 4096), 50);
      Promise.all([pa,pb]).then(rs=>{console.log(JSON.stringify(rs));process.exit(0)});
      setTimeout(()=>{console.log(JSON.stringify({hung:true}));process.exit(0)}, 4000);`);
    assert.strictEqual(r.signal, null, r.err);
    const res = JSON.parse(r.out.split('\n').pop()!);
    assert.ok(Array.isArray(res), `hung: ${r.out}`);
    const [ra, rb] = res;
    assert.strictEqual(rb[1], 'timed-out');
    assert.ok(rb[2] < 1000, `unrelated wait timed out at ${rb[2]} ms`);
    assert.ok(ra[2] < 2500, `faulting wait settled at ${ra[2]} ms`);
  } finally {
    unlinkQuietly(a);
    unlinkQuietly(b);
  }
});

// ---- S4-2 / S4-5 / S4-7: hostile /dev/shm entries ------------------------------

test('S4-2/S4-5/S4-7: FIFOs and symlinks in /dev/shm neither hang nor abort ops, and leak no fds', { skip: LINUX ? false : 'Linux /dev/shm' }, () => {
  const fifo = uniqueName();
  const link = uniqueName();
  const seg = uniqueName();
  try {
    spawnSync('mkfifo', [shmPath(fifo)]);
    fs.symlinkSync('/etc/hostname', shmPath(link));
    open(seg, 4096);
    const r = runNode(`
      const {open}=require(${JSON.stringify(CORE)});const ops=require(${JSON.stringify(require.resolve('../src/ops'))});
      const fs=require('node:fs');
      const fds=()=>fs.readdirSync('/proc/self/fd').length;
      const out={};
      const t0=Date.now();
      out.listed=ops.list().includes(${JSON.stringify(seg)});
      out.listHasFifo=ops.list().includes(${JSON.stringify(fifo)});
      try{ops.stat(${JSON.stringify(fifo)});out.statFifo='ok'}catch(e){out.statFifo=e.code}
      try{ops.stat(${JSON.stringify(link)});out.statLink='ok'}catch(e){out.statLink=e.code}
      try{ops.reap({dryRun:true});out.reap='ok'}catch(e){out.reap=e.code}
      const before=fds();
      for(let i=0;i<5;i++){ try{open(${JSON.stringify(fifo)},4096)}catch(e){out.openFifo=e.code} }
      out.fdDelta=fds()-before;
      out.ms=Date.now()-t0;
      console.log(JSON.stringify(out));`, [], 10_000);
    assert.strictEqual(r.signal, null, `hung or crashed: ${r.err}`);
    const o = lastJson(r.out);
    assert.deepStrictEqual(
      { listed: o.listed, listHasFifo: o.listHasFifo, statFifo: o.statFifo, statLink: o.statLink, reap: o.reap, openFifo: o.openFifo, fdDelta: o.fdDelta },
      { listed: true, listHasFifo: false, statFifo: 'E_INCOMPATIBLE', statLink: 'E_INCOMPATIBLE', reap: 'ok', openFifo: 'E_INCOMPATIBLE', fdDelta: 0 },
    );
    assert.ok(o.ms < 3000, `ops took ${o.ms} ms`);
  } finally {
    for (const n of [fifo, link]) { try { fs.unlinkSync(shmPath(n)); } catch { /* gone */ } }
    unlinkQuietly(seg);
  }
});

// ---- S4-3 / E4-6: Claiming words are judged only within our pid namespace ------

test('S4-3: a foreign-namespace Claiming word is never recovered; our own dead claimer is', { skip: LINUX ? false : 'Linux pid namespaces' }, () => {
  for (const foreign of [true, false]) {
    const name = uniqueName();
    try {
      const m = Mutex.open(name);
      const v = new Int32Array(open(name, MUTEX_DATA_BYTES, { mode: 'join', kind: 'mutex' }));
      const ours = nsTag(m.identityWords().ns);
      const tag = foreign ? ((ours % 255) + 1) : ours;  // any other non-zero tag
      const deadPid = spawnSync(process.execPath, ['-e', '0']).pid ?? 999_999;
      for (let s = 0; s < SLOT_LAYOUT.SLOT_COUNT; s++) {
        const base = SLOT_LAYOUT.SLOTS + s * SLOT_LAYOUT.SLOT_WORDS;
        Atomics.store(v, base + SLOT_LAYOUT.GEN, s + 1);
        Atomics.store(v, base + SLOT_LAYOUT.STATE, claimingWord(tag, deadPid));
      }
      if (foreign) {
        assertThrowsCode(() => m.lock({ timeoutMs: 200 }), 'E_TIMEOUT');
      } else {
        m.lock({ timeoutMs: 1000 });
        m.unlock();
      }
      m.close();
    } finally {
      unlinkQuietly(name);
    }
  }
});

// ---- P4-4: the settle window does not stall unrelated claims -------------------

test('P4-4: a role claim settling on a stuck participant does not block other claims', { skip: LINUX ? false : 'Linux' }, async () => {
  const ring = uniqueName();
  const mx = uniqueName();
  try {
    const p = RingProducer.open(ring, { capacity: 4096 });
    Mutex.open(mx);
    const tag = nsTag(Mutex.open(mx).identityWords().ns);
    // A live process sits in the consumer slot's Claiming state forever.
    const stuck = spawn(process.execPath, ['-e', `
      const {open}=require(${JSON.stringify(CORE)});
      const v=new Int32Array(open(${JSON.stringify(ring)},{kind:'ring'}));
      Atomics.store(v, 36 + 8 + 7, ((${tag} & 0xff) << 24) | ((process.pid & 0x3fffff) << 2) | 2);
      console.log('set'); setTimeout(()=>{}, 5000);`], { stdio: ['ignore', 'pipe', 'ignore'] });
    const stuckExit = new Promise((r) => stuck.once('exit', r));
    await new Promise<void>((r) => stuck.stdout.once('data', () => r()));
    const go = new Int32Array(new SharedArrayBuffer(4));
    const w = new Worker(`
      const { workerData, parentPort } = require('node:worker_threads');
      const { Mutex } = require(workerData.MUTEX);
      while (Atomics.load(workerData.go, 0) === 0) {}
      const t0 = performance.now();
      const m = Mutex.open(workerData.mx); m.lock(); m.unlock(); m.close();
      parentPort.postMessage(performance.now() - t0);`, { eval: true, workerData: { MUTEX, mx, go } });
    const claimMs = new Promise<number>((r) => w.once('message', r));
    Atomics.store(go, 0, 1);
    assertThrowsCode(() => RingConsumer.open(ring), 'E_ROLE_TAKEN');  // ~100 ms settle window
    const ms = await claimMs;
    assert.ok(ms < 50, `unrelated claim took ${ms.toFixed(1)} ms during the settle window`);
    stuck.kill('SIGKILL');
    await stuckExit;
    p.close();
  } finally {
    unlinkQuietly(ring);
    unlinkQuietly(mx);
  }
});

// ---- E4-8: unlinkWhenUnused on the reuse path ----------------------------------

test('E4-8: unlinkWhenUnused requested on a reused mapping still unlinks', { skip: POSIX ? false : 'POSIX' }, () => {
  const name = uniqueName();
  try {
    const r = runNode(`
      (async()=>{
        const {open}=require(${JSON.stringify(CORE)});const fs=require('node:fs');
        let a=open(${JSON.stringify(name)},4096);
        let b=open(${JSON.stringify(name)},4096,{unlinkWhenUnused:true});
        a=null;b=null;
        for(let i=0;i<15;i++){globalThis.gc(); await new Promise(r=>setImmediate(r));}
        console.log(JSON.stringify({gone:!fs.existsSync(${JSON.stringify(shmPath(name))})}));process.exit(0);
      })();`, ['--expose-gc']);
    assert.deepStrictEqual(lastJson(r.out), { gone: true }, r.err);
  } finally {
    unlinkQuietly(name);
  }
});

// ---- E4-10: a raw open does not displace the typed registry entry ---------------

test('E4-10: a raw open of a ring does not create a second producer on this thread', () => {
  const name = uniqueName();
  try {
    const p1 = RingProducer.open(name, { capacity: 4096 });
    open(name, { raw: true });
    const p2 = RingProducer.open(name);
    assert.strictEqual(p1, p2);
    p1.close();
  } finally {
    unlinkQuietly(name);
  }
});

// ---- S4-14: test hooks are inert in production -----------------------------------

test('S4-14: the fault-injection hook is disabled without MEMBRIDGE_TEST_HOOKS=1', () => {
  const r = runNode(
    `try{require(${JSON.stringify(require.resolve('../src/native'))}).nativeOrThrow().debugFailAfterGrow();console.log('ENABLED')}catch(e){console.log(e.code)}`,
  );
  assert.strictEqual(r.out, 'E_UNSUPPORTED');
  void list;
  void reap;
});
