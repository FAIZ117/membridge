// segment.cc — shm_open/ftruncate/fallocate/mmap on POSIX;
// CreateFileMappingW/MapViewOfFile on Windows. Size comes in as a double from
// JS and is handled as int64/uint64 — never Uint32Value (F5).

#include "segment.h"

#include "liveness.h"

#include "header.h"

#include <cerrno>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace membridge {

namespace {

// POSIX shm names: start with '/', no other '/', <= 250 bytes on Linux;
// macOS limits names to PSHMNAMLEN (31) — if U2 holds this is the hard limit,
// so enforce it there (PLAN §5.5).
void CheckNameChars(const std::string& name, size_t maxLen) {
  if (name.size() < 2 || name[0] != '/') {
    ThrowError(v8::Isolate::GetCurrent(), "E_NAME_INVALID",
               "segment name must start with '/'", name);
  }
  for (char c : name) {
    if (c == '\0') {
      ThrowError(v8::Isolate::GetCurrent(), "E_NAME_INVALID",
                 "segment name contains NUL", name);
    }
  }
  if (name.find('/', 1) != std::string::npos) {
    ThrowError(v8::Isolate::GetCurrent(), "E_NAME_INVALID",
               "segment name must not contain '/' after the leading slash", name);
  }
  if (name.size() > maxLen) {
    ThrowError(v8::Isolate::GetCurrent(), "E_NAME_INVALID",
               "segment name exceeds platform limit of " + std::to_string(maxLen) + " bytes", name);
  }
}

#ifdef _WIN32
std::wstring ToWide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}
#endif

}  // namespace

void ValidateName(v8::Isolate* isolate, const std::string& name) {
#ifdef _WIN32
  (void)isolate;
  // Windows: same character rules, but the kernel limit applies to the
  // escaped object name (checked in ObjectName below).
  CheckNameChars(name, 250);
#else
  (void)isolate;
#if defined(__APPLE__)
  CheckNameChars(name, 31);  // PSHMNAMLEN if U2 holds (§5.5)
#else
  CheckNameChars(name, 250);
#endif
#endif
}

std::string ObjectName(const std::string& name, bool winGlobal) {
#if defined(_WIN32)
  std::string escaped;
  escaped.reserve(name.size() * 3 + 32);
  for (char c : name) {
    if (c == '/') escaped += "%2F";
    else if (c == '%') escaped += "%25";
    else escaped += c;
  }
  // "Local\membridge" prefix + escaped name; NT object names are limited to
  // 255 UTF-16 chars — exceeding it is E_NAME_INVALID, never truncation.
  const std::string prefix = winGlobal ? "Global\\membridge" : "Local\\membridge";
  if (prefix.size() + escaped.size() > 255) {
    ThrowError(v8::Isolate::GetCurrent(), "E_NAME_INVALID",
               "escaped object name exceeds the 255-char kernel limit", name);
  }
  return prefix + escaped;
#else
  (void)winGlobal;
  return name;
#endif
}

