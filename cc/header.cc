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

inline uint64_t HeaderDataBytesOf(Header* h) {
  return reinterpret_cast<std::atomic<uint64_t>*>(&h->dataBytes)->load(std::memory_order_acquire);
}

bool IsReady(Header* h) {
  return AtomicInitState(h)->load(std::memory_order_acquire) == kInitReady;
}

// Back the object for headerBytes+dataBytes and re-map the handle when its
// mapping does not cover the new size. Only the initializer (the state == 1
// owner) may call: it ftruncates, and on Linux optionally reserves the pages
// up front (creator path, F7). POSIX; Windows sections are fixed at create.
void EnsureSized(v8::Isolate* isolate, SegmentHandle& handle, const std::string& name,
                 uint32_t headerBytes, uint64_t dataBytes, bool reserve) {
#if defined(_WIN32)
  (void)handle; (void)name; (void)reserve;
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
#if defined(__linux__)
  if (reserve) {
    const int rc = ::posix_fallocate(handle.fd, 0, static_cast<off_t>(target));
    if (rc != 0) {
      ThrowError(isolate, "E_NO_SPACE", "not enough space to reserve segment", name);
    }
  }
#endif
  RemapSegment(handle, target);
#endif
}

inline double SteadyMs() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
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
    // Sole creator (we won O_EXCL): publish the header page's size field so
    // attach rows are addressable, claim a row, flip to INITIALIZING — from
    // this moment joiners can see a LIVE initializer instead of taking over —
    // then size the object fully and publish ready (review F5).
    h->headerBytes = headerBytes;
    std::atomic_thread_fence(std::memory_order_release);
    bool owned = false;
    const int slot = EnsureAttachedRow(h, self, &owned);
    h->initializerSlot = slot;
    AtomicInitState(h)->store(kInitInitializing, std::memory_order_release);
    SyncWake(&h->initState, 2147483647);
    EnsureSized(isolate, handle, name, headerBytes, dataBytes, opts.reserve);
    h = AsHeader(handle.base);  // EnsureSized may have re-mapped
    WriteHeader(h, headerBytes, dataBytes, kindFlags);
    if (slot < 0) h->flags |= kFlagAttachOverflow;
    h->initializerSlot = slot;  // WriteHeader cleared it; restore
    *outSlot = slot;
    *outOwned = owned;
    AtomicInitState(h)->store(kInitReady, std::memory_order_release);
    SyncWake(&h->initState, 2147483647);
    return;
  }

  if (opts.raw) {
    // No header: size checks ran against st_size in the caller.
    return;
  }

  const double deadline = SteadyMs() + opts.initTimeoutMs;
  const double startMs = SteadyMs();

  for (;;) {
    int32_t state = AtomicInitState(h)->load(std::memory_order_acquire);

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
      // Geometry truth (review Sec F1): the header's dataBytes is a HINT
      // bounded by the real mapping — a hostile or stale header can never
      // size a BackingStore past mapped memory.
      const uint64_t maxWindow =
          handle.mappingBytes >= headerBytes ? handle.mappingBytes - headerBytes : 0;
      if (HeaderDataBytesOf(h) > maxWindow) {
        ThrowError(isolate, "E_INCOMPATIBLE",
                   "segment header describes more data than the segment holds", name);
      }
      // Kind check (review F33): a non-plain join must land on its own kind.
      if (kindFlags != kKindPlain && (h->flags & kKindMask) != kindFlags) {
        ThrowError(isolate, "E_INCOMPATIBLE",
                   "segment kind mismatch (opened as " +
                       std::to_string((kindFlags & kKindMask) >> 8) + ")", name);
      }
      bool owned = false;
      const int slot = EnsureAttachedRow(h, self, &owned);
      if (slot < 0) h->flags |= kFlagAttachOverflow;
      *outSlot = slot;
      *outOwned = owned;
      return;
    }

    if (state == kInitUninit) {
      // Either a live creator between shm_open and its INITIALIZING store
      // (milliseconds), or one that died before publishing anything. Give it
      // a brief grace unless OpenSegment already grace-waited for the header
      // page, then win the 0 -> 1 CAS — exactly one initializer proceeds.
      if (!handle.waitedForCreator && SteadyMs() - startMs < 50.0) {
        SyncWait(&h->initState, kInitUninit, 10.0);
        continue;
      }
      if (AtomicInitState(h)->compare_exchange_strong(state, kInitInitializing,
                                                      std::memory_order_acq_rel)) {
        // We are the initializer now. Order: size (and re-map), then publish
        // the header's size field, claim the row, then the full header — the
        // attach-row scan needs a sane headerBytes, never a zeroed page.
        EnsureSized(isolate, handle, name, headerBytes, dataBytes, false);
        h = AsHeader(handle.base);  // EnsureSized may have re-mapped
        h->headerBytes = headerBytes;
        std::atomic_thread_fence(std::memory_order_release);
        bool owned = false;
        const int slot = EnsureAttachedRow(h, self, &owned);
        h->initializerSlot = slot;
        WriteHeader(h, headerBytes, dataBytes, kindFlags);
        if (slot < 0) h->flags |= kFlagAttachOverflow;
        h->initializerSlot = slot;  // WriteHeader cleared it; restore
        *outSlot = slot;
        *outOwned = owned;
        AtomicInitState(h)->store(kInitReady, std::memory_order_release);
        SyncWake(&h->initState, 2147483647);
        return;
      }
      continue;  // lost the CAS: someone else is initializing
    }

    // state == INITIALIZING: is the initializer alive?
    const int initSlot = h->initializerSlot;
    // Sec F3: the row count is bounded by the MAPPING, never the (shared,
    // attacker-writable) headerBytes field.
    const uint32_t n = AttachSlotCount(std::min(h->headerBytes,
                                                static_cast<uint32_t>(handle.mappingBytes)));
    bool initAlive = false;
    if (initSlot >= 0 && static_cast<uint32_t>(initSlot) < n &&
        AtomicSlotRefcount(&h->attachTable[initSlot])->load(std::memory_order_acquire) > 0) {
      initAlive = CheckLiveness(h->attachTable[initSlot].identity) == Liveness::kAlive;
    }
    if (initAlive) {
      if (SteadyMs() >= deadline) {
        ThrowError(isolate, "E_INIT_TIMEOUT",
                   "segment initializer did not finish within " +
                       std::to_string(static_cast<int64_t>(opts.initTimeoutMs)) + " ms",
                   name);
      }
      const double remaining = deadline - SteadyMs();
      SyncWait(&h->initState, kInitInitializing, remaining > 250.0 ? 250.0 : remaining);
      continue;
    }

    // Dead or absent initializer (review F5/F26): hand the baton back through
    // UNINIT so exactly one joiner wins the 0 -> 1 CAS — no concurrent
    // WriteHeader war, no shrink races between takeover sizes.
    if (AtomicInitState(h)->compare_exchange_strong(state, kInitUninit,
                                                    std::memory_order_acq_rel)) {
      SyncWake(&h->initState, 2147483647);
    }
    // CAS failure means the initializer finished (state moved to ready) or
    // another takeover already reset it: loop and re-read.
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
  Header* h = AsHeader(handle.base);

  // Acquire the init lock: CAS ready -> initializing. Losers wait for ready
  // and return 0 so the caller re-evaluates against the updated header.
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

  // Record ourselves as the initializer (review F26): joiners arriving
  // mid-grow now wait on OUR liveness; a crashed grower is recoverable.
  {
    const Identity self = SelfIdentity();
    bool owned = false;
    h->initializerSlot = EnsureAttachedRow(h, self, &owned);
  }

  // Never shrink (review F8): a concurrent grower may have extended the
  // object past our snapshot; the header records max(current, requested).
  struct stat st{};
  if (::fstat(handle.fd, &st) != 0) {
    AtomicInitState(h)->store(kInitReady, std::memory_order_release);
    ThrowSystemError(isolate, "fstat", errno, name);
  }
  const uint64_t current =
      static_cast<uint64_t>(st.st_size) > headerBytes
          ? static_cast<uint64_t>(st.st_size) - headerBytes
          : 0;
  const uint64_t target = newDataBytes > current ? newDataBytes : current;
  const uint64_t existing = HeaderDataBytesOf(h);
  const uint64_t finalBytes = target > existing ? target : existing;
  // EnsureSized ftruncates (only up) and re-maps so the mapping covers it.
  const uint32_t kindBits = h->flags & kKindMask;
  EnsureSized(isolate, handle, name, headerBytes, finalBytes, false);
  h = AsHeader(handle.base);  // EnsureSized may have re-mapped
  WriteHeader(h, headerBytes, finalBytes, kindBits);
  AtomicInitState(h)->store(kInitReady, std::memory_order_release);
  SyncWake(&h->initState, 2147483647);  // review P10: wake joiners
  return finalBytes;
#endif
}

}  // namespace membridge
