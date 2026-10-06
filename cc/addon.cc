// addon.cc — plain-V8 addon entry (node.h, NODE_MODULE_INIT, no N-API —
// PLAN §4). Owns the open() flow: validation, registry reuse with sizes,
// init protocol, BackingStore windows (§5). Native throws are NativeError
// (membridge.h) caught at each entry point — they never cross V8 frames.

#include "membridge.h"
#include "header.h"
#include "liveness.h"
#include "mutex.h"
#include "registry.h"
#include "segment.h"
#include "wait.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace membridge {

namespace {

std::mutex g_error_mu;
std::map<v8::Isolate*, v8::Global<v8::Function>> g_error_ctors;

v8::Local<v8::String> Str(v8::Isolate* isolate, const char* s) {
  return v8::String::NewFromUtf8(isolate, s).ToLocalChecked();
}

v8::Local<v8::Value> MakeError(v8::Isolate* isolate, const char* code, const std::string& message) {
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::Local<v8::Function> ctor;
  {
    std::lock_guard<std::mutex> lock(g_error_mu);
    auto it = g_error_ctors.find(isolate);
    if (it != g_error_ctors.end()) ctor = it->second.Get(isolate);
  }
  if (!ctor.IsEmpty()) {
    v8::Local<v8::Value> argv[2] = {Str(isolate, code), Str(isolate, message.c_str())};
    v8::Local<v8::Value> obj;
    if (ctor->NewInstance(ctx, 2, argv).ToLocal(&obj)) {
      return obj;
    }
  }
  // Fallback (ctor not registered yet): plain Error carrying the code.
  v8::Local<v8::Object> err =
      v8::Exception::Error(Str(isolate, message.c_str())).As<v8::Object>();
  err->Set(ctx, Str(isolate, "code"), Str(isolate, code)).Check();
  return err;
}

void SetErrField(v8::Isolate* isolate, v8::Local<v8::Object> o, const char* key,
                 v8::Local<v8::Value> v) {
  o->Set(isolate->GetCurrentContext(), Str(isolate, key), v).Check();
}

}  // namespace

void SetErrorCtor(v8::Isolate* isolate, v8::Local<v8::Function> ctor) {
  {
    std::lock_guard<std::mutex> lock(g_error_mu);
    g_error_ctors[isolate].Reset(isolate, ctor);
  }
  // Drop the entry when the isolate goes away so the map never holds a
  // Global for a destroyed isolate.
  node::AddEnvironmentCleanupHook(
      isolate,
      [](void* p) {
        v8::Isolate* iso = static_cast<v8::Isolate*>(p);
        std::lock_guard<std::mutex> lock(g_error_mu);
        g_error_ctors.erase(iso);
      },
      isolate);
}

void ThrowError(v8::Isolate* isolate, const char* code, const std::string& message) {
  isolate->ThrowException(MakeError(isolate, code, message));
  throw NativeError{};
}

void ThrowError(v8::Isolate* isolate, const char* code, const std::string& message,
                const std::string& name) {
  v8::Local<v8::Value> err = MakeError(isolate, code, message);
  if (err->IsObject()) {
    SetErrField(isolate, err.As<v8::Object>(), "segmentName", Str(isolate, name.c_str()));
  }
  isolate->ThrowException(err);
  throw NativeError{};
}

void ThrowError(v8::Isolate* isolate, const char* code, const std::string& message,
                const std::string& name, double requested, double existing) {
  v8::Local<v8::Value> err = MakeError(isolate, code, message);
  if (err->IsObject()) {
    v8::Local<v8::Object> o = err.As<v8::Object>();
    SetErrField(isolate, o, "segmentName", Str(isolate, name.c_str()));
    SetErrField(isolate, o, "requested", v8::Number::New(isolate, requested));
    SetErrField(isolate, o, "existing", v8::Number::New(isolate, existing));
  }
  isolate->ThrowException(err);
  throw NativeError{};
}

void ThrowSystemError(v8::Isolate* isolate, const std::string& syscall, int err,
                      const std::string& name) {
  const std::string msg =
      syscall + " failed: " + strerror(err) + " (errno " + std::to_string(err) + ")";
  v8::Local<v8::Value> e = MakeError(isolate, "E_SYSTEM", msg);
  if (e->IsObject()) {
    v8::Local<v8::Object> o = e.As<v8::Object>();
    if (!name.empty()) {
      SetErrField(isolate, o, "segmentName", Str(isolate, name.c_str()));
    }
    SetErrField(isolate, o, "syscall", Str(isolate, syscall.c_str()));
    SetErrField(isolate, o, "errno", v8::Number::New(isolate, static_cast<double>(err)));
  }
  isolate->ThrowException(e);
  throw NativeError{};
}

