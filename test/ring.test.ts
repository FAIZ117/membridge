// ring.test.ts — §13 RingBuffer tests: cross-process checksummed stream across
// wrap/SKIP boundaries, full/empty blocking, E_MESSAGE_TOO_LARGE, producer
// killed mid-reserve (nothing visible), consumer killed mid-peek (message
// redelivered — at-least-once), E_ROLE_TAKEN and role takeover after death.

import { test } from 'node:test';
import { fork } from 'node:child_process';
import { Worker } from 'node:worker_threads';
import { RingProducer, RingConsumer, RING_HEADER_BYTES } from '../src/ringbuffer';
import { open } from '../src/core';
import { assert, assertThrowsCode, uniqueName, holdForTest, unlinkQuietly } from './helpers';

const PKG = require.resolve('../src/core');
const ROLE = process.env.SHM_BRIDGE_TEST_ROLE;
const ARG = process.env.SHM_BRIDGE_TEST_ARG ?? '';

function checksum(buf: Uint8Array): number {
  let h = 2166136261;
  for (const b of buf) {
    h ^= b;
    h = Math.imul(h, 16777619);
  }
  return h >>> 0;
}

function makeMessage(i: number, n: number): Buffer {
  const buf = Buffer.alloc(n);
  for (let j = 0; j < n; j++) buf[j] = (i * 31 + j * 7) & 0x7f;
  return buf;
}

function childMain(role: string, arg: string): void {
  const { open } = require(PKG);
  const rb = require(PKG.replace(/core\.js$/, 'ringbuffer.js')) as typeof import('../src/ringbuffer');
  const [name = '', a = '0', b = ''] = arg.split('|');

  if (role === 'rb-producer') {
    const p = rb.RingProducer.open(name, { capacity: 4096 });
    const count = Number(a);
    for (let i = 0; i < count; i++) {
      const n = 1 + (i * 37) % 1500; // forces wrap + SKIP on a 4 KiB ring
      p.write(makeMessage(i, n));
    }
    process.send!({ type: 'done', count });
    process.exit(0);
  }
  if (role === 'rb-consumer') {
    const c = rb.RingConsumer.open(name);
    const count = Number(a);
    for (let i = 0; i < count; i++) {
      const n = 1 + (i * 37) % 1500;
      const msg = c.read({ timeoutMs: 5000 });
      if (msg === null || msg.length !== n) process.exit(1);
      if (checksum(msg) !== checksum(makeMessage(i, n))) process.exit(2);
    }
    process.send!({ type: 'done', count });
    process.exit(0);
  }
  if (role === 'rb-producer-hold') {
    // reserve mid-way and die before commit: nothing may become visible
    const p = rb.RingProducer.open(name, { capacity: 4096 });
    p.write(makeMessage(0, 100)); // one complete message
    p.reserve(1000);
    process.send!({ type: 'reserved' });
    setTimeout(() => process.exit(3), 30000);
    return;
  }
  if (role === 'rb-consumer-hold') {
    const c = rb.RingConsumer.open(name);
    const view = c.peek({ timeoutMs: 5000 });
    if (view === null) process.exit(1);
    process.send!({ type: 'peeked', len: view.length });
    setTimeout(() => process.exit(3), 30000); // dies WITHOUT release
    return;
  }
  throw new Error(`unknown role ${role}`);
}

if (ROLE !== undefined) {
  try {
    childMain(ROLE, ARG);
  } catch {
    process.exit(2);
  }
} else {
  registerTests();
}

function forkChild(role: string, arg: string): ReturnType<typeof fork> {
  return fork(__filename, {
    env: { ...process.env, SHM_BRIDGE_TEST_ROLE: role, SHM_BRIDGE_TEST_ARG: arg },
    stdio: 'inherit',
  });
}

function waitFor(child: ReturnType<typeof fork>, type: string, timeoutMs = 15000): Promise<any> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`no '${type}' from child`)), timeoutMs);
    child.on('message', function h(m: any) {
      if (m.type === type) {
        clearTimeout(timer);
        child.removeListener('message', h);
        resolve(m);
      }
    });
    child.on('exit', (code) => {
      clearTimeout(timer);
      reject(new Error(`child exited ${code} waiting for '${type}'`));
    });
  });
}

