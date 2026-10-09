// spike/addon.cc — shm-bridge M1 spike addon (plain V8 via node.h, no N-API).
//
// Proves the M1 exit criteria from a few dozen lines, mirroring the real §3
// shape so the spike's conclusions transfer to M2:
//   * Mapping owns `base` (what mmap returned); ~Mapping munmaps base.
//   * The SAB is a *window*: NewBackingStore(base + headerBytes, dataBytes,
//     deleter, ref) where ref is a heap shared_ptr<Mapping>; the deleter only
//     drops the reference (it may run on any thread).
//   * Registry is name -> weak_ptr<Mapping> so lifetime follows the last SAB.
//   * Raw shared futexes (no glibc wrapper, F14): FUTEX_WAIT/FUTEX_WAKE via
//     syscall(SYS_futex) without FUTEX_PRIVATE_FLAG, and futex_waitv via
//     syscall(__NR_futex_waitv) with FUTEX2_SIZE_U32 and no FUTEX2_PRIVATE.
//
// Scratch code for M1 only — M2 rebuilds this per PLAN §5 (header table,
// size policies, errors, Windows paths).

#include <node.h>
#include <v8.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

using namespace v8;

namespace {

#ifndef __NR_futex_waitv
#define __NR_futex_waitv 449  // same value on x86_64 and asm-generic (arm64 et al.)
#endif
#ifndef FUTEX2_SIZE_U32
#define FUTEX2_SIZE_U32 2
#endif
#ifndef FUTEX2_PRIVATE
#define FUTEX2_PRIVATE 128
#endif

constexpr size_t kHeaderBytes = 4096;  // spike: fixed one-page header
constexpr double kMaxSegmentBytes = 256.0 * 1024 * 1024;

struct Mapping {
  void* base = nullptr;
  size_t mappingBytes = 0;
  ~Mapping() {
    if (base != nullptr) {
      munmap(base, mappingBytes);
      base = nullptr;
    }
  }
};

std::mutex g_mutex;
std::map<std::string, std::weak_ptr<Mapping>> g_registry;

void SabDeleter(void* /*data*/, size_t /*length*/, void* deleter_data) {
  // Runs when V8 GCs the BackingStore, possibly on any thread. Dropping the
  // shared_ptr may make refcount hit 0 here -> ~Mapping munmaps base.
  delete static_cast<std::shared_ptr<Mapping>*>(deleter_data);
}

Local<SharedArrayBuffer> MakeWindow(Isolate* isolate,
                                    const std::shared_ptr<Mapping>& mapping,
                                    size_t dataOffset, size_t dataBytes) {
  auto* ref = new std::shared_ptr<Mapping>(mapping);
  std::shared_ptr<BackingStore> bs = SharedArrayBuffer::NewBackingStore(
      static_cast<char*>(mapping->base) + dataOffset, dataBytes, SabDeleter,
      ref);
  return SharedArrayBuffer::New(isolate, bs);
}

void ThrowErrno(Isolate* isolate, const char* what, int e) {
  std::string msg = std::string(what) + ": " + strerror(e) + " (errno " +
                    std::to_string(e) + ")";
  isolate->ThrowException(
      Exception::Error(String::NewFromUtf8(isolate, msg.c_str()).ToLocalChecked()));
}

// open(name, dataBytes) -> SAB covering the data region (create-or-join).
void Open(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);
  if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsNumber()) {
    isolate->ThrowException(Exception::TypeError(String::NewFromUtf8Literal(
        isolate, "open(name: string, dataBytes: number)")));
    return;
  }
  String::Utf8Value name(isolate, args[0]);
  double sizeD = args[1]->NumberValue(isolate->GetCurrentContext()).FromJust();
  if (!(sizeD >= 1 && sizeD <= kMaxSegmentBytes)) {
    isolate->ThrowException(Exception::RangeError(String::NewFromUtf8Literal(
        isolate, "dataBytes out of range")));
    return;
  }
  size_t dataBytes = static_cast<size_t>(sizeD);
  size_t mappingBytes = kHeaderBytes + dataBytes;

  std::shared_ptr<Mapping> mapping;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_registry.find(*name);
    if (it != g_registry.end()) mapping = it->second.lock();
    if (mapping && mapping->mappingBytes < mappingBytes) {
      // Grown request: a new Mapping replaces the registry entry (the old
      // mapping stays alive through the SABs that hold it).
      mapping.reset();
    }
    if (!mapping) {
      mapping = std::make_shared<Mapping>();
      int fd = shm_open(*name, O_CREAT | O_RDWR, 0600);
      if (fd < 0) {
        ThrowErrno(isolate, "shm_open", errno);
        return;
      }
      if (ftruncate(fd, static_cast<off_t>(mappingBytes)) != 0) {
        int e = errno;
        close(fd);
        shm_unlink(*name);
        ThrowErrno(isolate, "ftruncate", e);
        return;
      }
      void* base = mmap(nullptr, mappingBytes, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
      close(fd);
      if (base == MAP_FAILED) {
        int e = errno;
        shm_unlink(*name);
        ThrowErrno(isolate, "mmap", e);
        return;
      }
      mapping->base = base;
      mapping->mappingBytes = mappingBytes;
      g_registry[*name] = mapping;
    }
  }
  args.GetReturnValue().Set(
      MakeWindow(isolate, mapping, kHeaderBytes, dataBytes));
}