namespace {

// ---- JS -> C++ option parsing ---------------------------------------------

bool GetProp(v8::Isolate* isolate, v8::Local<v8::Context> ctx, v8::Local<v8::Object> o,
             const char* key, v8::Local<v8::Value>* out) {
  return o->Get(ctx, Str(isolate, key)).ToLocal(out);
}

double GetEnvMaxSegmentBytes() {
  const char* env = std::getenv("MEMBRIDGE_MAX_SEGMENT_BYTES");
  if (env == nullptr) return static_cast<double>(kDefaultMaxSegmentBytes);
  char* end = nullptr;
  const long long v = std::strtoll(env, &end, 10);
  if (end == env || v <= 0) return static_cast<double>(kDefaultMaxSegmentBytes);
  return static_cast<double>(v);
}

bool ParseOpts(v8::Isolate* isolate, v8::Local<v8::Value> optsVal, OpenOpts* opts, int* kind) {
  *kind = 0;  // kKindPlain
  if (optsVal.IsEmpty() || !optsVal->IsObject()) return true;
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  v8::Local<v8::Object> o = optsVal.As<v8::Object>();
  v8::Local<v8::Value> v;

  opts->maxSegmentBytes = static_cast<uint64_t>(GetEnvMaxSegmentBytes());

  if (GetProp(isolate, ctx, o, "mode", &v) && v->IsString()) {
    v8::String::Utf8Value s(isolate, v);
    if (std::strcmp(*s, "create") == 0) opts->mode = Mode::kCreate;
    else if (std::strcmp(*s, "join") == 0) opts->mode = Mode::kJoin;
    else if (std::strcmp(*s, "create-or-join") != 0) return false;
  }
  if (GetProp(isolate, ctx, o, "sizePolicy", &v) && v->IsString()) {
    v8::String::Utf8Value s(isolate, v);
    if (std::strcmp(*s, "exact") == 0) opts->sizePolicy = SizePolicy::kExact;
    else if (std::strcmp(*s, "at-least") == 0) opts->sizePolicy = SizePolicy::kAtLeast;
    else if (std::strcmp(*s, "grow") == 0) opts->sizePolicy = SizePolicy::kGrow;
    else return false;
  }
  if (GetProp(isolate, ctx, o, "reserve", &v) && !v->IsUndefined()) {
    if (!v->IsBoolean()) return false;
    opts->reserve = v->BooleanValue(isolate);
  }
  if (GetProp(isolate, ctx, o, "permissions", &v) && !v->IsUndefined()) {
    if (!v->IsInt32()) return false;
    opts->permissions = static_cast<uint32_t>(v->Int32Value(ctx).ToChecked());
  }
  if (GetProp(isolate, ctx, o, "initTimeoutMs", &v) && !v->IsUndefined()) {
    if (!v->IsNumber()) return false;
    const double d = v->NumberValue(ctx).ToChecked();
    if (!(d > 0)) return false;
    opts->initTimeoutMs = d;
  }
  if (GetProp(isolate, ctx, o, "raw", &v) && !v->IsUndefined()) {
    if (!v->IsBoolean()) return false;
    opts->raw = v->BooleanValue(isolate);
  }
  if (GetProp(isolate, ctx, o, "winGlobal", &v) && !v->IsUndefined()) {
    if (!v->IsBoolean()) return false;
    opts->winGlobal = v->BooleanValue(isolate);
  }
  if (GetProp(isolate, ctx, o, "unlinkWhenUnused", &v) && !v->IsUndefined()) {
    if (!v->IsBoolean()) return false;
    opts->unlinkWhenUnused = v->BooleanValue(isolate);
  }
  if (GetProp(isolate, ctx, o, "kind", &v) && !v->IsUndefined()) {
    if (!v->IsInt32()) return false;
    *kind = v->Int32Value(ctx).ToChecked();
  }
  return true;
}

// ---- SAB windows -----------------------------------------------------------

void SabDeleter(void* /*data*/, size_t /*length*/, void* deleter_data) {
  // Runs when V8 GCs the BackingStore, possibly on any thread. Dropping the
  // shared_ptr may bring refcount to 0 here -> ~Mapping unmaps base.
  delete static_cast<std::shared_ptr<Mapping>*>(deleter_data);
}

v8::Local<v8::SharedArrayBuffer> MakeWindow(v8::Isolate* isolate,
                                            const std::shared_ptr<Mapping>& m,
                                            uint64_t dataOffset, uint64_t windowBytes) {
  m->bsCount.fetch_add(1, std::memory_order_relaxed);
  auto* ref = new std::shared_ptr<Mapping>(m);
  std::shared_ptr<v8::BackingStore> bs = v8::SharedArrayBuffer::NewBackingStore(
      static_cast<char*>(m->base) + dataOffset, static_cast<size_t>(windowBytes), SabDeleter,
      ref);
  return v8::SharedArrayBuffer::New(isolate, bs);
}

// RAII for the segment between OpenSegment and ownership transfer to Mapping.
struct SegmentGuard {
  SegmentHandle h;
  ~SegmentGuard() {
    if (h.base != nullptr) CloseSegment(h);
  }
};

// ---- open ------------------------------------------------------------------

uint64_t HeaderDataBytes(Header* h) {
  return *reinterpret_cast<std::atomic<uint64_t>*>(&h->dataBytes);
}

[[noreturn]] void ThrowSizeMismatch(v8::Isolate* isolate, const std::string& name,
                                    uint64_t requested, uint64_t existing) {
  ThrowError(isolate, "E_SIZE_MISMATCH",
             "size policy violated: requested " + std::to_string(requested) + " vs existing " +
                 std::to_string(existing),
             name, static_cast<double>(requested), static_cast<double>(existing));
}

// Reuse evaluation for a live registry mapping (§5.4): the cached mapping is
// reused only if its dataBytes — re-read from the header, since the segment
// may have been grown by another process — covers the request under the
// active policy AND the mapping itself covers the window (an externally grown
// segment needs a bigger mapping: grow-replaces-entry via a full open).
// Returns false when the caller must run a full open.
bool TryReuse(v8::Isolate* isolate, const std::shared_ptr<Mapping>& m, const OpenOpts& opts,
              bool haveSize, uint64_t requested, uint64_t* windowBytes) {
  const uint64_t existing = m->raw ? m->dataBytes : HeaderDataBytes(static_cast<Header*>(m->base));
  uint64_t want = existing;
  if (haveSize) {
    switch (opts.sizePolicy) {
      case SizePolicy::kExact:
        if (existing != requested) ThrowSizeMismatch(isolate, m->name, requested, existing);
        want = requested;
        break;
      case SizePolicy::kAtLeast:
        if (requested > existing) ThrowSizeMismatch(isolate, m->name, requested, existing);
        want = requested;
        break;
      case SizePolicy::kGrow:
        if (requested > existing) return false;
        want = requested;
        break;
    }
  }
  if (want > m->dataBytes) return false;  // mapping too small for the window
  *windowBytes = want;
  return true;
}

void ValidateSize(v8::Isolate* isolate, const std::string& name, bool haveSize, double sizeD,
                  const OpenOpts& opts) {
  if (!haveSize) return;
  if (!(sizeD >= 1.0 && sizeD <= static_cast<double>(kHardMaxSegmentBytes)) ||
      sizeD != std::floor(sizeD)) {
    ThrowError(isolate, "E_SIZE_INVALID",
               "size must be a safe integer >= 1 (got " + std::to_string(sizeD) + ")", name);
  }
  if (sizeD > static_cast<double>(opts.maxSegmentBytes)) {
    ThrowError(isolate, "E_SIZE_INVALID",
               "size " + std::to_string(static_cast<int64_t>(sizeD)) +
                   " exceeds MEMBRIDGE_MAX_SEGMENT_BYTES (" +
                   std::to_string(static_cast<int64_t>(opts.maxSegmentBytes)) + ")",
               name);
  }
}

void Open(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

  if (args.Length() < 1 || !args[0]->IsString()) {
    isolate->ThrowException(v8::Exception::TypeError(
        Str(isolate, "open(name: string, size?: number, opts?: object)")));
    return;
  }
  v8::String::Utf8Value nameArg(isolate, args[0]);
  const std::string name(*nameArg, nameArg.length());

  bool haveSize = false;
  double sizeD = 0;
  if (args.Length() >= 2 && args[1]->IsNumber()) {
    haveSize = true;
    sizeD = args[1]->NumberValue(ctx).ToChecked();
  }
  v8::Local<v8::Value> optsVal;
  if (args.Length() >= 3) {
    optsVal = args[2];
  } else if (args.Length() == 2 && !args[1]->IsNumber()) {
    optsVal = args[1];  // open(name, opts) — join at existing size (§5.1)
  }

  OpenOpts opts;
  int kind = 0;
  if (!ParseOpts(isolate, optsVal, &opts, &kind)) {
    isolate->ThrowException(v8::Exception::TypeError(Str(isolate, "invalid open() options")));
    return;
  }
  ValidateSize(isolate, name, haveSize, sizeD, opts);
  ValidateName(isolate, name);

  const uint64_t requested = haveSize ? static_cast<uint64_t>(sizeD) : 0;

  const uint32_t headerBytes = opts.raw ? 0u : EffectiveHeaderBytes();
  uint32_t kindFlags = kKindPlain;
  if (kind == 1) kindFlags = kKindMutex;
  else if (kind == 2) kindFlags = kKindRing;

  // Registry reuse (§5.4). mode 'create' never reuses: it must reach the
  // E_EXISTS check even when a live mapping is cached. A raw/plain mismatch
  // never reuses either (review F33): the window offset differs by a header
  // page.
  if (opts.mode != Mode::kCreate) {
    if (auto live = Registry::Get().Find(name)) {
      // Review F16: another process may have unlinked and recreated the name
      // — the cached mapping would silently split this process onto old
      // memory. Validate the object identity (dev/ino) before reusing.
      if (live->raw != opts.raw || !NameRefersTo(name, *live)) {
        Registry::Get().Erase(name);
      } else {
        uint64_t windowBytes = 0;
        if (TryReuse(isolate, live, opts, haveSize, requested, &windowBytes)) {
          args.GetReturnValue().Set(MakeWindow(isolate, live, live->headerBytes, windowBytes));
          return;
        }
      }
    }
  }

  // Size-less open joins at the existing size (whole object); it can never
  // create. Size policy for it is "whatever exists".
  if (!haveSize) {
    if (opts.mode == Mode::kCreate) {
      ThrowError(isolate, "E_SIZE_INVALID", "size is required when creating a segment", name);
    }
    opts.mode = Mode::kJoin;
  }

  SegmentGuard guard;
  guard.h = OpenSegment(isolate, name, opts, headerBytes, requested, kindFlags);

  int attachSlot = -1;
  bool ownsRow = false;
  if (!opts.raw) {
    // The takeover path of InitOrJoin rewrites dataBytes; for a size-less
    // join the honest value is the file size minus the header.
    const uint64_t initBytes =
        haveSize ? requested : (guard.h.mappingBytes > headerBytes ? guard.h.mappingBytes - headerBytes : 0);
    InitOrJoin(isolate, guard.h, name, opts, headerBytes, initBytes, kindFlags, &attachSlot,
               &ownsRow);

    // Grow (§5.3): re-enter the init lock and ftruncate up, then re-map so
    // the mapping covers the new size. GrowSegment returns 0 when it lost the
    // lock race — retry the policy against the refreshed header instead of
    // returning a window past the object (review F8).
    Header* h = static_cast<Header*>(guard.h.base);
    if (haveSize && opts.sizePolicy == SizePolicy::kGrow) {
      const double giveUp =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count() + opts.initTimeoutMs;
      while (HeaderDataBytes(h) < requested) {
        const uint64_t grown = GrowSegment(isolate, guard.h, name, opts, headerBytes, requested);
        h = static_cast<Header*>(guard.h.base);  // GrowSegment may re-map
        if (grown == 0) {
          const double nowMs =
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
          if (nowMs >= giveUp) {
            ThrowError(isolate, "E_INIT_TIMEOUT", "segment busy (growing) for too long", name);
          }
          continue;
        }
      }
    }

    const uint64_t nowExisting = HeaderDataBytes(h);
    if (haveSize && opts.sizePolicy == SizePolicy::kExact && nowExisting != requested) {
      ThrowSizeMismatch(isolate, name, requested, nowExisting);
    } else if (haveSize && opts.sizePolicy == SizePolicy::kAtLeast && requested > nowExisting) {
      ThrowSizeMismatch(isolate, name, requested, nowExisting);
    }
  }
  // raw: OpenSegment enforced exact/at-least/grow against st_size before mmap.

  // Authoritative sizes for the Mapping + window. The mapping is the truth:
  // every window is clamped to mappingBytes - headerBytes (review Sec F1) —
  // InitOrJoin already validated the header against this bound, so the clamp
  // is defense in depth.
  const uint64_t maxWindow =
      opts.raw ? guard.h.mappingBytes
               : (guard.h.mappingBytes > headerBytes ? guard.h.mappingBytes - headerBytes : 0);
  const uint64_t headerExisting =
      opts.raw ? 0 : HeaderDataBytes(static_cast<Header*>(guard.h.base));
  const uint64_t wanted =
      haveSize ? requested : (opts.raw ? guard.h.mappingBytes : headerExisting);
  const uint64_t windowBytes = wanted > maxWindow ? maxWindow : wanted;
  const uint64_t mappedDataBytes = windowBytes;

  auto m = std::make_shared<Mapping>();
  m->base = guard.h.base;
  m->mappingBytes = guard.h.mappingBytes;
  m->dataBytes = mappedDataBytes;
  m->headerBytes = headerBytes;
  m->raw = opts.raw;
  m->fd = guard.h.fd;
  m->name = name;
  m->unlinkWhenUnused = opts.unlinkWhenUnused;
  RecordIdentity(*m);
  // Only the Mapping that claimed the attach row releases it (a second
  // Mapping over the same segment in this process shares the row).
  m->attachSlot = ownsRow ? attachSlot : -1;
#ifdef _WIN32
  m->section = guard.h.section;
#endif
  guard.h.base = nullptr;  // ownership moved into the Mapping
  guard.h.fd = -1;
#ifdef _WIN32
  guard.h.section = nullptr;
#endif
  Registry::Get().Put(name, m);

  args.GetReturnValue().Set(MakeWindow(isolate, m, headerBytes, windowBytes));
}

void Unlink(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  if (args.Length() < 1 || !args[0]->IsString()) {
    isolate->ThrowException(v8::Exception::TypeError(Str(isolate, "unlink(name: string)")));
    return;
  }
  v8::String::Utf8Value nameArg(isolate, args[0]);
  const std::string name(*nameArg, nameArg.length());
  ValidateName(isolate, name);

#if defined(_WIN32)
  // The section disappears with the last handle; unlink only prevents new
  // membridge joins by marking the header (§5.4).
  if (auto live = Registry::Get().Find(name)) {
    if (!live->raw && live->base != nullptr) {
      static_cast<Header*>(live->base)->flags |= kFlagUnlinked;
    }
  }
  Registry::Get().Erase(name);
#else
  Registry::Get().Erase(name);
  if (::shm_unlink(name.c_str()) != 0) {
    if (errno == ENOENT) {
      ThrowError(isolate, "E_NOT_FOUND", "segment does not exist", name);
    }
    ThrowSystemError(isolate, "shm_unlink", errno, name);
  }
#endif
  args.GetReturnValue().Set(v8::Undefined(isolate));
}

void Close(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  if (args.Length() < 1 || !args[0]->IsString()) {
    isolate->ThrowException(v8::Exception::TypeError(Str(isolate, "close(name: string)")));
    return;
  }
  v8::String::Utf8Value nameArg(isolate, args[0]);
  const std::string name(*nameArg, nameArg.length());
  // Documented compatibility no-op (§5.4): it cannot unmap (use-after-free
  // under a live SAB) and there is no per-isolate state to release.
  ValidateName(isolate, name);
  args.GetReturnValue().Set(v8::Undefined(isolate));
}

void IsNative(const v8::FunctionCallbackInfo<v8::Value>& args) {
  args.GetReturnValue().Set(v8::Boolean::New(args.GetIsolate(), true));
}

void DebugRegistryHas(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  if (args.Length() < 1 || !args[0]->IsString()) {
    isolate->ThrowException(v8::Exception::TypeError(Str(isolate, "debugRegistryHas(name: string)")));
    return;
  }
  v8::String::Utf8Value nameArg(isolate, args[0]);
  const std::string name(*nameArg, nameArg.length());
  args.GetReturnValue().Set(v8::Boolean::New(isolate, Registry::Get().Find(name) != nullptr));
}

void SelfIdentityJs(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  const Identity id = SelfIdentity();
  v8::Local<v8::Object> o = v8::Object::New(isolate);
  o->Set(ctx, Str(isolate, "pid"), v8::Number::New(isolate, id.pid)).Check();
  o->Set(ctx, Str(isolate, "threadId"), v8::Number::New(isolate, id.threadId)).Check();
  o->Set(ctx, Str(isolate, "startTime"),
         v8::Number::New(isolate, static_cast<double>(id.startTime)))
      .Check();
  o->Set(ctx, Str(isolate, "pidNsInode"),
         v8::Number::New(isolate, static_cast<double>(id.pidNsInode)))
      .Check();
  args.GetReturnValue().Set(o);
}

void SetErrorCtorJs(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  if (args.Length() < 1 || !args[0]->IsFunction()) {
    isolate->ThrowException(
        v8::Exception::TypeError(Str(isolate, "setMembridgeErrorCtor(ctor: function)")));
    return;
  }
  SetErrorCtor(isolate, args[0].As<v8::Function>());
  args.GetReturnValue().Set(v8::Undefined(isolate));
}

template <typename Fn>
void Guarded(const v8::FunctionCallbackInfo<v8::Value>& args, Fn&& fn) {
  try {
    fn();
  } catch (const NativeError&) {
    // JS exception is pending; nothing to clean up (RAII guards ran).
  } catch (...) {
    // Unexpected: report as E_SYSTEM rather than unwind into V8 frames.
    membridge::ThrowSystemError(args.GetIsolate(), "membridge (internal)", 0, "");
  }
}

// ---- §6 sync primitive -----------------------------------------------------

int32_t* WordAddrOf(v8::Isolate* isolate, v8::Local<v8::Value> arg, int32_t index) {
  if (!arg->IsInt32Array()) {
    ThrowError(isolate, "E_NAME_INVALID",
               "sync.wait/notify need an Int32Array view over a membridge segment");
  }
  v8::Local<v8::Int32Array> view = arg.As<v8::Int32Array>();
  const size_t words = view->Length();
  if (index < 0 || static_cast<size_t>(index) >= words) {
    ThrowError(isolate, "E_NAME_INVALID", "word index out of range");
  }
  auto* base = static_cast<int32_t*>(view->Buffer()->Data());
  return base + view->ByteOffset() / 4 + index;
}

void SyncWaitJs(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 3) {
    isolate->ThrowException(v8::Exception::TypeError(
        Str(isolate, "wait(view: Int32Array, index: number, expected: number, timeoutMs?: number)")));
    return;
  }
  const int32_t index = args[1]->Int32Value(ctx).ToChecked();
  int32_t* addr = WordAddrOf(isolate, args[0], index);
  const uint32_t expected = static_cast<uint32_t>(args[2]->Int32Value(ctx).ToChecked());
  double timeoutMs = 0;
  bool hasTimeout = false;
  if (args.Length() >= 4 && args[3]->IsNumber()) {
    timeoutMs = args[3]->NumberValue(ctx).ToChecked();
    // JS layer sends -1 for "no timeout".
    if (timeoutMs >= 0) {
      if (!(timeoutMs > 0)) {
        isolate->ThrowException(v8::Exception::RangeError(Str(isolate, "timeoutMs must be > 0")));
        return;
      }
      hasTimeout = true;
    }
  }
  const WaitResult r =
      SyncWait(addr, expected, hasTimeout ? timeoutMs : std::numeric_limits<double>::quiet_NaN());
  switch (r) {
    case WaitResult::kOk: args.GetReturnValue().Set(Str(isolate, "ok")); return;
    case WaitResult::kNotEqual: args.GetReturnValue().Set(Str(isolate, "not-equal")); return;
    case WaitResult::kTimedOut: args.GetReturnValue().Set(Str(isolate, "timed-out")); return;
  }
}