SegmentHandle OpenSegment(v8::Isolate* isolate, const std::string& name, const OpenOpts& opts,
                          uint32_t headerBytes, uint64_t dataBytes, uint32_t kindFlags) {
  (void)kindFlags;
  // dataBytes == 0 (join at existing size): map the whole object — POSIX via
  // st_size, Windows via a full-section view. Creating without a size is
  // rejected by the caller before we get here.
  bool wholeObject = dataBytes == 0;
  const uint64_t requestedTotal = opts.raw ? dataBytes : static_cast<uint64_t>(headerBytes) + dataBytes;
  SegmentHandle h;

#if defined(_WIN32)
  (void)isolate;
  const std::wstring wname = ToWide(ObjectName(name, opts.winGlobal));
  // Review F27: join must not create. Probe for the existing section first;
  // only create/create-or-join fall through to CreateFileMappingW.
  if (opts.mode == Mode::kJoin) {
    HANDLE existing = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wname.c_str());
    if (existing == nullptr) {
      ThrowError(isolate, "E_NOT_FOUND", "segment does not exist", name);
    }
    void* joined = MapViewOfFile(existing, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (joined == nullptr) {
      const int e = static_cast<int>(GetLastError());
      CloseHandle(existing);
      ThrowSystemError(isolate, "MapViewOfFile", e, name);
    }
    // Review R16: the mapping size is needed by every downstream check —
    // query it from the view instead of storing 0 (which made every join
    // fail E_INCOMPATIBLE and every window clamp to zero).
    MEMORY_BASIC_INFORMATION mbi{};
    size_t mapped = 0;
    if (VirtualQuery(joined, &mbi, sizeof(mbi)) != 0) {
      mapped = static_cast<size_t>(mbi.RegionSize);
    }
    h.base = joined;
    h.mappingBytes = mapped;
    h.fd = -1;
    h.created = false;
    h.section = existing;
    return h;
  }
  DWORD high = 0, low = 0;
  if (!wholeObject) {
    high = static_cast<DWORD>(requestedTotal >> 32);
    low = static_cast<DWORD>(requestedTotal & 0xFFFFFFFFu);
  }
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, high, low, wname.c_str());
  if (section == nullptr) {
    ThrowSystemError(isolate, "CreateFileMappingW", static_cast<int>(GetLastError()), name);
  }
  const bool exists = GetLastError() == ERROR_ALREADY_EXISTS;
  if (exists && opts.mode == Mode::kCreate) {
    CloseHandle(section);
    ThrowError(isolate, "E_EXISTS", "segment already exists", name);
  }
  // An EXISTING section may have been created with a different size (a
  // different kind or a racing opener) — a view larger than the section is
  // refused by the kernel (ACCESS_DENIED, which surfaced as a misleading
  // E_SYSTEM in the kind-mismatch path). Map the WHOLE existing section and
  // let the header's kind/size checks below throw the real errors.
  const bool mapWhole = wholeObject || exists;
  void* base = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0,
                             mapWhole ? 0 : static_cast<SIZE_T>(requestedTotal));
  if (base == nullptr) {
    const int e = static_cast<int>(GetLastError());
    CloseHandle(section);
    ThrowSystemError(isolate, "MapViewOfFile", e, name);
  }
  size_t mapped = static_cast<size_t>(requestedTotal);
  if (mapWhole) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(base, &mbi, sizeof(mbi)) != 0) {
      mapped = static_cast<size_t>(mbi.RegionSize);
    }
  }
  h.base = base;
  h.mappingBytes = mapped;
  h.fd = -1;
  h.created = !exists;
  h.section = section;
  return h;
#else
  const std::string obj = ObjectName(name, opts.winGlobal);
  int fd = -1;
  bool created = false;
  if (opts.mode == Mode::kJoin) {
    fd = ::shm_open(obj.c_str(), O_RDWR, 0);
    if (fd < 0) {
      if (errno == ENOENT) {
        ThrowError(isolate, "E_NOT_FOUND", "segment does not exist", name);
      }
      ThrowSystemError(isolate, "shm_open", errno, name);
    }
  } else {
    fd = ::shm_open(obj.c_str(), O_CREAT | O_EXCL | O_RDWR, static_cast<mode_t>(opts.permissions));
    if (fd >= 0) {
      created = true;
    } else if (errno == EEXIST) {
      if (opts.mode == Mode::kCreate) {
        ThrowError(isolate, "E_EXISTS", "segment already exists", name);
      }
      fd = ::shm_open(obj.c_str(), O_RDWR, 0);
    } else {
      ThrowSystemError(isolate, "shm_open", errno, name);
    }
    if (fd < 0) {
      ThrowSystemError(isolate, "shm_open", errno, name);
    }
  }
  // Round-4 S4-5: every throw below must release the fd (several paths used
  // to leak it — repeated failing opens ended in EMFILE for the process).
  struct FdGuard {
    int& fd;
    ~FdGuard() {
      if (fd >= 0) ::close(fd);
    }
  } fdGuard{fd};

  uint64_t total = requestedTotal;
  {
    // The MAPPING is the single source of truth for geometry (review Sec
    // F1/F2): a joiner never maps more bytes than the object actually holds,
    // so a hostile or stale header can never size a BackingStore past the
    // mapping. POSIX only; Windows sections are sized at CreateFileMappingW
    // time.
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
      const int e = errno;
      ::close(fd);
        fd = -1;
      ThrowSystemError(isolate, "fstat", e, name);
    }
    // Only a regular shm object is a segment (round-4 S4-2/S4-5): a FIFO,
    // device or directory planted at the name must fail cleanly, not block,
    // hang in a grace loop or fail deep inside ftruncate. Linux-only: glibc
    // shm_open maps names to /dev/shm paths, where the attack lives; darwin's
    // shm namespace is opaque (fstat there is not S_IFREG) and unreachable by
    // hostile filesystem paths.