// window(name, dataOffset, len) -> second SAB over the SAME mapping (U5:
// distinct BackingStores over one mapping).
void Window(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);
  if (args.Length() < 3 || !args[0]->IsString() || !args[1]->IsNumber() ||
      !args[2]->IsNumber()) {
    isolate->ThrowException(Exception::TypeError(String::NewFromUtf8Literal(
        isolate, "window(name: string, dataOffset: number, len: number)")));
    return;
  }
  String::Utf8Value name(isolate, args[0]);
  size_t off = static_cast<size_t>(args[1]->NumberValue(
                  isolate->GetCurrentContext()).FromJust());
  size_t len = static_cast<size_t>(args[2]->NumberValue(
                  isolate->GetCurrentContext()).FromJust());
  std::shared_ptr<Mapping> mapping;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_registry.find(*name);
    if (it != g_registry.end()) mapping = it->second.lock();
  }
  if (!mapping || kHeaderBytes + off + len > mapping->mappingBytes) {
    isolate->ThrowException(Exception::Error(
        String::NewFromUtf8Literal(isolate, "no live mapping for window")));
    return;
  }
  args.GetReturnValue().Set(
      MakeWindow(isolate, mapping, kHeaderBytes + off, len));
}

void Unlink(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);
  if (args.Length() < 1 || !args[0]->IsString()) {
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8Literal(isolate, "unlink(name: string)")));
    return;
  }
  String::Utf8Value name(isolate, args[0]);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_registry.erase(*name);
  }
  if (shm_unlink(*name) != 0 && errno != ENOENT) {
    ThrowErrno(isolate, "shm_unlink", errno);
    return;
  }
  args.GetReturnValue().Set(Undefined(isolate));
}

// alive(name) -> true if the registry holds a live Mapping (GC-lifecycle check)
void Alive(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);
  String::Utf8Value name(isolate, args[0]);
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_registry.find(*name);
  bool alive = it != g_registry.end() && !it->second.expired();
  args.GetReturnValue().Set(Boolean::New(isolate, alive));
}

int32_t* WordAddr(Isolate* isolate, const Local<Value>& arg, int index) {
  if (!arg->IsInt32Array()) {
    isolate->ThrowException(Exception::TypeError(
        String::NewFromUtf8Literal(isolate, "expected Int32Array")));
    return nullptr;
  }
  Local<Int32Array> arr = arg.As<Int32Array>();
  Local<ArrayBuffer> ab = arr->Buffer();
  auto* data = static_cast<int32_t*>(ab->Data());
  if (data == nullptr) {
    isolate->ThrowException(Exception::Error(
        String::NewFromUtf8Literal(isolate, "detached buffer")));
    return nullptr;
  }
  size_t words = arr->Length();
  if (index < 0 || static_cast<size_t>(index) >= words) {
    isolate->ThrowException(Exception::RangeError(
        String::NewFromUtf8Literal(isolate, "index out of range")));
    return nullptr;
  }
  return data + index;
}

// futexWait(int32array, index, expected, timeoutMs) -> code:
//   0 woken, 1 EAGAIN (value mismatch), 2 ETIMEDOUT, 3 EINTR, else -errno.
// Raw syscall, FUTEX_WAIT **without** FUTEX_PRIVATE_FLAG => shared futex.
void FutexWait(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);
  if (args.Length() < 4) {
    isolate->ThrowException(Exception::TypeError(String::NewFromUtf8Literal(
        isolate, "futexWait(arr, index, expected, timeoutMs)")));
    return;
  }
  int32_t* addr = WordAddr(isolate, args[0], args[1]->Int32Value(
                      isolate->GetCurrentContext()).FromJust());
  if (addr == nullptr) return;
  uint32_t expected = static_cast<uint32_t>(
      args[2]->Int32Value(isolate->GetCurrentContext()).FromJust());
  double timeoutMs = args[3]->NumberValue(isolate->GetCurrentContext()).FromJust();
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(timeoutMs / 1000);
  ts.tv_nsec = static_cast<long>((timeoutMs - ts.tv_sec * 1000) * 1e6);
  long r = syscall(SYS_futex, addr, FUTEX_WAIT, expected, &ts, nullptr, 0);
  int64_t code;
  if (r == 0) {
    code = 0;
  } else {
    switch (errno) {
      case EAGAIN: code = 1; break;
      case ETIMEDOUT: code = 2; break;
      case EINTR: code = 3; break;
      default: code = -errno; break;
    }
  }
  args.GetReturnValue().Set(
      Number::New(isolate, static_cast<double>(code)));
}