void SyncNotifyJs(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 2) {
    isolate->ThrowException(v8::Exception::TypeError(
        Str(isolate, "notify(view: Int32Array, index: number, count?: number)")));
    return;
  }
  const int32_t index = args[1]->Int32Value(ctx).ToChecked();
  int32_t* addr = WordAddrOf(isolate, args[0], index);
  int count = 2147483647;
  if (args.Length() >= 3 && !args[2]->IsUndefined()) {
    const double d = args[2]->NumberValue(ctx).ToChecked();
    // JS layer sends -1 for the default (wake all waiters).
    if (d >= 0) {
      count = static_cast<int>(d > 2147483647.0 ? 2147483647.0 : d);
    }
  }
  args.GetReturnValue().Set(v8::Number::New(isolate, SyncWake(addr, count)));
}

void SyncWaitAsyncJs(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 3) {
    isolate->ThrowException(v8::Exception::TypeError(
        Str(isolate, "waitAsync(view: Int32Array, index: number, expected: number, timeoutMs?: number)")));
    return;
  }
  v8::Local<v8::Value> viewVal = args[0];
  const int32_t index = args[1]->Int32Value(ctx).ToChecked();
  int32_t* addr = WordAddrOf(isolate, viewVal, index);
  const uint32_t expected = static_cast<uint32_t>(args[2]->Int32Value(ctx).ToChecked());
  double timeoutMs = 0;
  bool hasTimeout = false;
  if (args.Length() >= 4 && args[3]->IsNumber()) {
    timeoutMs = args[3]->NumberValue(ctx).ToChecked();
    // JS layer sends -1 for "no timeout".
    if (timeoutMs >= 0) {
      if (!(timeoutMs > 0)) {
        isolate->ThrowException(v8::Exception::RangeError(Str(isolate, "timeoutMs must be > 0")));
        return;
      }
      hasTimeout = true;
    }
  }

  v8::Local<v8::Promise::Resolver> resolver =
      v8::Promise::Resolver::New(ctx).ToLocalChecked();
  v8::Local<v8::ArrayBuffer> buf = viewVal.As<v8::Int32Array>()->Buffer();
  const bool ok = StartAsyncWait(isolate, resolver, buf, addr, expected, timeoutMs, hasTimeout);
  if (!ok) {
    v8::Local<v8::Value> err =
        MakeError(isolate, "E_TOO_MANY_WAITERS", "too many pending async waits");
    resolver->Reject(ctx, err).Check();
  }
  args.GetReturnValue().Set(resolver->GetPromise());
}

