// header.h — the §5.2 segment header as a byte-level contract. Everything the
// header promises is static_asserted here so the layout cannot drift; the
// tests hand-craft and parse these bytes (test/helpers.ts) as an outside
// cross-check. All fields are little-endian two's complement, standard C
// layout with natural alignment.

#ifndef SHM_BRIDGE_HEADER_H_
#define SHM_BRIDGE_HEADER_H_

#include "shm_bridge.h"

#include <atomic>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#elif !defined(_WIN32)
#include <unistd.h>  // sysconf
#endif

namespace shm_bridge {

// Header page: one page at mapping offset 0 (size = max(4096, page size)) so
// the data region stays page-aligned and 16 KiB pages (Apple Silicon) work.
constexpr uint32_t kMinHeaderBytes = 4096;

// initState word (waitable i32). Layout:
//   bits 0-7    state: 0 UNINIT, 1 INITIALIZING, 2 READY
//   bits 8-19   initializer's attach row while INITIALIZING (review R7 — the
//               baton is published atomically with the state, so a joiner can
//               never see "initializing" without knowing which row holds it)
//   bits 20-30  epoch: bumped (never to 0) on every transition INTO
//               INITIALIZING and preserved by READY and by a handback to
//               UNINIT (round-3 C16). A joiner's handback CAS names the whole
//               word, so after the same row is reused by a newer takeover the
//               stale CAS fails instead of handing back a LIVE baton (ABA).
// Rows are bounded by the header page (<= 2047 at 64 KiB pages). Epoch 0 with
// no row bits only appears in hand-crafted/legacy crash states; readers then
// fall back to the `initializerSlot` field.
constexpr int32_t kInitUninit = 0;
constexpr int32_t kInitInitializing = 1;
constexpr int32_t kInitReady = 2;
constexpr int kInitSlotShift = 8;
constexpr int kInitEpochShift = 20;
constexpr uint32_t kInitSlotMask = 0xFFFu;
constexpr uint32_t kInitEpochMask = 0x7FFu;
inline int32_t InitStateOf(int32_t w) { return w & 0xFF; }
inline int InitSlotOf(int32_t w) { return static_cast<int>((static_cast<uint32_t>(w) >> kInitSlotShift) & kInitSlotMask); }
inline uint32_t InitEpochOf(int32_t w) { return (static_cast<uint32_t>(w) >> kInitEpochShift) & kInitEpochMask; }
inline int32_t InitWord(int32_t state, int slot, uint32_t epoch) {
  return static_cast<int32_t>(static_cast<uint32_t>(state) |
                              ((static_cast<uint32_t>(slot) & kInitSlotMask) << kInitSlotShift) |
                              ((epoch & kInitEpochMask) << kInitEpochShift));
}
inline uint32_t NextInitEpoch(int32_t observed) {
  const uint32_t e = (InitEpochOf(observed) + 1) & kInitEpochMask;
  return e == 0 ? 1 : e;
}
// The baton for `slot`, taken from the observed word.
inline int32_t InitStatePacked(int slot, int32_t observed) {
  return InitWord(kInitInitializing, slot, NextInitEpoch(observed));
}

struct AttachSlot {
  Identity identity;  // 24 B @0 — pid, threadId (0 here), startTime, pidNsInode
  int32_t refcount;   // 24 — 0 = free row; claim is CAS 0 -> 1; stays 1 while attached
  int32_t reserved;   // 28
};
static_assert(sizeof(AttachSlot) == 32, "attach slot must be 32 bytes");

struct Header {
  uint32_t magic;           // 0
  uint32_t layoutVersion;   // 4
  int32_t initState;        // 8  (waitable word in M3 — futex/os_sync target)
  int32_t initializerSlot;  // 12 — attach row of the initializer, -1 = none
  uint32_t headerBytes;     // 16
  uint32_t flags;           // 20
  uint64_t dataBytes;       // 24 — authoritative data size (works on Windows too)
  AttachSlot attachTable[]; // 32 — (headerBytes - 32) / 32 rows (~126 @ 4 KiB)
};
static_assert(offsetof(Header, magic) == 0, "header layout drift");
static_assert(offsetof(Header, layoutVersion) == 4, "header layout drift");
static_assert(offsetof(Header, initState) == 8, "header layout drift");
static_assert(offsetof(Header, initializerSlot) == 12, "header layout drift");
static_assert(offsetof(Header, headerBytes) == 16, "header layout drift");
static_assert(offsetof(Header, flags) == 20, "header layout drift");
static_assert(offsetof(Header, dataBytes) == 24, "header layout drift");
static_assert(offsetof(Header, attachTable) == 32, "header layout drift");
static_assert(sizeof(Header) == 32, "header layout drift");

inline uint32_t AttachSlotCount(uint32_t headerBytes) {
  if (headerBytes < sizeof(Header)) return 0;  // unwritten/zeroed header
  return (headerBytes - sizeof(Header)) / sizeof(AttachSlot);
}

// Typed atomic views over the header words. The header page is native-only
// memory (guardrail 4: the user-facing SAB covers the data region only), so
// these are never exposed to JS.
inline std::atomic<int32_t>* AtomicInitState(Header* h) {
  return reinterpret_cast<std::atomic<int32_t>*>(&h->initState);
}
inline std::atomic<int32_t>* AtomicSlotRefcount(AttachSlot* s) {
  return reinterpret_cast<std::atomic<int32_t>*>(&s->refcount);
}

inline uint32_t EffectiveHeaderBytes() {
#if defined(_WIN32)
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  const DWORD pageSize = si.dwPageSize > 0 ? si.dwPageSize : 4096;
  return pageSize > kMinHeaderBytes ? pageSize : kMinHeaderBytes;
#else
  const long pageSize = sysconf(_SC_PAGESIZE);
  uint32_t pagesz = pageSize > 0 ? static_cast<uint32_t>(pageSize) : 4096u;
  return pagesz > kMinHeaderBytes ? pagesz : kMinHeaderBytes;
#endif
}

// Attach-table row ops. Claim wins a CAS on refcount 0 -> 1, then writes the
// identity (the row is exclusively ours while claimed). Returns the row index
// or -1 (table full — caller sets ATTACH_OVERFLOW).
// Find/claim this process's row (per-process attachment, threadId 0).
// *outOwned is true when this call claimed a fresh row — only the claiming
// Mapping releases it on detach (a second Mapping over the same segment in
// this process, e.g. after grow-replaces-entry, shares the row).
// `nslots` is the row count computed ONCE from the validated local
// headerBytes and bounded by the mapping (review R4) — never re-derived from
// the shared headerBytes field inside the scan.
int EnsureAttachedRow(Header* h, const Identity& self, bool* outOwned, uint32_t nslots);
int ClaimAttachRow(Header* h, const Identity& self, uint32_t nslots);
void ReleaseAttachRow(Header* h, int slot);
int ReleaseDeadRows(Header* h, uint32_t nslots);

// Row count bounded by both the (locally validated) headerBytes and the real
// mapping — the only correct way to derive it (review R4).
inline uint32_t RowCountBounded(uint32_t headerBytes, size_t mappingBytes) {
  const uint32_t byHeader = AttachSlotCount(headerBytes);
  const uint64_t byMapping =
      mappingBytes > sizeof(Header) ? (mappingBytes - sizeof(Header)) / sizeof(AttachSlot) : 0;
  return static_cast<uint32_t>(byHeader < byMapping ? byHeader : byMapping);
}

struct SegmentHandle;
struct OpenOpts;

// The §5.2 create/join + init protocol. `handle` is an open segment (POSIX fd
// still open); on success the header is ready, *outSlot holds our attach row
// (-1 for raw or when the table overflowed), *outOwned whether we claimed it,
// and *outDataBytes carries the geometry read ONCE from the header after
// validation (the caller must never re-read it — review R5). `mayInitialize`
// is false for size-less joins, which wait for a ready segment instead of
// taking a dead initializer's place (review R6).
void InitOrJoin(v8::Isolate* isolate, SegmentHandle& handle, const std::string& name,
                const OpenOpts& opts, uint32_t headerBytes, uint64_t dataBytes,
                uint32_t kindFlags, int* outSlot, bool* outOwned, uint64_t* outDataBytes,
                bool mayInitialize);

// Post-ready geometry refresh (review R2/R3): snapshot dataBytes once,
// validate it against the caller's cap and the REAL file size, and re-map the
// handle so the mapping covers headerBytes + dataBytes. Returns the snapshot.
// E_INCOMPATIBLE when the file cannot back the header's claim.
uint64_t EnsureMappingCovers(v8::Isolate* isolate, SegmentHandle& handle, const std::string& name,
                             uint32_t headerBytes, uint64_t maxSegmentBytes);

// Grow the data region (POSIX only, §5.3): ftruncate up under the init lock
// and update dataBytes. Returns 0 if another holder is busy (caller retries
// the policy); throws E_GROW_UNSUPPORTED on Windows, E_SYSTEM on failure.
// On failure the init state is restored to ready before throwing (review R8).
uint64_t GrowSegment(v8::Isolate* isolate, SegmentHandle& handle, const std::string& name,
                     const OpenOpts& opts, uint32_t headerBytes, uint64_t newDataBytes,
                     uint64_t maxSegmentBytes);

}  // namespace shm_bridge

#endif  // SHM_BRIDGE_HEADER_H_