#if defined(__linux__)
    if (!S_ISREG(st.st_mode)) {
      if (created) ::shm_unlink(obj.c_str());
      ThrowError(isolate, "E_INCOMPATIBLE", "segment name refers to a non-regular file", name);
    }
#endif
    off_t fileSize = st.st_size;

    if (opts.raw) {
      if (created) {
        // Raw creator: no header, no init protocol — size the whole object
        // now (full sizing via InitOrJoin needs the header protocol).
        if (::ftruncate(fd, static_cast<off_t>(requestedTotal)) != 0) {
          const int e = errno;
          ::close(fd);
        fd = -1;
          ::shm_unlink(obj.c_str());
          ThrowSystemError(isolate, "ftruncate", e, name);
        }
#if defined(__linux__)
        if (opts.reserve) {
          const int rc = ReserveBacking(fd, requestedTotal);
          if (rc != 0) {
            ::close(fd);
        fd = -1;
            ::shm_unlink(obj.c_str());
            ThrowReserveError(isolate, rc, name);
          }
        }
#endif
        // total stays requestedTotal
      } else if (!wholeObject) {
        if (static_cast<uint64_t>(fileSize) < requestedTotal) {
          if (opts.sizePolicy == SizePolicy::kGrow) {
            // raw grow: no header/init lock exists — foreign-mode tradeoff
            if (::ftruncate(fd, static_cast<off_t>(requestedTotal)) != 0) {
              ThrowSystemError(isolate, "ftruncate", errno, name);
            }
            fileSize = static_cast<off_t>(requestedTotal);
          } else {
            ThrowError(isolate, "E_SIZE_MISMATCH",
                       "raw segment is smaller than requested", name,
                       static_cast<double>(requestedTotal), static_cast<double>(fileSize));
          }
        }
      }
      if (!created && wholeObject) total = static_cast<uint64_t>(fileSize);
    } else if (created) {
      // Creator: size the WHOLE object in one ftruncate (review R9 — two
      // truncates would break macOS U2, and Open's window logic assumes the
      // mapping covers header+data). Failure unlinks the object so a bad
      // create never strands a half-sized name (review R9). The (slow)
      // posix_fallocate reserve runs in InitOrJoin AFTER the creator holds
      // the init baton (round-4 E4-4: reserving here, before the baton,
      // let a joiner's 50 ms grace expire mid-fallocate and take over a LIVE
      // creator's segment at the joiner's size).
      if (::ftruncate(fd, static_cast<off_t>(requestedTotal)) != 0) {
        const int e = errno;
        ::close(fd);
        fd = -1;
        ::shm_unlink(obj.c_str());
        ThrowSystemError(isolate, "ftruncate", e, name);
      }
      total = requestedTotal;
    } else {
      // Joiner: wait briefly for a mid-init creator to publish the header
      // page; a creator that died before even that is taken over after the
      // grace period (everyone truncates to exactly headerBytes — idempotent,
      // no shrink war) and the §5.2 CAS serializes real initialization.
      if (fileSize < static_cast<off_t>(headerBytes)) {
        // Grace-wait for a mid-init creator to publish the header page.
        // Backoff poll from 50 µs (review R22: the flat 2 ms sleep put a
        // 2 ms floor on every racing join). A size-less join never takes over
        // (review R6), so it waits out its whole initTimeoutMs here instead of
        // truncating the object (round-3 C23: it used to ftruncate a 0-byte
        // object to a header page and then wait ~250 ms + initTimeoutMs).
        const auto graceStart = std::chrono::steady_clock::now();
        h.waitStartMs = std::chrono::duration<double, std::milli>(graceStart.time_since_epoch()).count();
        const double graceMs = wholeObject ? opts.initTimeoutMs : 250.0;
        int delayUs = 50;
        while (fileSize < static_cast<off_t>(headerBytes)) {
          if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                        graceStart).count() > graceMs) {
            break;
          }
          ::usleep(static_cast<useconds_t>(delayUs));
          if (delayUs < 2000) delayUs *= 2;
          if (::fstat(fd, &st) != 0) {
            const int e = errno;
            ::close(fd);
        fd = -1;
            ThrowSystemError(isolate, "fstat", e, name);
          }
          fileSize = st.st_size;
        }
        if (wholeObject && fileSize < static_cast<off_t>(headerBytes)) {
          ::close(fd);
        fd = -1;
          ThrowError(isolate, "E_INIT_TIMEOUT",
                     "segment was never initialized within " +
                         std::to_string(static_cast<int64_t>(opts.initTimeoutMs)) + " ms",
                     name);
        }
        // Take over the page — GROW-ONLY (review R8): re-fstat immediately
        // before truncating so a takeover that sized the object in between is
        // never shrunk under a live mapping.
        if (::fstat(fd, &st) != 0) {
          const int e = errno;
          ::close(fd);
        fd = -1;
          ThrowSystemError(isolate, "fstat", e, name);
        }
        if (st.st_size < static_cast<off_t>(headerBytes)) {
          if (::ftruncate(fd, static_cast<off_t>(headerBytes)) != 0) {
            ThrowSystemError(isolate, "ftruncate", errno, name);
          }
          fileSize = static_cast<off_t>(headerBytes);
        } else {
          fileSize = st.st_size;
        }
        h.waitedForCreator = true;
      }
      // Never map past EOF: sized joins map min(requested, file) (InitOrJoin
      // re-maps after a takeover/grow extends the object); whole-object joins
      // map exactly the file.
      total = wholeObject ? static_cast<uint64_t>(fileSize)
                          : std::min(requestedTotal, static_cast<uint64_t>(fileSize));
    }
  }

  void* base = ::mmap(nullptr, static_cast<size_t>(total), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) {
    const int e = errno;
    ::close(fd);
        fd = -1;
    if (created) ::shm_unlink(obj.c_str());
    ThrowSystemError(isolate, "mmap", e, name);
  }

  h.base = base;
  h.mappingBytes = static_cast<size_t>(total);
  h.fd = fd;
  fd = -1;  // the handle owns it now (disarms fdGuard)
  h.created = created;
  return h;