// ---- §7 mutex support (CAS protocol lives in JS; native does liveness) -----

int32_t* DataAddrOf(v8::Isolate* isolate, v8::Local<v8::Value> arg) {
  if (!arg->IsInt32Array()) {
    ThrowError(isolate, "E_NAME_INVALID", "expected an Int32Array over the mutex data region");
  }
  v8::Local<v8::Int32Array> view = arg.As<v8::Int32Array>();
  return static_cast<int32_t*>(view->Buffer()->Data()) + view->ByteOffset() / 4;
}

// mutexClaimSlot(view) -> { slot, gen, token }
void MutexClaimSlotJs(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 1 || !args[0]->IsString()) {
    isolate->ThrowException(
        v8::Exception::TypeError(Str(isolate, "mutexClaimSlot(name, view)")));
    return;
  }
  v8::String::Utf8Value nameArg(isolate, args[0]);
  int32_t* data = DataAddrOf(isolate, args[1]);
  const uint32_t token = MutexClaimSlot(isolate, data, std::string(*nameArg, nameArg.length()));
  v8::Local<v8::Object> o = v8::Object::New(isolate);
  o->Set(ctx, Str(isolate, "slot"),
         v8::Number::New(isolate, (token & kMutexTokenMask) >> 16)).Check();
  o->Set(ctx, Str(isolate, "gen"),
         v8::Number::New(isolate, token & kMutexGenMask)).Check();
  o->Set(ctx, Str(isolate, "token"), v8::Number::New(isolate, token)).Check();
  args.GetReturnValue().Set(o);
}

