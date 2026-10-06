// wait.cc — §6 implementation.
//
// Linux shared futexes are keyed by the underlying page, so FUTEX_WAIT /
// FUTEX_WAKE without FUTEX_PRIVATE_FLAG wake across processes on the same shm
// page (F14, probes/f14-shared-futex.js). glibc exports no futex symbols, so
// everything goes through raw syscalls.
//
// Async waits run on membridge-owned threads (never the libuv pool):
//  - Linux >= 5.16: ONE multiplexer thread parks in futex_waitv() with up to
//    127 waiters (FUTEX2_SIZE_U32, no FUTEX2_PRIVATE) plus a process-private
//    control word; the syscall returns the index of the woken entry (pinned
//    by the M1 probe). Registering/cancelling bumps the control word and
//    wakes the mux to rebuild its wait set.
//  - otherwise: one thread per pending wait, capped -> E_TOO_MANY_WAITERS.
//
// Fulfilment crosses onto the isolate's loop thread via a uv_async + queue;
// pending waits pin their Mapping and are cancelled+resolved 'timed-out' at
// isolate teardown (worker exit or .terminate()).

#include "wait.h"
#include "registry.h"

#include <string>

#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#ifndef __NR_futex_waitv
#define __NR_futex_waitv 449
#endif
#ifndef FUTEX2_SIZE_U32
#define FUTEX2_SIZE_U32 2
#endif
#ifndef FUTEX2_PRIVATE
#define FUTEX2_PRIVATE 128
#endif
#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif
#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif
#endif

#include <uv.h>

