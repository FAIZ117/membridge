// segment.h — OS-level segment creation/opening and name validation (§5.5).

#ifndef MEMBRIDGE_SEGMENT_H_
#define MEMBRIDGE_SEGMENT_H_

#include "membridge.h"

#include <string>

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
#ifdef _WIN32
  void* section = nullptr;  // HANDLE of the file mapping
#endif
};

// Throws E_EXISTS / E_NOT_FOUND / E_SYSTEM / E_NO_SPACE. `dataBytes` must be
// validated already. For raw segments headerBytes must be 0.
SegmentHandle OpenSegment(v8::Isolate* isolate, const std::string& name, const OpenOpts& opts,
                          uint32_t headerBytes, uint64_t dataBytes, uint32_t kindFlags);

void CloseSegment(SegmentHandle& h);

}  // namespace membridge

#endif  // MEMBRIDGE_SEGMENT_H_
