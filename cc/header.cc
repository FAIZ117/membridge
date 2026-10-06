// header.cc — attach-table rows and the §5.2 init state machine.
//
// Geometry rules (fix round 2, review R1–R5):
//   * The header is a HINT. Every open reads magic/version/kind/headerBytes/
//     dataBytes ONCE into locals, validates them, and never re-reads them.
//   * The MAPPING and the file are the truth: after a segment reaches ready,
//     the handle is re-fstat'd and re-mapped so it covers
//     headerBytes + dataBytes (a joiner that mapped only the header page
//     while the creator was sizing now widens to the full object).
//   * Row counts derive from the validated LOCAL headerBytes bounded by the
//     mapping — never from the shared field inside a scan.
//
// Init protocol (fix rounds 2–3, R6–R9/R7):
//   creator: ftruncate the FULL object up front (single truncate — U2-safe),
//     claim row, CAS 0→(1|row<<8), reserve, publish header, state 2, wake.
//   joiner at state 0: grace-wait, then win CAS 0→(1|row<<8) (size-less joins
//     never initialize — they wait, review R6).
//   joiner at INITIALIZING: the word NAMES the initializer's row (R7) — a
//     dead one is handed back via packed→0 so exactly one joiner
//     initializes. The takeover winner claims its row BEFORE the CAS, so the
//     baton can never name a row that is not yet published.

#include "header.h"
#include "liveness.h"
#include "segment.h"
#include "wait.h"

#include <algorithm>
#include <chrono>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace membridge {