namespace membridge {

namespace {

// ---- clock helpers ---------------------------------------------------------

double NowMs() {
  using namespace std::chrono;
  return duration_cast<duration<double, std::milli>>(steady_clock::now().time_since_epoch())
      .count();
}

#if defined(__linux__)
struct timespec DeadlineToTimespec(double deadlineMs) {
  // futex_waitv takes an ABSOLUTE timeout on the given clockid — deadlineMs is
  // on the steady (monotonic) epoch, so convert directly, no "remaining".
  if (deadlineMs < 0) deadlineMs = 0;
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(deadlineMs / 1000);
  ts.tv_nsec = static_cast<long>((deadlineMs - ts.tv_sec * 1000) * 1e6);
  return ts;
}
#endif

// ---- node + hub ------------------------------------------------------------

enum class Fulfill { kOk, kTimedOut };

struct WaitNode;

struct Hub {
  uv_async_t async;
  v8::Isolate* isolate;
  std::mutex mu;
  std::deque<std::pair<std::shared_ptr<PromiseState>, Fulfill>> queue;
  int pending = 0;
  bool closed = false;
  // Set while the handle is closing: keeps the Hub alive until uv's close
  // callback runs, so g_hubs can drop it immediately (review F3).
  std::shared_ptr<Hub> selfKeepAlive;
};

std::mutex g_hubs_mu;
std::map<v8::Isolate*, std::shared_ptr<Hub>> g_hubs;

Hub* HubFor(v8::Isolate* isolate) {
  std::lock_guard<std::mutex> lock(g_hubs_mu);
  auto it = g_hubs.find(isolate);
  return it == g_hubs.end() ? nullptr : it->second.get();
}

struct WaitNode {
  int32_t* addr;
  uint32_t expected;
  bool hasTimeout;
  double deadlineMs;  // steady-clock ms; valid when hasTimeout
  std::shared_ptr<PromiseState> state;
  std::shared_ptr<Hub> hub;  // owns the hub: survives teardown's erase+close (F18)
  bool onThread = false;
  bool cancelled = false;
  std::atomic<bool> delivered{false};  // exactly one pending-decrement per node
};

void ResolveOnLoop(v8::Isolate* isolate, PromiseState& state, const char* result) {
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> ctx = state.context.Get(isolate);
  v8::Local<v8::Promise::Resolver> resolver = state.resolver.Get(isolate);
  state.resolver.Reset();
  state.buf.Reset();
  state.context.Reset();  // Globals must not die off-isolate (review F17)
  resolver
      ->Resolve(ctx, v8::String::NewFromUtf8(isolate, result).ToLocalChecked())
      .Check();
  // Raw uv callbacks do not drain microtasks — the promise reaction job
  // (the .then) must run here.
  isolate->PerformMicrotaskCheckpoint();
}

// ---- process-wide wait set + multiplexer -----------------------------------

constexpr size_t kMuxCapacity = 127;  // futex_waitv entries minus control word
constexpr size_t kThreadOverflowCap = 64;
constexpr size_t kTotalCap = kMuxCapacity + kThreadOverflowCap;

std::mutex g_mu;
std::deque<std::shared_ptr<WaitNode>> g_pending;   // handled by the mux
std::vector<std::shared_ptr<WaitNode>> g_threads;  // one thread per node
int32_t g_ctrl = 0;  // process-private control word (same process as the mux)
size_t g_threadWaits = 0;
bool g_muxStarted = false;
bool g_waitvOk = false;

void AsyncCallback(uv_async_t* handle) {
  auto* hub = static_cast<Hub*>(handle->data);
  std::deque<std::pair<std::shared_ptr<PromiseState>, Fulfill>> batch;
  {
    std::lock_guard<std::mutex> lock(hub->mu);
    batch.swap(hub->queue);
  }
  for (auto& f : batch) {
    if (f.first->settled.exchange(true)) continue;
    ResolveOnLoop(hub->isolate, *f.first, f.second == Fulfill::kOk ? "ok" : "timed-out");
  }
  // Loop pinning is done in JS (sync.ts holds a timer while waits pend):
  // uv_ref alone proved unreliable across Node versions (Node 22 runner).
}



void WakeMux() {
#if defined(__linux__)
  // The control word is process-private, so PRIVATE is correct here.
  syscall(SYS_futex, &g_ctrl, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, nullptr, nullptr, 0);
#endif
}

void Deliver(const std::shared_ptr<WaitNode>& node, Fulfill result) {
  if (node->delivered.exchange(true)) return;  // one delivery per node, ever
  const std::shared_ptr<Hub> hub = node->hub;  // keeps the hub alive (F18)
  // Do NOT settle here — the resolve point (AsyncCallback or the teardown
  // hook) settles exactly once. Dropping out of the wait set happens via
  // `cancelled`, which the mux snapshot prunes. The closed-check and the
  // send happen under the same lock: no send on a closing/closed handle.
  bool send = false;
  if (!node->state->settled.load()) {
    std::lock_guard<std::mutex> lk(hub->mu);
    if (!hub->closed) {
      hub->queue.emplace_back(node->state, result);
      if (hub->pending > 0) hub->pending--;
      send = true;
    }
  }
  node->cancelled = true;
  if (send) uv_async_send(&hub->async);
}

// Fallback waiter thread: one per wait (futex where available, bounded poll
// otherwise). Runs with the node already removed from g_pending.
void WaitThreadMain(std::shared_ptr<WaitNode> node) {
#if defined(__linux__)
  for (;;) {
    if (node->cancelled) break;  // resolved by the teardown hook
    double remaining = node->hasTimeout ? node->deadlineMs - NowMs() : 1e12;
    if (node->hasTimeout && remaining <= 0) {
      Deliver(node, Fulfill::kTimedOut);
      break;
    }
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(remaining / 1000);
    ts.tv_nsec = static_cast<long>((remaining - ts.tv_sec * 1000) * 1e6);
    long r = syscall(SYS_futex, node->addr, FUTEX_WAIT, node->expected, &ts, nullptr, 0);
    if (node->cancelled) break;
    if (r == 0) {
      Deliver(node, Fulfill::kOk);
      break;
    }
    if (errno == EAGAIN) {
      Deliver(node, Fulfill::kOk);  // the awaited store already landed
      break;
    }
    if (errno == ETIMEDOUT) {
      Deliver(node, Fulfill::kTimedOut);
      break;
    }
    // EINTR or other: retry with a recomputed deadline
  }
#elif defined(_WIN32)
  // Named-semaphore wait (§6): block on the word's semaphore in bounded
  // slices and re-check the word each wake — a missed/extra permit costs a
  // spurious wake, never correctness.
  const std::wstring name = WordSemaphoreName(node->addr);
  HANDLE sem = CreateSemaphoreW(nullptr, 0, 0x7FFFFFFF, name.c_str());
  if (sem == nullptr) {
    Deliver(node, Fulfill::kTimedOut);
    return;
  }
  for (;;) {
    if (node->cancelled) break;
    if (*node->addr != static_cast<int32_t>(node->expected)) {
      Deliver(node, Fulfill::kOk);
      break;
    }
    DWORD slice = 250;
    if (node->hasTimeout) {
      const double rem = node->deadlineMs - NowMs();
      if (rem <= 0) {
        Deliver(node, Fulfill::kTimedOut);
        break;
      }
      slice = static_cast<DWORD>(rem > 250 ? 250 : rem);
    }
    const DWORD wr = WaitForSingleObject(sem, slice);
    if (node->cancelled) break;
    if (wr == WAIT_OBJECT_0) {
      if (*node->addr != static_cast<int32_t>(node->expected)) {
        Deliver(node, Fulfill::kOk);
        break;
      }
      // permit without the store landing (race, §6): loop and re-check
    } else if (wr == WAIT_TIMEOUT) {
      if (node->hasTimeout && NowMs() >= node->deadlineMs) {
        Deliver(node, Fulfill::kTimedOut);
        break;
      }
    }
  }
  CloseHandle(sem);
#else
  // macOS / other: os_sync when available (U1), else bounded exponential
  // back-off poll (50 µs -> 2 ms), §6 fallback. Correctness comes from
  // re-checking the word; wake latency <= 2 ms in the fallback.
  const bool useOsSync =
      os_sync_wait_on_address != nullptr && os_sync_wake_by_address_shared != nullptr;
  double delayUs = 50;
  for (;;) {
    if (node->cancelled) break;  // resolved by the teardown hook
    if (*node->addr != static_cast<int32_t>(node->expected)) {
      Deliver(node, Fulfill::kOk);
      break;
    }
    uint32_t sliceMs = 250;
    if (node->hasTimeout) {
      const double rem = node->deadlineMs - NowMs();
      if (rem <= 0) {
        Deliver(node, Fulfill::kTimedOut);
        break;
      }
      sliceMs = static_cast<uint32_t>(rem > 250 ? 250 : rem);
    }
    if (useOsSync) {
      os_sync_wait_on_address(node->addr, static_cast<uint64_t>(
                                              static_cast<uint32_t>(node->expected)),
                              sizeof(int32_t), OS_SYNC_WAIT_ON_ADDRESS_SHARED);
      // spurious wakes and value changes are re-checked on the next iteration
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(delayUs)));
      if (delayUs < 2000) delayUs *= 2;
      (void)sliceMs;
    }
  }
#endif
  if (node->onThread) {
    const std::shared_ptr<Hub> hub = node->hub;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      g_threadWaits--;
      for (auto it = g_threads.begin(); it != g_threads.end(); ++it) {
        if (it->get() == node.get()) {
          g_threads.erase(it);
          break;
        }
      }
    }
    if (hub != nullptr) {
      std::lock_guard<std::mutex> lk(hub->mu);
      if (!hub->closed) uv_async_send(&hub->async);  // empty-queue wake: harmless
    }
  }
}

