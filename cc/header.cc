// header.cc — attach-table rows and the §5.2 init state machine.
//
// initState: 0 = uninit, 1 = initializing, 2 = ready. The initializer claims
// an attach row, records it in initializerSlot, CASes 0 -> 1, finishes sizing
// + header fields, then releases 2. A joiner that finds 1 checks the
// initializer's liveness: dead -> idempotent takeover; alive -> wait until
// ready or E_INIT_TIMEOUT. M2 waits by polling; M3 swaps the poll for a
// native wait on the initState word (same protocol, §6).
//
// A zeroed header (magic 0) means the creator crashed before publishing —
// magic/version are only enforced on ready segments, so takeover can rewrite
// a half-initialized header but never a foreign segment.

#include "header.h"
#include "liveness.h"
#include "segment.h"
#include "wait.h"

#include <chrono>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace membridge {

int ClaimAttachRow(Header* h, const Identity& self) {
  const uint32_t n = AttachSlotCount(h->headerBytes);
  for (uint32_t i = 0; i < n; i++) {
    AttachSlot* s = &h->attachTable[i];
    auto* rc = AtomicSlotRefcount(s);
    int32_t expected = 0;
    if (rc->compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
      s->identity = self;
      s->reserved = 0;
      return static_cast<int>(i);
    }
  }
  return -1;
}

void ReleaseAttachRow(Header* h, int slot) {
  const uint32_t n = AttachSlotCount(h->headerBytes);
  if (slot < 0 || static_cast<uint32_t>(slot) >= n) return;
  AtomicSlotRefcount(&h->attachTable[slot])->store(0, std::memory_order_release);
}

int ReleaseDeadRows(Header* h) {
  const uint32_t n = AttachSlotCount(h->headerBytes);
  int freed = 0;
  for (uint32_t i = 0; i < n; i++) {
    AttachSlot* s = &h->attachTable[i];
    auto* rc = AtomicSlotRefcount(s);
    int32_t cur = rc->load(std::memory_order_acquire);
    if (cur <= 0) continue;
    // Only clear rows we can prove dead; foreign pid-namespace rows stay
    // (liveness unknowable -> alive, §7.1). Identity of pid 0 is never
    // claimed, so it marks a row mid-claim — skip it.
    if (s->identity.pid == 0) continue;
    if (CheckLiveness(s->identity) == Liveness::kDead) {
      if (rc->compare_exchange_strong(cur, 0, std::memory_order_acq_rel)) {
        freed++;
      }
    }
  }
  return freed;
}

int EnsureAttachedRow(Header* h, const Identity& self, bool* outOwned) {
  // One row per process: Mapping reuse across isolates shares it.
  *outOwned = false;
  const uint32_t n = AttachSlotCount(h->headerBytes);
  for (uint32_t i = 0; i < n; i++) {
    AttachSlot* s = &h->attachTable[i];
    if (AtomicSlotRefcount(s)->load(std::memory_order_acquire) > 0 &&
        s->identity.pid == self.pid && s->identity.threadId == 0 &&
        s->identity.startTime == self.startTime) {
      return static_cast<int>(i);
    }
  }
  *outOwned = true;
  return ClaimAttachRow(h, Identity{self.pid, 0, self.startTime, self.pidNsInode});
}