int ClaimAttachRow(Header* h, const Identity& self, uint32_t nslots) {
  for (uint32_t i = 0; i < nslots; i++) {
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
  if (slot < 0) return;
  AtomicSlotRefcount(&h->attachTable[slot])->store(0, std::memory_order_release);
}

int ReleaseDeadRows(Header* h, uint32_t nslots) {
  int freed = 0;
  for (uint32_t i = 0; i < nslots; i++) {
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

int EnsureAttachedRow(Header* h, const Identity& self, bool* outOwned, uint32_t nslots) {
  // One row per process: Mapping reuse across isolates shares it.
  *outOwned = false;
  for (uint32_t i = 0; i < nslots; i++) {
    AttachSlot* s = &h->attachTable[i];
    if (AtomicSlotRefcount(s)->load(std::memory_order_acquire) > 0 &&
        s->identity.pid == self.pid && s->identity.threadId == 0 &&
        s->identity.startTime == self.startTime) {
      return static_cast<int>(i);
    }
  }
  *outOwned = true;
  return ClaimAttachRow(h, Identity{self.pid, 0, self.startTime, self.pidNsInode}, nslots);
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

inline double SteadyMs() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Back the object for headerBytes+dataBytes (takeover/creator full sizing)
// and re-map the handle when the mapping does not cover the new size. Only
// the initializer (the state == 1 owner) may call this.
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

struct ErrRestorer {
  // Restores initState when an initializer throws mid-flight (review R8): a
  // live initializer error must not leave the segment wedged. The value to
  // restore depends on where the baton was taken from: a takeover winner
  // restores UNINIT (the header may be half-written — another joiner must be
  // able to take over), a grower restores READY (the old header is intact).
  std::atomic<int32_t>* word = nullptr;
  int32_t restoreValue = kInitReady;
  ~ErrRestorer() {
    if (word != nullptr) {
      word->store(restoreValue, std::memory_order_release);
      SyncWake(reinterpret_cast<int32_t*>(word), 2147483647);
    }
  }
};

}  // namespace

uint64_t EnsureMappingCovers(v8::Isolate* isolate, SegmentHandle& handle, const std::string& name,
                             uint32_t headerBytes, uint64_t maxSegmentBytes) {
  Header* h = AsHeader(handle.base);
  const uint64_t dataBytes = HeaderDataBytesOf(h);  // read ONCE
  if (dataBytes > maxSegmentBytes) {
    ThrowError(isolate, "E_INCOMPATIBLE",
               "segment header declares more data than MEMBRIDGE_MAX_SEGMENT_BYTES allows", name);
  }
#if !defined(_WIN32)
  struct stat st{};
  if (::fstat(handle.fd, &st) != 0) {
    ThrowSystemError(isolate, "fstat", errno, name);
  }
  if (static_cast<uint64_t>(st.st_size) < static_cast<uint64_t>(headerBytes) + dataBytes) {
    ThrowError(isolate, "E_INCOMPATIBLE",
               "segment header describes more data than the segment holds", name);
  }
#endif
  RemapSegment(handle, static_cast<uint64_t>(headerBytes) + dataBytes);
  return dataBytes;
}

void InitOrJoin(v8::Isolate* isolate, SegmentHandle& handle, const std::string& name,
                const OpenOpts& opts, uint32_t headerBytes, uint64_t dataBytes,
                uint32_t kindFlags, int* outSlot, bool* outOwned, uint64_t* outDataBytes,
                bool mayInitialize) {
  Header* h = AsHeader(handle.base);
  const Identity self = SelfIdentity();
  *outSlot = -1;
  *outOwned = false;
  *outDataBytes = dataBytes;

  if (handle.created) {
    // Sole creator (won O_EXCL; OpenSegment already ftruncate'd + reserved the
    // FULL object in one truncate — U2-safe, review R9). Publish the row and
    // flip to INITIALIZING via CAS before the header write, so joiners see a
    // live initializer instead of a takeover window (review R7).
    h->headerBytes = headerBytes;
    std::atomic_thread_fence(std::memory_order_release);
    bool owned = false;
    const int slot = EnsureAttachedRow(h, self, &owned,
                                       RowCountBounded(headerBytes, handle.mappingBytes));
    h->initializerSlot = slot;
    int32_t expected0 = kInitUninit;
    if (!AtomicInitState(h)->compare_exchange_strong(expected0, InitStatePacked(slot),
                                                     std::memory_order_acq_rel)) {
      // A joiner's grace takeover won the word (creator was >250 ms pre-row).
      // The joiner owns initialization now; fall through to the joiner loop
      // and behave like any other joiner against our own object.
    } else {
      WriteHeader(h, headerBytes, dataBytes, kindFlags);
      if (slot < 0) h->flags |= kFlagAttachOverflow;
      h->initializerSlot = slot;
      *outSlot = slot;
      *outOwned = owned;
      *outDataBytes = dataBytes;
      AtomicInitState(h)->store(kInitReady, std::memory_order_release);
      SyncWake(&h->initState, 2147483647);
      return;
    }
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
      // ---- read the geometry ONCE into locals; never re-read it ----
      const uint32_t hb = h->headerBytes;
      const uint32_t magic = h->magic;
      const uint32_t version = h->layoutVersion;
      const uint32_t flags = h->flags;
      const uint64_t hdrData = HeaderDataBytesOf(h);
      if (magic != kMagic || version != kLayoutVersion) {
        ThrowError(isolate, "E_INCOMPATIBLE",
                   "segment is not a membridge segment or has an incompatible layout version",
                   name);
      }
      if (hb != headerBytes) {
        ThrowError(isolate, "E_INCOMPATIBLE",
                   "segment uses a different header page size (" +
                       std::to_string(hb) + " vs " + std::to_string(headerBytes) + ")",
                   name);
      }
      if (kindFlags != kKindPlain && (flags & kKindMask) != kindFlags) {
        ThrowError(isolate, "E_INCOMPATIBLE",
                   "segment kind mismatch (opened as " +
                       std::to_string((kindFlags & kKindMask) >> 8) + ")",
                   name);
      }
      if (hdrData > opts.maxSegmentBytes) {
        ThrowError(isolate, "E_INCOMPATIBLE",
                   "segment header declares more data than MEMBRIDGE_MAX_SEGMENT_BYTES allows",
                   name);
      }
      // The FILE is the truth for a ready segment: re-fstat and re-map so the
      // mapping covers header + data (a joiner that arrived mid-create and
      // mapped only the header page widens here — review R1/R2).
#if !defined(_WIN32)
      struct stat st{};
      if (::fstat(handle.fd, &st) != 0) {
        ThrowSystemError(isolate, "fstat", errno, name);
      }
      if (static_cast<uint64_t>(st.st_size) < static_cast<uint64_t>(headerBytes) + hdrData) {
        ThrowError(isolate, "E_INCOMPATIBLE",
                   "segment header describes more data than the segment holds", name);
      }
#endif
      RemapSegment(handle, static_cast<uint64_t>(headerBytes) + hdrData);
      h = AsHeader(handle.base);  // RemapSegment may have moved the mapping
      bool owned = false;
      const int slot = EnsureAttachedRow(h, self, &owned,
                                         RowCountBounded(hb, handle.mappingBytes));
      if (slot < 0) h->flags |= kFlagAttachOverflow;
      *outSlot = slot;
      *outOwned = owned;
      *outDataBytes = hdrData;
      return;
    }

    if (state == kInitUninit) {
      if (!mayInitialize) {
        // Size-less joins never initialize (review R6): a 0-byte crashed
        // creator's object must not become a ready 0-byte segment. A sized
        // opener elsewhere can still initialize it; wait, then time out.
        if (SteadyMs() >= deadline) {
          ThrowError(isolate, "E_INIT_TIMEOUT",
                     "segment did not reach a ready state within " +
                         std::to_string(static_cast<int64_t>(opts.initTimeoutMs)) + " ms",
                     name);
        }
        const double remaining = deadline - SteadyMs();
        SyncWait(&h->initState, kInitUninit, remaining > 250.0 ? 250.0 : remaining);
        continue;
      }
      // Either a live creator between shm_open and its INITIALIZING store
      // (milliseconds), or one that died before publishing anything. Give it
      // a brief grace unless OpenSegment already grace-waited for the header
      // page, then win the 0 -> 1 CAS — exactly one initializer proceeds.
      if (!handle.waitedForCreator && SteadyMs() - startMs < 50.0) {
        SyncWait(&h->initState, kInitUninit, 10.0);
        continue;
      }
      // Claim our attach row BEFORE winning the baton (review R7): the CAS
      // below publishes the row index INSIDE the state word, so no joiner can
      // ever observe "initializing" without knowing exactly which row holds
      // the initializer — the stale-field takeover race is closed by
      // construction. Losing the CAS only costs a fresh row, released below.
      bool owned = false;
      const int slot = EnsureAttachedRow(h, self, &owned,
                                         RowCountBounded(headerBytes, handle.mappingBytes));
      const int32_t packed = slot >= 0 ? InitStatePacked(slot) : kInitInitializing;
      int32_t expected0 = kInitUninit;
      if (AtomicInitState(h)->compare_exchange_strong(expected0, packed,
                                                      std::memory_order_acq_rel)) {
        ErrRestorer restorer;
        restorer.word = AtomicInitState(h);
        restorer.restoreValue = kInitUninit;  // a failed takeover must hand the baton back
        // Publish ourselves BEFORE any slow work (review R7): other joiners
        // at state 1 must see a live initializer and wait.
        h->headerBytes = headerBytes;
        std::atomic_thread_fence(std::memory_order_release);
        h->initializerSlot = slot;
        EnsureSized(isolate, handle, name, headerBytes, dataBytes, false);
        h = AsHeader(handle.base);  // EnsureSized may have re-mapped
        WriteHeader(h, headerBytes, dataBytes, kindFlags);
        if (slot < 0) h->flags |= kFlagAttachOverflow;
        h->initializerSlot = slot;
        *outSlot = slot;
        *outOwned = owned;
        *outDataBytes = dataBytes;
        restorer.word = nullptr;  // committed: the store below publishes
        AtomicInitState(h)->store(kInitReady, std::memory_order_release);
        SyncWake(&h->initState, 2147483647);
        return;
      }
      // Lost the CAS: someone else holds the baton. A row we claimed fresh is
      // surplus now; a reused process row must stay (refcount is per process).
      if (owned) ReleaseAttachRow(h, slot);
      continue;  // someone else is initializing
    }

    // state == INITIALIZING: the word names the initializer (review R7 —
    // bits 8+ are its attach row). A bare 1 (hand-crafted crash state) falls
    // back to the initializerSlot field.
    const int initSlot =
        state > kInitInitializing ? InitSlotOf(state) : h->initializerSlot;
    const uint32_t hb1 = h->headerBytes;  // local read (review R23)
    const uint32_t n = RowCountBounded(hb1, handle.mappingBytes);
    bool initAlive = false;
    if (initSlot >= 0 && static_cast<uint32_t>(initSlot) < n &&
        AtomicSlotRefcount(&h->attachTable[initSlot])->load(std::memory_order_acquire) > 0) {
      initAlive = CheckLiveness(h->attachTable[initSlot].identity) == Liveness::kAlive;
    }
    if (initAlive || !mayInitialize) {
      // !mayInitialize: a takeover would rewrite geometry we cannot validate
      // (size-less) — wait for a ready state instead (review R6).
      if (SteadyMs() >= deadline) {
        ThrowError(isolate, "E_INIT_TIMEOUT",
                   "segment initializer did not finish within " +
                       std::to_string(static_cast<int64_t>(opts.initTimeoutMs)) + " ms",
                   name);
      }
      const double remaining = deadline - SteadyMs();
      // Wait on the CURRENT word value: it is the packed baton (state bits +
      // row), so parking on it wakes exactly when the initializer commits,
      // fails, or a takeover hands the baton back.
      SyncWait(&h->initState, state, remaining > 250.0 ? 250.0 : remaining);
      continue;
    }

    // Dead or absent initializer (review F5/F26): hand the baton back through
    // UNINIT so exactly one joiner wins the 0 -> packed CAS — no concurrent
    // WriteHeader war, no shrink races between takeover sizes. The CAS target
    // is the whole packed word: only a handback of THIS initializer's baton
    // can win.
    if (AtomicInitState(h)->compare_exchange_strong(state, kInitUninit,
                                                    std::memory_order_acq_rel)) {
      SyncWake(&h->initState, 2147483647);
    }
    // CAS failure means the initializer finished (state moved to ready) or
    // another takeover already reset it: loop and re-read.
  }
}

uint64_t GrowSegment(v8::Isolate* isolate, SegmentHandle& handle, const std::string& name,
                     const OpenOpts& opts, uint32_t headerBytes, uint64_t newDataBytes,
                     uint64_t maxSegmentBytes) {
#if defined(_WIN32)
  (void)handle; (void)opts; (void)headerBytes; (void)newDataBytes; (void)maxSegmentBytes;
  ThrowError(isolate, "E_GROW_UNSUPPORTED",
             "grow is a POSIX-only policy: Windows sections are fixed at CreateFileMappingW time",
             name);
#else
  Header* h = AsHeader(handle.base);

  // Claim our attach row first, then acquire the init lock by CASing
  // ready -> packed-initializing (the baton word names us — review R7).
  // Losers WAIT ON THE WORD (not a poll — review R8) and return 0 so the
  // caller re-evaluates.
  const Identity self = SelfIdentity();
  bool owned = false;
  const int slot = EnsureAttachedRow(h, self, &owned,
                                     RowCountBounded(headerBytes, handle.mappingBytes));
  const int32_t packed = slot >= 0 ? InitStatePacked(slot) : kInitInitializing;
  int32_t state = AtomicInitState(h)->load(std::memory_order_acquire);
  while (state == kInitReady &&
         !AtomicInitState(h)->compare_exchange_weak(state, packed,
                                                    std::memory_order_acq_rel)) {
  }
  if (state != kInitReady) {
    if (owned) ReleaseAttachRow(h, slot);  // surplus row: we did not win the baton
    const double deadline = SteadyMs() + opts.initTimeoutMs;
    for (;;) {
      if (IsReady(h)) return 0;
      if (SteadyMs() >= deadline) {
        ThrowError(isolate, "E_INIT_TIMEOUT", "segment busy (initializing/growing) for too long",
                   name);
      }
      const int32_t cur = AtomicInitState(h)->load(std::memory_order_acquire);
      SyncWait(&h->initState, cur, 50.0);
    }
  }

  ErrRestorer restorer;
  restorer.word = AtomicInitState(h);  // grow failure restores the intact READY header

  // Record ourselves in the field too (byte contract / ops visibility); the
  // word above is the authoritative baton (review F26/R7).
  h->initializerSlot = slot;

  // Never shrink below the REAL file (review F8/R10): the floor comes from
  // st_size — not the shared header — and is bounded by the caller's cap so a
  // racing writer cannot make us ftruncate to an attacker-chosen size.
  struct stat st{};
  if (::fstat(handle.fd, &st) != 0) {
    ThrowSystemError(isolate, "fstat", errno, name);
  }
  const uint64_t byFile =
      static_cast<uint64_t>(st.st_size) > headerBytes
          ? static_cast<uint64_t>(st.st_size) - headerBytes
          : 0;
  uint64_t finalBytes = newDataBytes > byFile ? newDataBytes : byFile;
  if (finalBytes > maxSegmentBytes) finalBytes = maxSegmentBytes;
  // EnsureSized ftruncates (only up) and re-maps so the mapping covers it.
  EnsureSized(isolate, handle, name, headerBytes, finalBytes, false);
  h = AsHeader(handle.base);  // EnsureSized may have re-mapped
  const uint32_t kindBits = h->flags & kKindMask;
  WriteHeader(h, headerBytes, finalBytes, kindBits);
  restorer.word = nullptr;  // committed
  AtomicInitState(h)->store(kInitReady, std::memory_order_release);
  SyncWake(&h->initState, 2147483647);  // review P10: wake joiners
  return finalBytes;
#endif
}

}  // namespace membridge