// Multiplexer thread body (Linux >= 5.16 only).
void MuxMain() {
#if defined(__linux__)
  for (;;) {
    std::vector<std::shared_ptr<WaitNode>> snapshot;
    int32_t ctrlExpected;
    double minDeadline = 0;
    bool hasTimeout = false;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      for (auto it = g_pending.begin(); it != g_pending.end();) {
        if ((*it)->cancelled || (*it)->state->settled.load()) {
          it = g_pending.erase(it);
        } else {
          ++it;
        }
      }
      snapshot.reserve(g_pending.size());
      for (const std::shared_ptr<WaitNode>& n : g_pending) snapshot.push_back(n);
      ctrlExpected = g_ctrl;
      for (const std::shared_ptr<WaitNode>& n : snapshot) {
        if (n->hasTimeout && (!hasTimeout || n->deadlineMs < minDeadline)) {
          minDeadline = n->deadlineMs;
          hasTimeout = true;
        }
      }
    }

    const size_t n = snapshot.size() + 1;  // + control word
    std::vector<struct futex_waitv> vec(n);
    for (size_t i = 0; i < snapshot.size(); i++) {
      vec[i].val = static_cast<__u64>(snapshot[i]->expected);
      vec[i].uaddr = reinterpret_cast<__u64>(snapshot[i]->addr);
      vec[i].flags = FUTEX2_SIZE_U32;  // no FUTEX2_PRIVATE: shared
      vec[i].__reserved = 0;
    }
    vec[snapshot.size()].val = static_cast<__u64>(static_cast<uint32_t>(ctrlExpected));
    vec[snapshot.size()].uaddr = reinterpret_cast<__u64>(&g_ctrl);
    vec[snapshot.size()].flags = FUTEX2_SIZE_U32 | FUTEX2_PRIVATE;  // process-private
    vec[snapshot.size()].__reserved = 0;

    struct timespec ts;
    struct timespec* tsp = nullptr;
    if (hasTimeout) {
      ts = DeadlineToTimespec(minDeadline);
      tsp = &ts;
    }
    long r = syscall(__NR_futex_waitv, vec.data(), static_cast<int>(n), 0, tsp, CLOCK_MONOTONIC);

    std::lock_guard<std::mutex> lock(g_mu);
    // r is the 0-based INDEX of the woken entry (M1 probe pinned this).
    if (r >= 0 && static_cast<size_t>(r) < snapshot.size()) {
      Deliver(snapshot[static_cast<size_t>(r)], Fulfill::kOk);
    } else if (r == -1 && errno == EAGAIN) {
      // Some word changed before we could park (or the control word moved).
      // Fulfil every data waiter whose word already differs: the store the
      // waiter awaits has landed. Skip delivered/cancelled nodes — their
      // buffer pin may already be gone, so their word may be unmapped (F32).
      for (size_t i = 0; i < snapshot.size(); i++) {
        if (snapshot[i]->delivered.load() || snapshot[i]->cancelled) continue;
        if (*snapshot[i]->addr != static_cast<int32_t>(snapshot[i]->expected)) {
          Deliver(snapshot[i], Fulfill::kOk);
        }
      }
    } else if (r == -1 && errno == ETIMEDOUT) {
      const double now = NowMs();
      for (size_t i = 0; i < snapshot.size(); i++) {
        if (snapshot[i]->hasTimeout && snapshot[i]->deadlineMs <= now) {
          Deliver(snapshot[i], Fulfill::kTimedOut);
        }
      }
    }
    // EINTR / control wake / other: re-snapshot and go again.
  }