// mutexOwnerAlive(view, token) -> bool
void MutexOwnerAliveJs(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 2 || !args[1]->IsNumber()) {
    isolate->ThrowException(
        v8::Exception::TypeError(Str(isolate, "mutexOwnerAlive(view, token)")));
    return;
  }
  int32_t* data = DataAddrOf(isolate, args[0]);
  const uint32_t token = static_cast<uint32_t>(args[1]->NumberValue(ctx).ToChecked());
  args.GetReturnValue().Set(
      v8::Boolean::New(isolate, MutexOwnerAlive(data, token, kMutexHeaderWords, kMutexSlotCount)));
}

// ---- guarded trampolines ---------------------------------------------------
// Every JS entry point catches NativeError (JS exception already pending).
// Anything else becomes an E_SYSTEM — C++ must never unwind into V8 frames.

#define MEMBRIDGE_TRAMPOLINE(JsName, Impl)                                        \
  void JsName(const v8::FunctionCallbackInfo<v8::Value>& args) {                  \
    try {                                                                         \
      Impl(args);                                                                 \
    } catch (const NativeError&) {                                                \
      /* JS exception pending; RAII guards already unwound. */                    \
    } catch (...) {                                                               \
      v8::Isolate* iso = args.GetIsolate();                                       \
      iso->ThrowException(MakeError(iso, "E_SYSTEM", "internal error"));          \
    }                                                                             \
  }

