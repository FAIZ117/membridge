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
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace shm_bridge {

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
        s->identity.startTime == self.startTime &&
        s->identity.pidNsInode == self.pidNsInode) {  // round-4 E4-7
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
  return InitStateOf(AtomicInitState(h)->load(std::memory_order_acquire)) == kInitReady;
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
    const int rc = ReserveBacking(handle.fd, target);
    if (rc != 0) ThrowReserveError(isolate, rc, name);
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
  // The word is resolved from the HANDLE at unwind (round-3: EnsureSized may
  // re-map, so a pointer captured up front could go stale).
  SegmentHandle* handle = nullptr;
  int32_t restoreValue = kInitReady;
  ~ErrRestorer() {
    if (handle != nullptr) {
      Header* h = AsHeader(handle->base);
      AtomicInitState(h)->store(restoreValue, std::memory_order_release);
      SyncWake(&h->initState, 2147483647);
    }
  }
};

// Releases a freshly claimed attach row when an initializer throws (round-3
// C17: a failed takeover used to leak its row for the process's lifetime,
// blocking unlinkWhenUnused and reap). Resolves the header from the handle.
struct InitRowGuard {
  SegmentHandle* handle = nullptr;
  int slot = -1;
  ~InitRowGuard() {
    if (handle != nullptr && slot >= 0) ReleaseAttachRow(AsHeader(handle->base), slot);
  }
};

}  // namespace

