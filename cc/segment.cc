// segment.cc — shm_open/ftruncate/fallocate/mmap on POSIX;
// CreateFileMappingW/MapViewOfFile on Windows. Size comes in as a double from
// JS and is handled as int64/uint64 — never Uint32Value (F5).

#include "segment.h"

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
  void* base = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0,
                             wholeObject ? 0 : static_cast<SIZE_T>(requestedTotal));
  if (base == nullptr) {
    CloseHandle(section);
    ThrowSystemError(isolate, "MapViewOfFile", static_cast<int>(GetLastError()), name);
  }
  size_t mapped = static_cast<size_t>(requestedTotal);
  if (wholeObject) {
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
      ThrowSystemError(isolate, "fstat", e, name);
    }
    off_t fileSize = st.st_size;

    if (opts.raw) {
      if (created) {
        // Raw creator: no header, no init protocol — size the whole object
        // now (full sizing via InitOrJoin needs the header protocol).
        if (::ftruncate(fd, static_cast<off_t>(requestedTotal)) != 0) {
          const int e = errno;
          ::close(fd);
          ::shm_unlink(obj.c_str());
          ThrowSystemError(isolate, "ftruncate", e, name);
        }
#if defined(__linux__)
        if (opts.reserve) {
          const int rc = ::posix_fallocate(fd, 0, static_cast<off_t>(requestedTotal));
          if (rc != 0) {
            ::close(fd);
            ::shm_unlink(obj.c_str());
            ThrowError(isolate, "E_NO_SPACE", "not enough space to reserve segment", name);
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
      // Creator: back the header page IMMEDIATELY so joiners can never see a
      // sub-header-page object for long, and never map past EOF ourselves.
      // Full sizing (+ reserve) happens in InitOrJoin once state = 1 makes us
      // the sole, visible initializer.
      if (::ftruncate(fd, static_cast<off_t>(headerBytes)) != 0) {
        const int e = errno;
        ::close(fd);
        ::shm_unlink(obj.c_str());
        ThrowSystemError(isolate, "ftruncate", e, name);
      }
      total = headerBytes;
    } else {
      // Joiner: wait briefly for a mid-init creator to publish the header
      // page; a creator that died before even that is taken over after the
      // grace period (everyone truncates to exactly headerBytes — idempotent,
      // no shrink war) and the §5.2 CAS serializes real initialization.
      if (fileSize < static_cast<off_t>(headerBytes)) {
        const auto graceStart = std::chrono::steady_clock::now();
        while (fileSize < static_cast<off_t>(headerBytes)) {
          if (std::chrono::steady_clock::now() - graceStart > std::chrono::milliseconds(250)) {
            break;
          }
          ::usleep(2000);
          if (::fstat(fd, &st) != 0) {
            const int e = errno;
            ::close(fd);
            ThrowSystemError(isolate, "fstat", e, name);
          }
          fileSize = st.st_size;
        }
        if (fileSize < static_cast<off_t>(headerBytes)) {
          if (::ftruncate(fd, static_cast<off_t>(headerBytes)) != 0) {
            ThrowSystemError(isolate, "ftruncate", errno, name);
          }
          fileSize = static_cast<off_t>(headerBytes);
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
    if (created) ::shm_unlink(obj.c_str());
    ThrowSystemError(isolate, "mmap", e, name);
  }

  h.base = base;
  h.mappingBytes = static_cast<size_t>(total);
  h.fd = fd;
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
  const Header* h = static_cast<const Header*>(base);
  out->magic = h->magic;
  out->layoutVersion = h->layoutVersion;
  out->initState = h->initState;
  out->headerBytes = h->headerBytes;
  out->flags = h->flags;
  out->dataBytes = h->dataBytes;
  const uint32_t count = AttachSlotCount(h->headerBytes);
  const uint32_t n = count < maxAttach ? count : maxAttach;
  for (uint32_t i = 0; i < n; i++) {
    out->attach.push_back(h->attachTable[i].identity);
    out->refcounts.push_back(h->attachTable[i].refcount);
  }
  UnmapViewOfFile(base);
  CloseHandle(section);
#else
  const std::string obj = ObjectName(name, false);
  const int fd = ::shm_open(obj.c_str(), O_RDONLY, 0);
  if (fd < 0) {
    if (errno == ENOENT) {
      ThrowError(isolate, "E_NOT_FOUND", "segment does not exist", name);
    }
    ThrowSystemError(isolate, "shm_open", errno, name);
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
    ThrowError(isolate, "E_INCOMPATIBLE", "segment too small for a membridge header", name);
  }
  uint32_t headerBytes;
  std::memcpy(&headerBytes, buf + 16, 4);
  std::memcpy(&out->magic, buf + 0, 4);
  std::memcpy(&out->layoutVersion, buf + 4, 4);
  std::memcpy(&out->initState, buf + 8, 4);
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
  }
#endif
}

}  // namespace membridge