function registerTests(): void {
  test('cross-process stream: checksummed messages across wrap/SKIP boundaries', async (t) => {
    const name = uniqueName();
    holdForTest(t, open(name, RING_HEADER_BYTES + 4096, { kind: 'ring', mode: 'create' })); // no role claim
    const count = 300;
    const consumer = forkChild('rb-consumer', `${name}|${count}`);
    // let the consumer take the role, then produce
    await new Promise((r) => setTimeout(r, 150));
    const producer = forkChild('rb-producer', `${name}|${count}`);
    try {
      const done = await waitFor(consumer, 'done');
      assert.strictEqual(done.count, count);
      await waitFor(producer, 'done');
    } finally {
      consumer.kill('SIGKILL');
      producer.kill('SIGKILL');
      unlinkQuietly(name);
    }
  });

  test('full/empty blocking semantics', async (t) => {
    const name = uniqueName();
    try {
      holdForTest(t, open(name, RING_HEADER_BYTES + 4096, { kind: 'ring', mode: 'create' })); // no roles claimed
      const c = RingConsumer.open(name);
      // empty: consumer blocks until a late write. The write comes from a
      // worker producer (the segment was created without any role claim,
      // because c.read blocks the main JS thread).
      const t0 = Date.now();
      const w = new Worker(
        `const { workerData } = require('worker_threads');
         const { RingProducer } = require(workerData.pkg.replace(/core\\.js$/, 'ringbuffer.js'));
         const p = RingProducer.open(workerData.name);
         setTimeout(() => { p.write(Buffer.from('late message')); process.exit(0); }, 300);`,
        { eval: true, workerData: { name, pkg: PKG } },
      );
      const msg = c.read({ timeoutMs: 5000 });
      await w.terminate();
      assert.ok(msg !== null && Buffer.from(msg).toString() === 'late message');
      assert.ok(Date.now() - t0 >= 250, 'consumer actually waited for data');

      // the worker producer is dead: the parent takes over the producer role
      const p = RingProducer.open(name, { capacity: 4096 });
      p.write(Buffer.alloc(2000));
      p.write(Buffer.alloc(2000)); // ring is now full (2 * ~2008 <= 4096?)
      let full = false;
      for (let i = 0; i < 10 && !full; i++) {
        try {
          p.reserve(16, { timeoutMs: 200 });
          p.commit(); // keep filling
          p.write(Buffer.alloc(16));
        } catch {
          full = true;
        }
      }
      assert.ok(full, 'ring eventually reports full via E_TIMEOUT');
      // drain: consumer releases; producer can write again
      while (c.read({ timeoutMs: 50 }) !== null) {
        /* drain */
      }
      p.write(Buffer.from('after drain'));
      assert.strictEqual(Buffer.from(c.read({ timeoutMs: 1000 })!).toString(), 'after drain');
    } finally {
      unlinkQuietly(name);
    }
  });

  test('E_MESSAGE_TOO_LARGE and E_RING_STATE', () => {
    const name = uniqueName();
    try {
      const p = RingProducer.open(name, { capacity: 4096 }); // maxMessage = 2040
      const c = RingConsumer.open(name);
      assert.throws(() => p.reserve(2041), (e: any) => e.code === 'E_MESSAGE_TOO_LARGE');
      assert.throws(() => p.commit(), (e: any) => e.code === 'E_RING_STATE');
      const v = p.reserve(10);
      assert.throws(() => p.reserve(10), (e: any) => e.code === 'E_RING_STATE');
      assert.throws(() => c.release(), (e: any) => e.code === 'E_RING_STATE');
      p.commit();
      assert.strictEqual((c.peek({ timeoutMs: 1000 }) as Uint8Array).length, 10);
      c.release();
    } finally {
      unlinkQuietly(name);
    }
  });

  test('producer killed mid-reserve: nothing becomes visible', async (t) => {
    const name = uniqueName();
    const child = forkChild('rb-producer-hold', name);
    try {
      RingProducer.open(name, { capacity: 4096 }); // create + take role first
      // child re-opens after our role claim -> E_ROLE_TAKEN; so let the child
      // create instead: restart with a fresh name and no parent-side claim.
      unlinkQuietly(name);
    } finally {
      child.kill('SIGKILL');
      unlinkQuietly(name);
    }
    // Correct setup: parent only creates the segment via core.open with the
    // ring size, no role claim; the child becomes the producer.
    const name2 = uniqueName();
    const child2 = forkChild('rb-producer-hold', name2);
    try {
      holdForTest(t, open(name2, RING_HEADER_BYTES + 4096, { kind: 'ring', mode: 'create' }));
      await waitFor(child2, 'reserved');
      await new Promise((r) => setTimeout(r, 100));
      child2.kill('SIGKILL');
      await new Promise((r) => child2.on('exit', r));
      // a fresh consumer must see exactly the one committed message, nothing else
      const c = RingConsumer.open(name2);
      const first = c.read({ timeoutMs: 1000 });
      assert.ok(first !== null && first.length === 100, 'committed message visible');
      const nothing = c.read({ timeoutMs: 300 });
      assert.strictEqual(nothing, null, 'mid-reserve bytes never became visible');
    } finally {
      unlinkQuietly(name2);
    }
  });

  test('consumer killed mid-peek: message redelivered (at-least-once)', async (t) => {
    const name = uniqueName();
    try {
      holdForTest(t, open(name, RING_HEADER_BYTES + 4096, { kind: 'ring', mode: 'create' }));
      const child = forkChild('rb-consumer-hold', name);
      // the child needs a producer first: write via a temporary producer role
      // that exits (role becomes claimable again after death)
      const tmpProducer = forkChild('rb-producer', `${name}|1`);
      await waitFor(tmpProducer, 'done');
      await waitFor(child, 'peeked');
      await new Promise((r) => setTimeout(r, 100));
      child.kill('SIGKILL');
      await new Promise((r) => child.on('exit', r));
      // the message was peeked but never released -> redelivered
      const c = RingConsumer.open(name);
      const again = c.read({ timeoutMs: 2000 });
      assert.ok(again !== null, 'message redelivered after consumer death');
      assert.strictEqual(checksum(again), checksum(makeMessage(0, 1 + (0 * 37) % 1500)));
      assert.strictEqual(c.read({ timeoutMs: 200 }), null, 'and only once after release');
    } finally {
      unlinkQuietly(name);
    }
  });

  test('role claims: E_ROLE_TAKEN while alive, takeover after death', async () => {
    const name = uniqueName();
    try {
      const p1 = RingProducer.open(name, { capacity: 4096 });
      // Same-thread re-claim returns the existing role (review F23 fix):
      const p2 = RingProducer.open(name, { capacity: 4096 });
      p2.write(Buffer.from('same-thread re-claim works'));
      // ...but a DIFFERENT thread still gets E_ROLE_TAKEN while this one lives:
      const w = new Worker(
        `const { workerData } = require('worker_threads');
         const { RingProducer } = require(workerData.pkg);
         try {
           RingProducer.open(workerData.name);
           process.exitCode = 0; // BAD: role was taken
         } catch (e) {
           process.exitCode = e.code === 'E_ROLE_TAKEN' ? 11 : 12;
         }`,
        { eval: true, workerData: { name, pkg: require.resolve('../src/ringbuffer') } },
      );
      const exitCode = await new Promise<number>((res) => w.on('exit', (c) => res(c ?? -1)));
      assert.strictEqual(exitCode, 11, 'cross-thread producer sees E_ROLE_TAKEN');
      // a consumer can coexist with the producer (SPSC is per-role)
      const c1 = RingConsumer.open(name);
      assert.strictEqual(Buffer.from(c1.read({ timeoutMs: 1000 })!).toString(),
        'same-thread re-claim works');
      p1.write(Buffer.from('x'));
      assert.strictEqual(Buffer.from(c1.read({ timeoutMs: 1000 })!).toString(), 'x');
      // R15b: a SECOND consumer instance on this thread is refused — two live
      // consumers could both peek the same message and double-process it.
      assertThrowsCode(() => RingConsumer.open(name), 'E_ROLE_TAKEN');
      // close() releases the role, so a replacement can open immediately
      c1.close();
      const c2 = RingConsumer.open(name);
      assert.strictEqual(c2.read({ timeoutMs: 200 }), null, 'role re-claimed after close');
      c2.close();
      // role takeover after death is covered by the mid-reserve/mid-peek tests
    } finally {
      unlinkQuietly(name);
    }
  });

  test('capacity above the default segment cap -> E_SIZE_INVALID', () => {
    const name = uniqueName();
    try {
      assert.throws(
        () => RingProducer.open(name, { capacity: 512 * 1024 * 1024 }),
        (e: any) => e.code === 'E_SIZE_INVALID',
      );
      assert.throws(
        () => RingProducer.open(name, { capacity: 3000 }), // not a power of two / too small
        (e: any) => e.code === 'E_SIZE_INVALID',
      );
    } finally {
      unlinkQuietly(name);
    }
  });
}