#endif
}

void CloseSegment(SegmentHandle& h) {
#if defined(_WIN32)
  if (h.base != nullptr) UnmapViewOfFile(h.base);
  if (h.section != nullptr) CloseHandle(static_cast<HANDLE>(h.section));
#else
  if (h.base != nullptr) ::munmap(h.base, h.mappingBytes);
  if (h.fd >= 0) ::close(h.fd);
#endif
  h.base = nullptr;
  h.mappingBytes = 0;
  h.fd = -1;
#ifdef _WIN32
  h.section = nullptr;
#endif
}


#if defined(__linux__)
int ReserveBacking(int fd, uint64_t bytes) {
  int rc;
  do {
    rc = ::posix_fallocate(fd, 0, static_cast<off_t>(bytes));
  } while (rc == EINTR);  // a signal mid-reserve is not "out of space" (C21)
  return rc;
}

void ThrowReserveError(v8::Isolate* isolate, int rc, const std::string& name) {
  if (rc == ENOSPC || rc == EFBIG) {
    ThrowError(isolate, "E_NO_SPACE", "not enough space to reserve segment", name);
  }
  ThrowSystemError(isolate, "posix_fallocate", rc, name);
}
#endif

void RemapSegment(SegmentHandle& h, uint64_t newTotal) {
#if !defined(_WIN32)
  if (static_cast<uint64_t>(h.mappingBytes) >= newTotal) return;
  void* fresh = ::mmap(nullptr, static_cast<size_t>(newTotal), PROT_READ | PROT_WRITE,
                       MAP_SHARED, h.fd, 0);
  if (fresh == MAP_FAILED) return;  // caller's policy checks still bound windows
  ::munmap(h.base, h.mappingBytes);
  h.base = fresh;
  h.mappingBytes = static_cast<size_t>(newTotal);
#else
  (void)h;
  (void)newTotal;
#endif
}