#endif
}

void EnsureMux() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_muxStarted) return;
  g_muxStarted = true;
#if defined(__linux__)
  // Probe futex_waitv: a deliberately mismatched value returns EAGAIN when
  // the syscall exists, ENOSYS on older kernels (M1 spike pinned this).
  int32_t probeWord = 0;
  struct futex_waitv w;
  w.val = 1;  // != probeWord -> immediate EAGAIN when supported
  w.uaddr = reinterpret_cast<__u64>(&probeWord);
  w.flags = FUTEX2_SIZE_U32;
  w.__reserved = 0;
  long r = syscall(__NR_futex_waitv, &w, 1, 0, nullptr, CLOCK_MONOTONIC);
  g_waitvOk = !(r == -1 && errno == ENOSYS);
  if (g_waitvOk) {
    std::thread(MuxMain).detach();
    return;
  }
#endif
  // Without waitv the fallback path spawns one thread per wait (below).
}

}  // namespace

// ---- public API ------------------------------------------------------------

WaitResult SyncWait(int32_t* addr, uint32_t expected, double timeoutMs) {
#if defined(__linux__)
  const bool infinite = timeoutMs != timeoutMs;  // NaN = infinite
  const double deadline = infinite ? 0 : NowMs() + timeoutMs;  // absolute
  for (;;) {
    struct timespec ts;
    struct timespec* tsp = nullptr;
    if (!infinite) {
      const double remaining = deadline - NowMs();
      if (remaining <= 0) return WaitResult::kTimedOut;
      ts.tv_sec = static_cast<time_t>(remaining / 1000);
      ts.tv_nsec = static_cast<long>((remaining - ts.tv_sec * 1000) * 1e6);
      tsp = &ts;
    }
    long r = syscall(SYS_futex, addr, FUTEX_WAIT, expected, tsp, nullptr, 0);
    if (r == 0) return WaitResult::kOk;
    if (errno == EAGAIN) return WaitResult::kNotEqual;
    if (errno == ETIMEDOUT) return WaitResult::kTimedOut;
    if (errno == EINTR) continue;
    return WaitResult::kTimedOut;  // unexpected errno: bounded failure
  }
#else
  // Polling fallback (§6): bounded exponential back-off, 50 µs -> 2 ms.
  const bool infinite = timeoutMs != timeoutMs;
  const double deadline = infinite ? 0 : NowMs() + timeoutMs;
  double delayUs = 50;
  for (;;) {
    if (*addr != static_cast<int32_t>(expected)) return WaitResult::kOk;
    if (!infinite && NowMs() >= deadline) return WaitResult::kTimedOut;
    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(delayUs)));
    if (delayUs < 2000) delayUs *= 2;
  }