MEMBRIDGE_TRAMPOLINE(OpenJs, Open)
MEMBRIDGE_TRAMPOLINE(UnlinkJs, Unlink)
MEMBRIDGE_TRAMPOLINE(CloseJs, Close)
MEMBRIDGE_TRAMPOLINE(IsNativeJs, IsNative)
MEMBRIDGE_TRAMPOLINE(DebugRegistryHasJs, DebugRegistryHas)
MEMBRIDGE_TRAMPOLINE(SelfIdentityJs2, SelfIdentityJs)
MEMBRIDGE_TRAMPOLINE(SetErrorCtorJs2, SetErrorCtorJs)
MEMBRIDGE_TRAMPOLINE(SyncWaitJs2, SyncWaitJs)
MEMBRIDGE_TRAMPOLINE(SyncNotifyJs2, SyncNotifyJs)
MEMBRIDGE_TRAMPOLINE(SyncWaitAsyncJs2, SyncWaitAsyncJs)
// ringClaimRole(name, view, isProducer) -> token
void RingClaimRoleJs(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  if (args.Length() < 3 || !args[0]->IsString() || !args[2]->IsBoolean()) {
    isolate->ThrowException(v8::Exception::TypeError(
        Str(isolate, "ringClaimRole(name: string, view: Int32Array, isProducer: boolean)")));
    return;
  }
  v8::String::Utf8Value nameArg(isolate, args[0]);
  int32_t* data = DataAddrOf(isolate, args[1]);
  const bool isProducer = args[2]->BooleanValue(isolate);
  const uint32_t token = RingClaimRole(
      isolate, std::string(*nameArg, nameArg.length()), data,
      isProducer ? kRingProducerWord : kRingConsumerWord, isProducer ? 0 : 1);
  args.GetReturnValue().Set(v8::Number::New(isolate, token));
}