namespace {

Header* AsHeader(void* base) {
  return static_cast<Header*>(base);
}

void WriteHeader(Header* h, uint32_t headerBytes, uint64_t dataBytes, uint32_t kindFlags) {
  h->magic = kMagic;
  h->layoutVersion = kLayoutVersion;
  h->headerBytes = headerBytes;
  h->flags = (h->flags & ~(kKindMask | kFlagUnlinked)) | kindFlags;
  std::atomic_thread_fence(std::memory_order_release);
  // Joiners read dataBytes only after seeing initState == 2 (release).
  std::atomic<uint64_t>* db = reinterpret_cast<std::atomic<uint64_t>*>(&h->dataBytes);
  db->store(dataBytes, std::memory_order_release);
}

bool IsReady(Header* h) {
  return AtomicInitState(h)->load(std::memory_order_acquire) == kInitReady;
}

// Takeover must leave every page of header+dataBytes mapped-and-backed: a
// creator can die before ftruncate, leaving a zero-length object (touching
// pages past EOF would SIGBUS). Idempotent; POSIX only (Windows sections are
// sized at create).
void EnsureSizedOnTakeover(v8::Isolate* isolate, SegmentHandle& handle, const std::string& name,
                           uint32_t headerBytes, uint64_t dataBytes) {
#if defined(_WIN32)
  (void)handle; (void)name;
  if (handle.mappingBytes < static_cast<uint64_t>(headerBytes) + dataBytes) {
    ThrowError(isolate, "E_INCOMPATIBLE",
               "crashed initializer left a smaller Windows section; cannot take over", name);
  }
#else
  struct stat st{};
  if (::fstat(handle.fd, &st) != 0) {
    ThrowSystemError(isolate, "fstat", errno, name);
  }
  const uint64_t target = static_cast<uint64_t>(headerBytes) + dataBytes;
  if (static_cast<uint64_t>(st.st_size) < target) {
    if (::ftruncate(handle.fd, static_cast<off_t>(target)) != 0) {
      ThrowSystemError(isolate, "ftruncate", errno, name);
    }
  }
#endif
}

}  // namespace