#endif
}

#if defined(__APPLE__)
// U1 (unverified locally): macOS 14.4+ os_sync with the SHARED flag. Weak
// imports keep the addon loadable on older macOS; when the symbols are
// absent we degrade to the bounded-poll fallback below.
extern "C" {
int os_sync_wait_on_address(void* address, uint64_t value, size_t size, uint32_t flags)
    __attribute__((weak_import));
int os_sync_wake_by_address_shared(void* address, size_t size, uint32_t flags)
    __attribute__((weak_import));
}
#ifndef OS_SYNC_WAIT_ON_ADDRESS_SHARED
#define OS_SYNC_WAIT_ON_ADDRESS_SHARED 0x00000001
#endif
#endif

#if defined(_WIN32)
// Named semaphore per word (§6, U3: WaitOnAddress is process-private). The
// waiter count lives in the semaphore permits themselves: notify releases
// `count` permits; extra permits surface as allowed spurious wakeups —
// every waiter re-checks the word, so correctness never depends on a wake.
static std::wstring WordSemaphoreName(int32_t* addr) {
  std::shared_ptr<Mapping> m = Registry::FindByAddress(addr);
  wchar_t buf[256];
  if (m) {
    std::string escaped;
    for (char c : m->name) {
      if (c == '/') escaped += "%2F";
      else if (c == '%') escaped += "%25";
      else escaped += c;
    }
    const ptrdiff_t off = static_cast<int32_t*>(addr) - static_cast<int32_t*>(m->base) -
                          m->headerBytes / 4;
    _snwprintf(buf, 255, L"Local\\membridge-%hs-w%td", escaped.c_str(),
               static_cast<ptrdiff_t>(off));
  } else {
    _snwprintf(buf, 255, L"Local\\membridge-anon-%p", static_cast<void*>(addr));
  }
  return std::wstring(buf);
}
#endif

int SyncWake(int32_t* addr, int count) {
#if defined(__linux__)
  // No FUTEX_PRIVATE_FLAG: shared futexes are keyed by the underlying page.
  return static_cast<int>(
      syscall(SYS_futex, addr, FUTEX_WAKE, static_cast<uint32_t>(count), nullptr, nullptr, 0));
#elif defined(__APPLE__)
  if (os_sync_wake_by_address_shared != nullptr) {
    return os_sync_wake_by_address_shared(addr, sizeof(int32_t), OS_SYNC_WAIT_ON_ADDRESS_SHARED);
  }
  return 0;  // poll fallback: waiters re-check on their back-off schedule
#elif defined(_WIN32)
  const std::wstring name = WordSemaphoreName(addr);
  HANDLE sem = OpenSemaphoreW(SEMAPHORE_MODIFY, FALSE, name.c_str());
  if (sem == nullptr) return 0;  // no waiter ever created it
  const BOOL ok = ReleaseSemaphore(sem, count, nullptr);
  CloseHandle(sem);
  return ok ? count : 0;
#else
  (void)addr;
  (void)count;
  return 0;
#endif
}

