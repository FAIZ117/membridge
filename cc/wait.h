// wait.h — §6 cross-process wait/notify on i32 words of a membridge segment.
// Waitable words are i32 only (guardrail 4). Linux: raw shared futexes
// (FUTEX_WAIT/FUTEX_WAKE without FUTEX_PRIVATE_FLAG — F14); async waits are
// multiplexed on one membridge-owned thread via futex_waitv (kernel >= 5.16),
// otherwise one thread per wait, capped. macOS/Windows compile behind guards
// (polling / named semaphores per U1/U3).

#ifndef MEMBRIDGE_WAIT_H_
#define MEMBRIDGE_WAIT_H_

#include "membridge.h"

#include <memory>

namespace membridge {

enum class WaitResult { kOk, kNotEqual, kTimedOut };

// Synchronous: blocks the calling JS thread (like Atomics.wait).
WaitResult SyncWait(int32_t* addr, uint32_t expected, double timeoutMs /* NaN = infinite */);
// Wakes up to `count` waiters on the word. Returns how many were woken.
int SyncWake(int32_t* addr, int count);

// Async: runs on a membridge-owned waiter thread (never the libuv pool).
// Registers the wait and returns false when the process-wide cap is exhausted
// (caller rejects with E_TOO_MANY_WAITERS). `viewBuffer` is pinned for the
// wait's duration so the word's pages stay alive; the promise is fulfilled on
// the isolate's loop thread ('ok' | 'timed-out').
struct PromiseState {
  v8::Isolate* isolate;
  v8::Global<v8::Context> context;
  v8::Global<v8::Promise::Resolver> resolver;
  // Kept alive while the wait pends; reset on a JS thread (Globals must not
  // be destroyed off-isolate).
  v8::Global<v8::ArrayBuffer> buf;
  std::atomic<bool> settled{false};
};

bool StartAsyncWait(v8::Isolate* isolate, v8::Local<v8::Promise::Resolver> resolver,
                    v8::Local<v8::ArrayBuffer> viewBuffer, int32_t* addr, uint32_t expected,
                    double timeoutMs, bool hasTimeout);

// Isolate teardown (worker exit / .terminate()): cancels this isolate's
// pending waits and resolves each promise as 'timed-out' (§6).
void CancelIsolateWaits(v8::Isolate* isolate);

}  // namespace membridge

#endif  // MEMBRIDGE_WAIT_H_