void InitOrJoin(v8::Isolate* isolate, SegmentHandle& handle, const std::string& name,
                const OpenOpts& opts, uint32_t headerBytes, uint64_t dataBytes,
                uint32_t kindFlags, int* outSlot, bool* outOwned) {
  Header* h = AsHeader(handle.base);
  const Identity self = SelfIdentity();
  *outSlot = -1;
  *outOwned = false;

  if (handle.created) {
    // Creator path: publish header fields, claim a row, then release ready.
    WriteHeader(h, headerBytes, dataBytes, kindFlags);
    bool owned = false;
    const int slot = EnsureAttachedRow(h, self, &owned);
    if (slot < 0) h->flags |= kFlagAttachOverflow;
    h->initializerSlot = slot;
    *outSlot = slot;
    *outOwned = owned;
    AtomicInitState(h)->store(kInitReady, std::memory_order_release);
    return;
  }

  if (opts.raw) {
    // No header: size checks ran against st_size in the caller.
    return;
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::microseconds(static_cast<int64_t>(opts.initTimeoutMs * 1000));

  for (;;) {
    const int32_t state = AtomicInitState(h)->load(std::memory_order_acquire);

    if (state == kInitReady) {
      if (h->magic != kMagic || h->layoutVersion != kLayoutVersion) {
        ThrowError(isolate, "E_INCOMPATIBLE",
                   "segment is not a membridge segment or has an incompatible layout version",
                   name);
      }
      if (h->headerBytes != headerBytes) {
        ThrowError(isolate, "E_INCOMPATIBLE",
                   "segment uses a different header page size (" +
                       std::to_string(h->headerBytes) + " vs " + std::to_string(headerBytes) + ")",
                   name);
      }
      bool owned = false;
      const int slot = EnsureAttachedRow(h, self, &owned);
      if (slot < 0) h->flags |= kFlagAttachOverflow;
      *outSlot = slot;
      *outOwned = owned;
      return;
    }

    // state 0 or 1: either a crashed creator (header may be zeroed — a ready
    // segment always publishes magic last, before the release of 2) or a
    // half-finished initialization. Take over: idempotent, every field is
    // rewritten from our validated inputs (§5.2).
    int initSlot = h->initializerSlot;
    bool initDead = initSlot < 0;
    if (!initDead) {
      const uint32_t n = AttachSlotCount(h->headerBytes);
      if (static_cast<uint32_t>(initSlot) >= n ||
          AtomicSlotRefcount(&h->attachTable[initSlot])->load(std::memory_order_acquire) <= 0) {
        initDead = true;
      } else if (CheckLiveness(h->attachTable[initSlot].identity) != Liveness::kAlive) {
        initDead = true;
      }
    }

    if (state == kInitUninit || initDead) {
      // Order matters: the header must be written (headerBytes sane) before
      // any attach-row scan — on a zeroed (crashed-creator) page the row
      // count would otherwise underflow.
      EnsureSizedOnTakeover(isolate, handle, name, headerBytes, dataBytes);
      WriteHeader(h, headerBytes, dataBytes, kindFlags);
      bool owned = false;
      const int slot = EnsureAttachedRow(h, self, &owned);
      if (slot < 0) h->flags |= kFlagAttachOverflow;
      h->initializerSlot = slot;
      AtomicInitState(h)->store(kInitReady, std::memory_order_release);
      *outSlot = slot;
      *outOwned = owned;
      return;
    }

    // A live initializer is working: native wait on the initState word (it is
    // i32 and shared — §6), in bounded chunks so initializer death is noticed.
    const double now = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    if (now >= std::chrono::duration<double, std::milli>(deadline.time_since_epoch()).count()) {
      ThrowError(isolate, "E_INIT_TIMEOUT",
                 "segment initializer did not finish within " +
                     std::to_string(static_cast<int64_t>(opts.initTimeoutMs)) + " ms",
                 name);
    }
    SyncWait(&h->initState, static_cast<uint32_t>(state), 250.0);
  }
}

uint64_t GrowSegment(v8::Isolate* isolate, SegmentHandle& handle, const std::string& name,
                     const OpenOpts& opts, uint32_t headerBytes, uint64_t newDataBytes) {
#if defined(_WIN32)
  (void)handle; (void)opts; (void)headerBytes; (void)newDataBytes;
  ThrowError(isolate, "E_GROW_UNSUPPORTED",
             "grow is a POSIX-only policy: Windows sections are fixed at CreateFileMappingW time",
             name);
#else
  (void)headerBytes;
  Header* h = AsHeader(handle.base);

  // Acquire the init lock: CAS ready -> initializing. Losers wait for ready
  // and report 0 so the caller re-evaluates against the updated header —
  // nobody can shrink the segment behind the grower's back.
  int32_t state = AtomicInitState(h)->load(std::memory_order_acquire);
  while (state == kInitReady &&
         !AtomicInitState(h)->compare_exchange_weak(state, kInitInitializing,
                                                    std::memory_order_acq_rel)) {
  }
  if (state != kInitReady) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::microseconds(static_cast<int64_t>(opts.initTimeoutMs * 1000));
    for (;;) {
      if (IsReady(h)) return 0;
      if (std::chrono::steady_clock::now() >= deadline) {
        ThrowError(isolate, "E_INIT_TIMEOUT", "segment busy (initializing/growing) for too long",
                   name);
      }
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
  }

  // We hold the lock. Only grow: a concurrent grower may have extended the
  // file past our snapshot, and ftruncate to a smaller size would lose data.
  struct stat st{};
  if (::fstat(handle.fd, &st) != 0) {
    AtomicInitState(h)->store(kInitReady, std::memory_order_release);
    ThrowSystemError(isolate, "fstat", errno, name);
  }
  const uint64_t target = static_cast<uint64_t>(h->headerBytes) + newDataBytes;
  if (static_cast<uint64_t>(st.st_size) < target) {
    if (::ftruncate(handle.fd, static_cast<off_t>(target)) != 0) {
      const int e = errno;
      AtomicInitState(h)->store(kInitReady, std::memory_order_release);
      ThrowSystemError(isolate, "ftruncate", e, name);
    }
  }
  WriteHeader(h, h->headerBytes, newDataBytes, h->flags & kKindMask);
  AtomicInitState(h)->store(kInitReady, std::memory_order_release);
  return newDataBytes;
#endif
}

}  // namespace membridge