bool StartAsyncWait(v8::Isolate* isolate, v8::Local<v8::Promise::Resolver> resolver,
                    v8::Local<v8::ArrayBuffer> viewBuffer, int32_t* addr, uint32_t expected,
                    double timeoutMs, bool hasTimeout) {
  std::shared_ptr<Hub> hub;
  {
    std::lock_guard<std::mutex> lock(g_hubs_mu);
    hub = g_hubs[isolate];
    if (!hub) {
      hub = std::make_shared<Hub>();
      hub->isolate = isolate;
      hub->async.data = hub.get();
      uv_loop_t* loop = node::GetCurrentEventLoop(isolate);
      uv_async_init(loop, &hub->async, AsyncCallback);
      // Delivery-only handle: it must never pin the loop (sync.ts owns the
      // loop lifetime with a JS timer while waits are outstanding).
      uv_unref(reinterpret_cast<uv_handle_t*>(&hub->async));
      g_hubs[isolate] = hub;
      node::AddEnvironmentCleanupHook(
          isolate,
          [](void* p) { CancelIsolateWaits(static_cast<v8::Isolate*>(p)); },
          isolate);
    }
  }

  auto state = std::make_shared<PromiseState>();
  state->isolate = isolate;
  state->context.Reset(isolate, isolate->GetCurrentContext());
  state->resolver.Reset(isolate, resolver);
  state->buf.Reset(isolate, viewBuffer);

  auto node = std::make_shared<WaitNode>();
  node->addr = addr;
  node->expected = expected;
  node->hasTimeout = hasTimeout;
  node->deadlineMs = hasTimeout ? NowMs() + timeoutMs : 0;
  node->state = state;
  node->hub = hub;

  EnsureMux();
  bool registered = false;
  bool onThread = false;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    const size_t pending = g_pending.size();
    if (g_waitvOk && pending < kMuxCapacity) {
      g_pending.push_back(node);
      registered = true;
      g_ctrl++;
    } else if (g_threadWaits < kThreadOverflowCap) {
      g_threads.push_back(node);
      registered = true;
      onThread = true;
      node->onThread = true;
      g_threadWaits++;
    }
  }
  if (!registered) {
    return false;  // caller rejects with E_TOO_MANY_WAITERS
  }
  WakeMux();
  if (onThread) {
    std::thread(WaitThreadMain, node).detach();
  }
  {
    std::lock_guard<std::mutex> lock(hub->mu);
    hub->pending++;
  }
  return true;
}

void CancelIsolateWaits(v8::Isolate* isolate) {
  std::vector<std::shared_ptr<PromiseState>> toResolve;
  std::vector<int32_t*> threadWakeAddrs;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto it = g_pending.begin(); it != g_pending.end();) {
      if ((*it)->state->isolate == isolate) {
        (*it)->cancelled = true;
        toResolve.push_back((*it)->state);
        it = g_pending.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = g_threads.begin(); it != g_threads.end();) {
      if ((*it)->state->isolate == isolate) {
        (*it)->cancelled = true;
        toResolve.push_back((*it)->state);
        threadWakeAddrs.push_back((*it)->addr);
        it = g_threads.erase(it);
      } else {
        ++it;
      }
    }
  }
  WakeMux();
  // Cancelled fallback threads park in a SHARED futex on the word: waking it
  // may also wake foreign waiters, which re-check the word and re-park —
  // the standard futex discipline makes that safe (§6).
  for (int32_t* addr : threadWakeAddrs) SyncWake(addr, 1);
  // We are on the dying isolate's JS thread: resolve directly. Globals reset
  // here, on-isolate; PromiseStates stay alive via shared_ptr until the
  // node/thread side drops them.
  for (const std::shared_ptr<PromiseState>& state : toResolve) {
    if (!state->settled.exchange(true)) {
      ResolveOnLoop(isolate, *state, "timed-out");
    }
  }
  std::shared_ptr<Hub> hub;
  {
    std::lock_guard<std::mutex> lock(g_hubs_mu);
    auto it = g_hubs.find(isolate);
    if (it != g_hubs.end()) {
      hub = it->second;
      // Erase NOW (review F3): a later isolate at the same address must get a
      // fresh hub, not this closed one whose loop is dying.
      g_hubs.erase(it);
    }
  }
  if (hub != nullptr) {
    std::lock_guard<std::mutex> lock(hub->mu);
    hub->closed = true;
    hub->pending = 0;
    // Close the handle so the isolate's loop can shut down cleanly (worker
    // .terminate() tears the loop down; an open handle aborts the process).
    // selfKeepAlive holds the Hub until uv's close callback fires.
    hub->selfKeepAlive = hub;
    uv_close(reinterpret_cast<uv_handle_t*>(&hub->async),
             [](uv_handle_t* h) { static_cast<Hub*>(h->data)->selfKeepAlive.reset(); });
  }
}

}  // namespace membridge