void ReadHeader(v8::Isolate* isolate, const std::string& name, uint32_t maxAttach,
                HeaderInfo* out) {
#if defined(_WIN32)
  const std::wstring wname = ToWide(ObjectName(name, false));
  HANDLE section = OpenFileMappingW(FILE_MAP_READ, FALSE, wname.c_str());
  if (section == nullptr) {
    const DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND) {
      ThrowError(isolate, "E_NOT_FOUND", "segment does not exist", name);
    }
    ThrowSystemError(isolate, "OpenFileMappingW", static_cast<int>(e), name);
  }
  const void* base = MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);  // whole section
  if (base == nullptr) {
    const int e = static_cast<int>(GetLastError());
    CloseHandle(section);
    ThrowSystemError(isolate, "MapViewOfFile", e, name);
  }
  // Round-3 C8: every read is bounded by the VIEW, as the POSIX branch is
  // bounded by the bytes actually read — a small, raw or hostile section must
  // never let headerBytes steer the row loop past the mapping.
  MEMORY_BASIC_INFORMATION mbi{};
  const size_t region =
      VirtualQuery(base, &mbi, sizeof(mbi)) == sizeof(mbi) ? mbi.RegionSize : 0;
  if (region < sizeof(Header)) {
    UnmapViewOfFile(base);
    CloseHandle(section);
    ThrowError(isolate, "E_INCOMPATIBLE", "segment is smaller than a membridge header", name);
  }
  const Header* h = static_cast<const Header*>(base);
  out->magic = h->magic;
  out->layoutVersion = h->layoutVersion;
  // Expose the STATE bits only: while initializing the word packs the
  // initializer's row into bits 8+ (review R7) — ops consumers expect 0/1/2.
  out->initState = h->initState & 0xFF;
  const uint32_t hb = h->headerBytes;  // read once
  out->headerBytes = hb;
  out->flags = h->flags;
  out->dataBytes = h->dataBytes;
  uint32_t n = AttachSlotCount(hb);
  const uint64_t byView = (region - sizeof(Header)) / sizeof(AttachSlot);
  if (n > byView) n = static_cast<uint32_t>(byView);
  if (n > maxAttach) n = maxAttach;
  for (uint32_t i = 0; i < n; i++) {
    out->attach.push_back(h->attachTable[i].identity);
    out->refcounts.push_back(h->attachTable[i].refcount);
    const Liveness v1 = CheckLiveness(h->attachTable[i].identity);
    out->alive.push_back(v1 == Liveness::kAlive ? 1 : v1 == Liveness::kUnknown ? 2 : 0);
  }
  UnmapViewOfFile(base);
  CloseHandle(section);