// readHeader(name, maxAttach) -> header fields + attach rows (§9 stat)
void ReadHeaderJs(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
  if (args.Length() < 1 || !args[0]->IsString()) {
    isolate->ThrowException(
        v8::Exception::TypeError(Str(isolate, "readHeader(name: string, maxAttach?: number)")));
    return;
  }
  v8::String::Utf8Value nameArg(isolate, args[0]);
  const std::string name(*nameArg, nameArg.length());
  uint32_t maxAttach = 2048;
  if (args.Length() >= 2 && args[1]->IsNumber()) {
    const double d = args[1]->NumberValue(ctx).ToChecked();
    if (d >= 0 && d <= 4096) maxAttach = static_cast<uint32_t>(d);
  }
  HeaderInfo info;
  ReadHeader(isolate, name, maxAttach, &info);
  v8::Local<v8::Object> o = v8::Object::New(isolate);
  o->Set(ctx, Str(isolate, "magic"), v8::Number::New(isolate, info.magic)).Check();
  o->Set(ctx, Str(isolate, "layoutVersion"), v8::Number::New(isolate, info.layoutVersion)).Check();
  o->Set(ctx, Str(isolate, "initState"), v8::Number::New(isolate, info.initState)).Check();
  o->Set(ctx, Str(isolate, "headerBytes"), v8::Number::New(isolate, info.headerBytes)).Check();
  o->Set(ctx, Str(isolate, "flags"), v8::Number::New(isolate, info.flags)).Check();
  o->Set(ctx, Str(isolate, "dataBytes"), v8::Number::New(isolate, static_cast<double>(info.dataBytes))).Check();
  v8::Local<v8::Array> rows = v8::Array::New(isolate, static_cast<int>(info.attach.size()));
  for (size_t i = 0; i < info.attach.size(); i++) {
    const Identity& id = info.attach[i];
    v8::Local<v8::Object> r = v8::Object::New(isolate);
    r->Set(ctx, Str(isolate, "pid"), v8::Number::New(isolate, id.pid)).Check();
    r->Set(ctx, Str(isolate, "threadId"), v8::Number::New(isolate, id.threadId)).Check();
    r->Set(ctx, Str(isolate, "startTime"),
           v8::Number::New(isolate, static_cast<double>(id.startTime))).Check();
    r->Set(ctx, Str(isolate, "pidNsInode"),
           v8::Number::New(isolate, static_cast<double>(id.pidNsInode))).Check();
    r->Set(ctx, Str(isolate, "refcount"),
           v8::Number::New(isolate, info.refcounts[i])).Check();
    rows->Set(ctx, static_cast<uint32_t>(i), r).Check();
  }
  o->Set(ctx, Str(isolate, "attach"), rows).Check();
  args.GetReturnValue().Set(o);
}

