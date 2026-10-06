// membridge.h — shared definitions for the membridge native addon.
// Design: PLAN.md §3–§5. One Mapping owns the OS mapping (`base`, which is
// what mmap/MapViewOfFile returned); every SAB is a BackingStore-level window
// over `base + headerBytes` (F15: a SAB's length always equals its
// BackingStore's, so windows are made at the BackingStore level).

#ifndef MEMBRIDGE_MEMBRIDGE_H_
#define MEMBRIDGE_MEMBRIDGE_H_

#include <node.h>
#include <v8.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace membridge {

constexpr uint32_t kMagic = 0x424D454Du;  // "MEMB" little-endian
constexpr uint32_t kLayoutVersion = 1;

constexpr uint64_t kDefaultMaxSegmentBytes = 256ull * 1024 * 1024;
constexpr uint64_t kHardMaxSegmentBytes = 4ull * 1024 * 1024 * 1024;  // u64 field, sane upper bound

// Header flags (§5.2)
constexpr uint32_t kFlagAttachOverflow = 1u << 0;
constexpr uint32_t kFlagUnlinked = 1u << 1;
constexpr uint32_t kKindMask = 0xFFu << 8;
constexpr uint32_t kKindPlain = 0u << 8;
constexpr uint32_t kKindMutex = 1u << 8;
constexpr uint32_t kKindRing = 2u << 8;

enum class Mode { kCreateOrJoin, kCreate, kJoin };
enum class SizePolicy { kExact, kAtLeast, kGrow };
enum class Kind { kPlain, kMutex, kRing };

struct OpenOpts {
  Mode mode = Mode::kCreateOrJoin;
  SizePolicy sizePolicy = SizePolicy::kExact;
  bool reserve = true;
  uint32_t permissions = 0600;
  double initTimeoutMs = 5000;
  bool raw = false;
  bool winGlobal = false;
  uint64_t maxSegmentBytes = kDefaultMaxSegmentBytes;
};

// §7.1 participant identity — byte-exact, 24 bytes (shared with §7.2 mutex
// slots). `threadId` is 0 for process-level rows (attach table); mutex slots
// store the OS thread id.
struct Identity {
  int32_t pid;
  int32_t threadId;
  int64_t startTime;
  int64_t pidNsInode;
};
static_assert(sizeof(Identity) == 24, "identity must be 24 bytes");

// The OS mapping. The destructor unmaps `base` — never the window pointer a
// BackingStore was given — and releases the POSIX fd / Windows section handle.
// May run on any thread (V8 GC finalizer).
struct Mapping {
  void* base = nullptr;
  size_t mappingBytes = 0;  // headerBytes + dataBytes (raw: just dataBytes)
  uint64_t dataBytes = 0;
  uint32_t headerBytes = 0;
  bool raw = false;
  std::string name;
  int fd = -1;  // POSIX: kept open so `grow` can ftruncate
  int attachSlot = -1;  // claimed attach-table row, -1 = none (raw or overflow)
  std::atomic<int> bsCount{0};  // live BackingStores over this mapping
  std::atomic<bool> detached{false};
#ifdef _WIN32
  void* section = nullptr;  // HANDLE of the file mapping
#endif

  Mapping();
  ~Mapping();
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;

  // Clear our attach-table row exactly once (idempotent).
  void Detach();
};

// Errors (§10). Each Throw* sets the pending JS exception (a MembridgeError
// with the code plus structured fields) and throws NativeError, which every
// JS entry point catches — so C++ error paths unwind RAII guards and never
// cross into V8 frames. `syscall`/`errno` populate E_SYSTEM details.
struct NativeError {};

[[noreturn]] void ThrowError(v8::Isolate* isolate, const char* code, const std::string& message);
[[noreturn]] void ThrowError(v8::Isolate* isolate, const char* code, const std::string& message,
                             const std::string& name);
[[noreturn]] void ThrowError(v8::Isolate* isolate, const char* code, const std::string& message,
                             const std::string& name, double requested, double existing);
[[noreturn]] void ThrowSystemError(v8::Isolate* isolate, const std::string& syscall, int err,
                                   const std::string& name);

// Set once per isolate from index.ts so native throws real MembridgeError
// instances. Per-isolate: a v8::Global belongs to exactly one isolate, and
// every worker isolate loads the addon separately.
void SetErrorCtor(v8::Isolate* isolate, v8::Local<v8::Function> ctor);

}  // namespace membridge

#endif  // MEMBRIDGE_MEMBRIDGE_H_
