// header.h — the §5.2 segment header as a byte-level contract. Everything the
// header promises is static_asserted here so the layout cannot drift; the
// tests hand-craft and parse these bytes (test/helpers.ts) as an outside
// cross-check. All fields are little-endian two's complement, standard C
// layout with natural alignment.

#ifndef MEMBRIDGE_HEADER_H_
#define MEMBRIDGE_HEADER_H_

#include "membridge.h"

#include <atomic>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#elif !defined(_WIN32)
#include <unistd.h>  // sysconf
#endif

namespace membridge {

// Header page: one page at mapping offset 0 (size = max(4096, page size)) so
// the data region stays page-aligned and 16 KiB pages (Apple Silicon) work.
constexpr uint32_t kMinHeaderBytes = 4096;

// initState values
constexpr int32_t kInitUninit = 0;
constexpr int32_t kInitInitializing = 1;
constexpr int32_t kInitReady = 2;

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
int ClaimAttachRow(Header* h, const Identity& self);
void ReleaseAttachRow(Header* h, int slot);
// Release rows whose identity is provably dead (§7.1). Returns rows freed.
int ReleaseDeadRows(Header* h);
// Find/claim this process's row (per-process attachment, threadId 0).
// *outOwned is true when this call claimed a fresh row — only the claiming
// Mapping releases it on detach (a second Mapping over the same segment in
// this process, e.g. after grow-replaces-entry, shares the row).
int EnsureAttachedRow(Header* h, const Identity& self, bool* outOwned);

struct SegmentHandle;
struct OpenOpts;

// The §5.2 create/join + init protocol. `handle` is an open segment (POSIX fd
// still open); on success the header is ready, *outSlot holds our attach row
// (-1 for raw or when the table overflowed) and *outOwned whether we claimed it.
void InitOrJoin(v8::Isolate* isolate, SegmentHandle& handle, const std::string& name,
                const OpenOpts& opts, uint32_t headerBytes, uint64_t dataBytes,
                uint32_t kindFlags, int* outSlot, bool* outOwned);

// Grow the data region (POSIX only, §5.3): ftruncate up under the init lock
// and update dataBytes. Returns 0 if another holder is busy (caller retries
// the policy); throws E_GROW_UNSUPPORTED on Windows, E_SYSTEM on failure.
uint64_t GrowSegment(v8::Isolate* isolate, SegmentHandle& handle, const std::string& name,
                     const OpenOpts& opts, uint32_t headerBytes, uint64_t newDataBytes);

}  // namespace membridge

#endif  // MEMBRIDGE_HEADER_H_