// futexWake(int32array, index, count) -> number of waiters woken.
void FutexWake(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);
  if (args.Length() < 3) {
    isolate->ThrowException(Exception::TypeError(String::NewFromUtf8Literal(
        isolate, "futexWake(arr, index, count)")));
    return;
  }
  int32_t* addr = WordAddr(isolate, args[0], args[1]->Int32Value(
                      isolate->GetCurrentContext()).FromJust());
  if (addr == nullptr) return;
  int count = args[2]->Int32Value(isolate->GetCurrentContext()).FromJust();
  long r = syscall(SYS_futex, addr, FUTEX_WAKE, count, nullptr, nullptr, 0);
  args.GetReturnValue().Set(Number::New(isolate, static_cast<double>(r)));
}

// futexWaitv([[arr, index, expected], ...], timeoutMs) ->
//   >=0: index of the vector entry that woke (per man page semantics — the
//        spike verifies this empirically), or -errno (ENOSYS => kernel < 5.16
//        fallback: one thread per wait, no multiplexing).
// Each waiter uses FUTEX2_SIZE_U32 and **no** FUTEX2_PRIVATE (shared).
void FutexWaitv(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);
  if (args.Length() < 2 || !args[0]->IsArray()) {
    isolate->ThrowException(Exception::TypeError(String::NewFromUtf8Literal(
        isolate, "futexWaitv([[arr, index, expected], ...], timeoutMs)")));
    return;
  }
  Local<Context> ctx = isolate->GetCurrentContext();
  double timeoutMs = args[1]->NumberValue(ctx).FromJust();
  Local<Array> list = args[0].As<Array>();
  uint32_t n = list->Length();
  if (n == 0 || n > FUTEX_WAITV_MAX) {
    isolate->ThrowException(Exception::RangeError(
        String::NewFromUtf8Literal(isolate, "1..128 waiters required")));
    return;
  }
  std::vector<struct futex_waitv> waiters;
  waiters.reserve(n);
  for (uint32_t i = 0; i < n; i++) {
    Local<Value> elem;
    if (!list->Get(ctx, i).ToLocal(&elem) || !elem->IsArray()) {
      isolate->ThrowException(Exception::TypeError(
          String::NewFromUtf8Literal(isolate, "waiter must be [arr, i, v]")));
      return;
    }
    Local<Array> e = elem.As<Array>();
    Local<Value> arrV, idxV, valV;
    if (!e->Get(ctx, 0).ToLocal(&arrV) || !e->Get(ctx, 1).ToLocal(&idxV) ||
        !e->Get(ctx, 2).ToLocal(&valV)) {
      return;
    }
    int32_t* addr = WordAddr(isolate, arrV,
                             idxV->Int32Value(ctx).FromJust());
    if (addr == nullptr) return;
    struct futex_waitv w;
    w.val = static_cast<__u64>(static_cast<uint32_t>(
        valV->Int32Value(ctx).FromJust()));
    w.uaddr = static_cast<__u64>(reinterpret_cast<uintptr_t>(addr));
    w.flags = FUTEX2_SIZE_U32;  // no FUTEX2_PRIVATE: must be a shared futex
    w.__reserved = 0;
    waiters.push_back(w);
  }
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  time_t addSec = static_cast<time_t>(timeoutMs / 1000);
  double remMs = timeoutMs - static_cast<double>(addSec) * 1000.0;
  struct timespec ts;
  ts.tv_sec = now.tv_sec + addSec;
  ts.tv_nsec = now.tv_nsec + static_cast<long>(remMs * 1e6);
  while (ts.tv_nsec >= 1000000000L) { ts.tv_nsec -= 1000000000L; ts.tv_sec++; }
  long r = syscall(__NR_futex_waitv, waiters.data(), n, 0, &ts, CLOCK_MONOTONIC);
  int64_t code;
  if (r >= 0) {
    code = r;  // spike records the raw return value to pin the semantics
  } else {
    code = -errno;
  }
  args.GetReturnValue().Set(Number::New(isolate, static_cast<double>(code)));
}

}  // namespace

NODE_MODULE_INIT(/* exports, module, context */) {
  // Node 26 / V8 14.6 removed Context::GetIsolate() — use Isolate::GetCurrent()
  // (the module init runs with the registering isolate current).
  Isolate* isolate = Isolate::GetCurrent();
  HandleScope scope(isolate);
  NODE_SET_METHOD(exports, "open", Open);
  NODE_SET_METHOD(exports, "window", Window);
  NODE_SET_METHOD(exports, "unlink", Unlink);
  NODE_SET_METHOD(exports, "alive", Alive);
  NODE_SET_METHOD(exports, "futexWait", FutexWait);
  NODE_SET_METHOD(exports, "futexWake", FutexWake);
  NODE_SET_METHOD(exports, "futexWaitv", FutexWaitv);
}