uint64_t EnsureMappingCovers(v8::Isolate* isolate, SegmentHandle& handle, const std::string& name,
                             uint32_t headerBytes, uint64_t maxSegmentBytes) {
  Header* h = AsHeader(handle.base);
  const uint64_t dataBytes = HeaderDataBytesOf(h);  // read ONCE
  if (dataBytes > maxSegmentBytes) {
    ThrowError(isolate, "E_INCOMPATIBLE",
               "segment header declares more data than SHM_BRIDGE_MAX_SEGMENT_BYTES allows", name);
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

namespace {

// Data size the real object backs beyond the header page (POSIX: st_size;
// Windows: the mapped view).
uint64_t FileDataBytes(v8::Isolate* isolate, SegmentHandle& handle, const std::string& name,
                       uint32_t headerBytes) {
#if defined(_WIN32)
  (void)isolate; (void)name;
  return handle.mappingBytes > headerBytes ? handle.mappingBytes - headerBytes : 0;
#else
  struct stat st{};
  if (::fstat(handle.fd, &st) != 0) ThrowSystemError(isolate, "fstat", errno, name);
  return static_cast<uint64_t>(st.st_size) > headerBytes
             ? static_cast<uint64_t>(st.st_size) - headerBytes
             : 0;
#endif
}

}  // namespace

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
    int32_t observed = AtomicInitState(h)->load(std::memory_order_acquire);
    int32_t baton = 0;
    bool won = false;
    if (slot >= 0 && InitStateOf(observed) == kInitUninit) {
      h->initializerSlot = slot;
      baton = InitStatePacked(slot, observed);
      won = AtomicInitState(h)->compare_exchange_strong(observed, baton,
                                                        std::memory_order_acq_rel);
    }
    if (won) {
#if defined(__linux__)
      if (opts.reserve) {
        // Reserve only now that the baton names us (round-4 E4-4): joiners
        // see a LIVE initializer and wait instead of taking over mid-reserve.
        // A failed reserve hands the baton back, releases our row and unlinks
        // the name — a bad create never strands a half-initialized name (R9).
        ErrRestorer restorer;
        restorer.handle = &handle;
        restorer.restoreValue = InitWord(kInitUninit, 0, InitEpochOf(baton));
        InitRowGuard rowGuard;
        rowGuard.handle = &handle;
        rowGuard.slot = owned ? slot : -1;
        const int rc = ReserveBacking(handle.fd, static_cast<uint64_t>(headerBytes) + dataBytes);
        if (rc != 0) {
          ::shm_unlink(ObjectName(name, false).c_str());
          ThrowReserveError(isolate, rc, name);
        }
        restorer.handle = nullptr;
        rowGuard.handle = nullptr;
      }
#endif
      WriteHeader(h, headerBytes, dataBytes, kindFlags);
      h->initializerSlot = slot;
      *outSlot = slot;
      *outOwned = owned;
      *outDataBytes = dataBytes;
      AtomicInitState(h)->store(InitWord(kInitReady, 0, InitEpochOf(baton)),
                                std::memory_order_release);
      SyncWake(&h->initState, 2147483647);
      return;
    }
    // A joiner's grace takeover won the word (creator was >250 ms pre-row).
    // The joiner owns initialization now; behave like any other joiner
    // against our own object. A row we claimed fresh is surplus — release it
    // so the joiner path claims (and a Mapping owns) exactly one (C17).
    if (owned) ReleaseAttachRow(h, slot);
  }

  if (opts.raw) {
    // No header: size checks ran against st_size in the caller.
    return;
  }

  // One budget for the whole open: OpenSegment's grace wait counts (C23).
  const double startMs = handle.waitStartMs > 0 ? handle.waitStartMs : SteadyMs();
  const double deadline = startMs + opts.initTimeoutMs;
  auto throwInitTimeout = [&]() {
    ThrowError(isolate, "E_INIT_TIMEOUT",
               "segment did not reach a ready state within " +
                   std::to_string(static_cast<int64_t>(opts.initTimeoutMs)) + " ms",
               name);
  };
  auto parkOn = [&](int32_t word, double maxMs) {
    const double remaining = deadline - SteadyMs();
    if (remaining <= 0) throwInitTimeout();
    SyncWait(&h->initState, static_cast<uint32_t>(word), remaining > maxMs ? maxMs : remaining);
  };

  for (;;) {
    const int32_t state = AtomicInitState(h)->load(std::memory_order_acquire);

    if (InitStateOf(state) == kInitReady) {
      // ---- read the geometry ONCE into locals; never re-read it ----
      const uint32_t hb = h->headerBytes;
      const uint32_t magic = h->magic;
      const uint32_t version = h->layoutVersion;
      const uint32_t flags = h->flags;
      const uint64_t hdrData = HeaderDataBytesOf(h);
      if (magic != kMagic || version != kLayoutVersion) {
        ThrowError(isolate, "E_INCOMPATIBLE",
                   "segment is not a shm-bridge segment or has an incompatible layout version",
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
                   "segment header declares more data than SHM_BRIDGE_MAX_SEGMENT_BYTES allows",
                   name);
      }
      // The FILE is the truth for a ready segment: re-fstat and re-map so the
      // mapping covers header + data (a joiner that arrived mid-create and
      // mapped only the header page widens here — review R1/R2).
#if !defined(_WIN32)
      if (FileDataBytes(isolate, handle, name, headerBytes) < hdrData) {
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

    // Every non-ready pass honours the deadline (round-3 C11: the lost-CAS and
    // handback paths used to `continue` unchecked, so a writer that kept
    // forging a dead baton spun a sized opener past initTimeoutMs).
    if (SteadyMs() >= deadline) throwInitTimeout();

    if (InitStateOf(state) == kInitUninit) {
      if (!mayInitialize) {
        // Size-less joins never initialize (review R6): a 0-byte crashed
        // creator's object must not become a ready 0-byte segment. A sized
        // opener elsewhere can still initialize it; wait, then time out.
        parkOn(state, 250.0);
        continue;
      }
      // Either a live creator between shm_open and its INITIALIZING store
      // (milliseconds), or one that died before publishing anything. Give it
      // a brief grace unless OpenSegment already grace-waited for the header
      // page, then win the 0 -> 1 CAS — exactly one initializer proceeds.
      if (!handle.waitedForCreator && SteadyMs() - startMs < 50.0) {
        parkOn(state, 10.0);
        continue;
      }
      // Claim our attach row BEFORE winning the baton (review R7): the CAS
      // below publishes the row index INSIDE the state word, so no joiner can
      // ever observe "initializing" without knowing exactly which row holds
      // the initializer. Without a row (attach table full) we cannot name
      // ourselves, so we never take the baton — wait for another opener
      // (round-3 C16: a row-less bare baton was judged dead mid-initialization).
      bool owned = false;
      const int slot = EnsureAttachedRow(h, self, &owned,
                                         RowCountBounded(headerBytes, handle.mappingBytes));
      if (slot < 0) {
        parkOn(state, 250.0);
        continue;
      }
      const int32_t baton = InitStatePacked(slot, state);
      int32_t expected = state;
      if (!AtomicInitState(h)->compare_exchange_strong(expected, baton,
                                                       std::memory_order_acq_rel)) {
        // Lost the CAS: someone else holds the baton. A row we claimed fresh
        // is surplus; a reused process row must stay (refcount is per process).
        if (owned) ReleaseAttachRow(h, slot);
        continue;
      }
      ErrRestorer restorer;
      restorer.handle = &handle;
      // A failed takeover hands the baton back (epoch preserved).
      restorer.restoreValue = InitWord(kInitUninit, 0, InitEpochOf(baton));
      InitRowGuard rowGuard;
      rowGuard.handle = &handle;
      rowGuard.slot = owned ? slot : -1;

      // Read whatever header survives ONCE, before rewriting anything (C12):
      // a crashed GROW (or a creator that died mid-WriteHeader) leaves a valid
      // header whose kind and geometry are not ours to discard.
      const uint32_t priorMagic = h->magic;
      const uint32_t priorVersion = h->layoutVersion;
      const uint32_t priorHb = h->headerBytes;
      const uint32_t priorFlags = h->flags;
      const uint64_t priorData = HeaderDataBytesOf(h);
      uint32_t kind = kindFlags;
      uint64_t finalData = dataBytes;
      {
        // Round-4 E4-4: never write a header smaller than the object a
        // (dead) creator already sized — later exact joins of the creator's
        // size would fail forever. The file is the truth, bounded by our cap.
        uint64_t byFile = FileDataBytes(isolate, handle, name, headerBytes);
        if (byFile > opts.maxSegmentBytes) byFile = opts.maxSegmentBytes;
        if (byFile > finalData) finalData = byFile;
      }
      if (priorMagic == kMagic && priorVersion == kLayoutVersion && priorHb == headerBytes) {
        const uint32_t priorKind = priorFlags & kKindMask;
        if (kindFlags != kKindPlain && priorKind != kKindPlain && priorKind != kindFlags) {
          ThrowError(isolate, "E_INCOMPATIBLE",
                     "segment kind mismatch (opened as " +
                         std::to_string((kindFlags & kKindMask) >> 8) + ")",
                     name);
        }
        if (kindFlags == kKindPlain) kind = priorKind;
        // The prior size is a HINT bounded by the real object and our cap.
        uint64_t bounded = priorData;
        const uint64_t byFile = FileDataBytes(isolate, handle, name, headerBytes);
        if (bounded > byFile) bounded = byFile;
        if (bounded > opts.maxSegmentBytes) bounded = opts.maxSegmentBytes;
        if (bounded > finalData) finalData = bounded;
      }

      // Publish ourselves BEFORE any slow work (review R7).
      h->headerBytes = headerBytes;
      std::atomic_thread_fence(std::memory_order_release);
      h->initializerSlot = slot;
      EnsureSized(isolate, handle, name, headerBytes, finalData, false);
      h = AsHeader(handle.base);  // EnsureSized may have re-mapped
      WriteHeader(h, headerBytes, finalData, kind);
      h->initializerSlot = slot;
      *outSlot = slot;
      *outOwned = owned;
      *outDataBytes = finalData;
      restorer.handle = nullptr;  // committed: the store below publishes
      rowGuard.handle = nullptr;  // the caller adopts the row
      AtomicInitState(h)->store(InitWord(kInitReady, 0, InitEpochOf(baton)),
                                std::memory_order_release);
      SyncWake(&h->initState, 2147483647);
      return;
    }

    // INITIALIZING: the word names the initializer (review R7). Legacy /
    // hand-crafted words (no row bits, epoch 0) fall back to the field.
    const int initSlot = (InitSlotOf(state) == 0 && InitEpochOf(state) == 0)
                             ? h->initializerSlot
                             : InitSlotOf(state);
    // Rows bounded by OUR validated headerBytes and the mapping — never the
    // shared field, which a takeover rewrites just after its CAS (C16).
    const uint32_t n = RowCountBounded(headerBytes, handle.mappingBytes);
    bool initAlive = false;
    if (initSlot >= 0 && static_cast<uint32_t>(initSlot) < n &&
        AtomicSlotRefcount(&h->attachTable[initSlot])->load(std::memory_order_acquire) > 0) {
      initAlive = CheckLiveness(h->attachTable[initSlot].identity) == Liveness::kAlive;
    }
    if (initAlive || !mayInitialize) {
      // !mayInitialize: a takeover would rewrite geometry we cannot validate
      // (size-less) — wait for a ready state instead (review R6). Park on the
      // CURRENT word: it wakes exactly when the initializer commits, fails,
      // or a takeover hands the baton back.
      parkOn(state, 250.0);
      continue;
    }

    // Dead or absent initializer (review F5/F26): hand the baton back through
    // UNINIT (epoch preserved) so exactly one joiner wins the next CAS. The
    // CAS names the whole word — row AND epoch — so only a handback of THIS
    // baton can win, even if the row was since reused by a newer takeover.
    int32_t expected = state;
    if (AtomicInitState(h)->compare_exchange_strong(
            expected, InitWord(kInitUninit, 0, InitEpochOf(state)), std::memory_order_acq_rel)) {
      SyncWake(&h->initState, 2147483647);
    }
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
#elif defined(__APPLE__)
  // §5.3 / AGENTS.md guardrail 6 (fact U2): a macOS shm object cannot be
  // ftruncate'd a second time, so grow is refused up front with the
  // documented code instead of failing inside EnsureSized as E_SYSTEM.
  (void)handle; (void)opts; (void)headerBytes; (void)newDataBytes; (void)maxSegmentBytes;
  ThrowError(isolate, "E_GROW_UNSUPPORTED",
             "grow is unsupported on macOS: a POSIX shm object cannot be resized after creation",
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
  if (slot < 0) {
    // Without a row we cannot name ourselves in the baton (C16).
    ThrowError(isolate, "E_INIT_TIMEOUT",
               "attach table is full: cannot take the init baton to grow", name);
  }
  int32_t state = AtomicInitState(h)->load(std::memory_order_acquire);
  int32_t baton = 0;
  bool won = false;
  while (InitStateOf(state) == kInitReady) {
    baton = InitStatePacked(slot, state);
    if (AtomicInitState(h)->compare_exchange_weak(state, baton, std::memory_order_acq_rel)) {
      won = true;
      break;
    }
  }
  if (!won) {
    if (owned) ReleaseAttachRow(h, slot);  // surplus row: we did not win the baton
    const double deadline = SteadyMs() + opts.initTimeoutMs;
    for (;;) {
      if (IsReady(h)) return 0;
      const double remaining = deadline - SteadyMs();
      if (remaining <= 0) {
        ThrowError(isolate, "E_INIT_TIMEOUT", "segment busy (initializing/growing) for too long",
                   name);
      }
      const int32_t cur = AtomicInitState(h)->load(std::memory_order_acquire);
      SyncWait(&h->initState, static_cast<uint32_t>(cur), remaining > 50.0 ? 50.0 : remaining);
    }
  }

  ErrRestorer restorer;
  restorer.handle = &handle;  // grow failure restores the intact READY header
  restorer.restoreValue = InitWord(kInitReady, 0, InitEpochOf(baton));
  InitRowGuard rowGuard;
  rowGuard.handle = &handle;
  rowGuard.slot = owned ? slot : -1;

  // Record ourselves in the field too (byte contract / ops visibility); the
  // word above is the authoritative baton (review F26/R7).
  h->initializerSlot = slot;

  // Never shrink below the REAL file (review F8/R10): the floor comes from
  // st_size — not the shared header — and is bounded by the caller's cap so a
  // racing writer cannot make us ftruncate to an attacker-chosen size.
  const uint64_t byFile = FileDataBytes(isolate, handle, name, headerBytes);
  uint64_t finalBytes = newDataBytes > byFile ? newDataBytes : byFile;
  if (finalBytes > maxSegmentBytes) finalBytes = maxSegmentBytes;
  // EnsureSized ftruncates (only up) and re-maps so the mapping covers it.
  EnsureSized(isolate, handle, name, headerBytes, finalBytes, false);
  h = AsHeader(handle.base);  // EnsureSized may have re-mapped
  const uint32_t kindBits = h->flags & kKindMask;
  WriteHeader(h, headerBytes, finalBytes, kindBits);
  restorer.handle = nullptr;  // committed
  rowGuard.handle = nullptr;  // the caller adopts (or already owns) the row
  AtomicInitState(h)->store(InitWord(kInitReady, 0, InitEpochOf(baton)),
                            std::memory_order_release);
  SyncWake(&h->initState, 2147483647);  // review P10: wake joiners
  return finalBytes;
#endif
}

}  // namespace shm_bridge
