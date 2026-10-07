// segment.h — OS-level segment creation/opening and name validation (§5.5).

#ifndef MEMBRIDGE_SEGMENT_H_
#define MEMBRIDGE_SEGMENT_H_

#include "membridge.h"

#include <string>
#include <vector>

namespace membridge {

// Validate a segment name per platform (§5.5). Throws E_NAME_INVALID.
void ValidateName(v8::Isolate* isolate, const std::string& name);

// POSIX shm object name is the name itself (must start with '/'). Windows
// maps the name to Local\membridge<escaped> (or Global\ with the opt-in),
// percent-encoding '/' -> %2F and '%' -> %25 (§5.5); overflow of the kernel
// object-name limit is E_NAME_INVALID, never truncation.
std::string ObjectName(const std::string& name, bool winGlobal);

// Create/open the OS object and map `headerBytes + dataBytes` (raw: just
// dataBytes). On success fills *outBase, *outMappingBytes, *outFd (POSIX; the
// caller keeps the fd until sizing/init completes, then closes it) and sets
// *outCreated (true if this call created the object).
struct SegmentHandle {
  void* base = nullptr;
  size_t mappingBytes = 0;
  int fd = -1;  // POSIX only; -1 on Windows
  bool created = false;
  // POSIX: OpenSegment grace-waited for a creator that never published even
  // the header page — InitOrJoin may take over state 0 without waiting again.
  bool waitedForCreator = false;
  // Steady-clock ms at which OpenSegment started waiting for a creator (0 =
  // it did not wait). InitOrJoin's initTimeoutMs deadline starts here, so the
  // grace wait and the init wait share ONE budget (round-3 C23).
  double waitStartMs = 0;
#ifdef _WIN32
  void* section = nullptr;  // HANDLE of the file mapping
#endif
};

// Throws E_EXISTS / E_NOT_FOUND / E_SYSTEM / E_NO_SPACE. `dataBytes` must be
// validated already. For raw segments headerBytes must be 0.
SegmentHandle OpenSegment(v8::Isolate* isolate, const std::string& name, const OpenOpts& opts,
                          uint32_t headerBytes, uint64_t dataBytes, uint32_t kindFlags);

void CloseSegment(SegmentHandle& h);

// POSIX: munmap + re-mmap `h` at `newTotal` bytes (grow/takeover extended
// the object past the joiner's original, size-of-truth mapping). No-op when
// the mapping already covers newTotal. Windows sections are fixed-size.
#if defined(__linux__)
// posix_fallocate with EINTR retry (round-3 C21). Returns 0 or an errno.
int ReserveBacking(int fd, uint64_t bytes);
// ENOSPC/EFBIG -> E_NO_SPACE; anything else -> E_SYSTEM (posix_fallocate).
[[noreturn]] void ThrowReserveError(v8::Isolate* isolate, int rc, const std::string& name);
#endif

void RemapSegment(SegmentHandle& h, uint64_t newTotal);

// §9 stat: read the §5.2 header of `name` without mapping the data region
// (POSIX pread; Windows read-only view). Throws E_NOT_FOUND / E_INCOMPATIBLE.
struct HeaderInfo {
  uint32_t magic;
  uint32_t layoutVersion;
  int32_t initState;
  uint32_t headerBytes;
  uint32_t flags;
  uint64_t dataBytes;
  std::vector<Identity> attach;
  std::vector<int32_t> refcounts;
};
void ReadHeader(v8::Isolate* isolate, const std::string& name, uint32_t maxAttach,
                HeaderInfo* out);

}  // namespace membridge
#endif  // MEMBRIDGE_SEGMENT_H_