#else
  const std::string obj = ObjectName(name, false);
  // O_NONBLOCK: a FIFO planted in /dev/shm must not block stat()/reap()
  // forever (round-4 S4-2); the S_ISREG check below rejects it.
  const int fd = ::shm_open(obj.c_str(), O_RDONLY | O_NONBLOCK, 0);
  if (fd < 0) {
    if (errno == ENOENT) {
      ThrowError(isolate, "E_NOT_FOUND", "segment does not exist", name);
    }
    if (errno == ELOOP) {
      // a symlink at the name (glibc opens with O_NOFOLLOW): not a segment —
      // E_INCOMPATIBLE lets reap() skip it instead of aborting (S4-7)
      ThrowError(isolate, "E_INCOMPATIBLE", "segment name refers to a symlink", name);
    }
    ThrowSystemError(isolate, "shm_open", errno, name);
  }
  long long objSize = -1;
  {
    struct stat st{};
    // S_ISREG is a Linux-only gate — see the matching note in OpenSegment.
#if defined(__linux__)
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
#else
    if (::fstat(fd, &st) != 0) {
#endif
      ::close(fd);
      ThrowError(isolate, "E_INCOMPATIBLE", "segment name refers to a non-regular file", name);
    }
    objSize = static_cast<long long>(st.st_size);
  }
  uint8_t buf[64 * 1024];
  ssize_t total = 0;
  while (total < static_cast<ssize_t>(sizeof(buf))) {
    const ssize_t n = ::pread(fd, buf + static_cast<size_t>(total),
                              sizeof(buf) - static_cast<size_t>(total), static_cast<off_t>(total));
    if (n <= 0) break;
    total += n;
  }
  ::close(fd);
  if (total < 32) {
    // objSize (from the fstat above, pre-close) vs bytes actually read: on
    // macOS this pair diagnosed a sized object whose reads return EOF.
    ThrowError(isolate, "E_INCOMPATIBLE",
               "segment too small for a membridge header (" + std::to_string(total) +
                   " bytes read, object size " + std::to_string(objSize) + ")",
               name);
  }
  uint32_t headerBytes;
  std::memcpy(&headerBytes, buf + 16, 4);
  std::memcpy(&out->magic, buf + 0, 4);
  std::memcpy(&out->layoutVersion, buf + 4, 4);
  std::memcpy(&out->initState, buf + 8, 4);
  out->initState &= 0xFF;  // state bits only (row packing, review R7)
  out->headerBytes = headerBytes;
  std::memcpy(&out->flags, buf + 20, 4);
  std::memcpy(&out->dataBytes, buf + 24, 8);
  // Rows are bounded by the bytes ACTUALLY read (review F29: a short file
  // must not expose uninitialized stack) and by maxAttach; empty rows are
  // skipped natively (review P9).
  const uint32_t byFile =
      AttachSlotCount(headerBytes > static_cast<uint32_t>(total) ? static_cast<uint32_t>(total) : headerBytes);
  const uint32_t n = std::min(std::min(byFile, maxAttach),
                              static_cast<uint32_t>((total - 32) / 32));
  for (uint32_t i = 0; i < n; i++) {
    AttachSlot slot;
    std::memcpy(&slot, buf + 32 + i * sizeof(AttachSlot), sizeof(AttachSlot));
    if (slot.refcount <= 0 || slot.identity.pid == 0) continue;  // empty row
    out->attach.push_back(slot.identity);
    out->refcounts.push_back(slot.refcount);
    const Liveness v2 = CheckLiveness(slot.identity);
    out->alive.push_back(v2 == Liveness::kAlive ? 1 : v2 == Liveness::kUnknown ? 2 : 0);
  }
#endif
}

}  // namespace membridge