MEMBRIDGE_TRAMPOLINE(MutexClaimSlotJs2, MutexClaimSlotJs)
MEMBRIDGE_TRAMPOLINE(MutexOwnerAliveJs2, MutexOwnerAliveJs)
MEMBRIDGE_TRAMPOLINE(RingClaimRoleJs2, RingClaimRoleJs)
MEMBRIDGE_TRAMPOLINE(ReadHeaderJs2, ReadHeaderJs)

void RegisterModule(v8::Local<v8::Object> exports, v8::Local<v8::Context> ctx) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();  // F16: no Context::GetIsolate in V8 14.6
  struct Reg {
    const char* name;
    v8::FunctionCallback fn;
  };
  const Reg regs[] = {
      {"setMembridgeErrorCtor", SetErrorCtorJs2},
      {"open", OpenJs},
      {"unlink", UnlinkJs},
      {"close", CloseJs},
      {"isNative", IsNativeJs},
      {"debugRegistryHas", DebugRegistryHasJs},
      {"selfIdentity", SelfIdentityJs2},
      {"syncWait", SyncWaitJs2},
      {"syncNotify", SyncNotifyJs2},
      {"syncWaitAsync", SyncWaitAsyncJs2},
      {"mutexClaimSlot", MutexClaimSlotJs2},
      {"mutexOwnerAlive", MutexOwnerAliveJs2},
      {"ringClaimRole", RingClaimRoleJs2},
      {"readHeader", ReadHeaderJs2},
  };
  for (const Reg& r : regs) {
    v8::Local<v8::Function> f =
        v8::FunctionTemplate::New(isolate, r.fn)->GetFunction(ctx).ToLocalChecked();
    exports->Set(ctx, Str(isolate, r.name), f).Check();
  }
}

}  // namespace

}  // namespace membridge

NODE_MODULE_INIT(/* exports, module, context */) {
  membridge::RegisterModule(exports, context);
}
