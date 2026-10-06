// segment.cc — shm_open/ftruncate/fallocate/mmap on POSIX;
// CreateFileMappingW/MapViewOfFile on Windows. Size comes in as a double from
// JS and is handled as int64/uint64 — never Uint32Value (F5).

#include "segment.h"

#include "header.h"

#include <cerrno>
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
    // Size the mapping against the on-disk object BEFORE any page is touched:
    // a creator that crashed before ftruncate leaves a zero-length object, and
    // reading its header would SIGBUS. POSIX only; Windows sections are sized
    // at CreateFileMappingW time.
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
      const int e = errno;
      ::close(fd);
      ThrowSystemError(isolate, "fstat", e, name);
    }
    const off_t fileSize = st.st_size;
    if (opts.raw) {
      if (!created && !wholeObject) {
        if (static_cast<uint64_t>(fileSize) < requestedTotal) {
          if (opts.sizePolicy == SizePolicy::kGrow) {
            // raw grow: no header/init lock exists — foreign-mode tradeoff
            if (::ftruncate(fd, static_cast<off_t>(requestedTotal)) != 0) {
              ThrowSystemError(isolate, "ftruncate", errno, name);
            }
          } else {
            ThrowError(isolate, "E_SIZE_MISMATCH",
                       "raw segment is smaller than requested", name,
                       static_cast<double>(requestedTotal), static_cast<double>(fileSize));
          }
        }
      }
      // total stays requestedTotal (exact/at-least/grow) or fileSize (whole)
      if (wholeObject) total = static_cast<uint64_t>(fileSize);
    } else {
      if (wholeObject) {
        total = static_cast<uint64_t>(fileSize) < headerBytes
                    ? headerBytes
                    : static_cast<uint64_t>(fileSize);
      } else if (!created && fileSize < static_cast<off_t>(headerBytes)) {
        // Creator died before even writing the header page: size the object
        // for the takeover path (it rewrites the header; InitOrJoin's
        // EnsureSizedOnTakeover fixes the data region when needed).
        if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
          ThrowSystemError(isolate, "ftruncate", errno, name);
        }
      }
    }
  }

  // Size the object on create. On join, only `grow` extends it (caller runs
  // that under the init lock). Windows needs none of this — the section is
  // fixed at CreateFileMappingW time, which is why `grow` is POSIX-only and
  // the size lives in the header (§5.2/§5.3).
  if (created) {
    if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
      const int e = errno;
      ::close(fd);
      ::shm_unlink(obj.c_str());
      ThrowSystemError(isolate, "ftruncate", e, name);
    }
    // Reserve (Linux): tmpfs charges lazily, so a full /dev/shm would
    // otherwise surface as SIGBUS hours later. Reserving turns it into a
    // create-time error (F7).
#if defined(__linux__)
    if (opts.reserve) {
      const int rc = ::posix_fallocate(fd, 0, static_cast<off_t>(total));
      if (rc != 0) {
        ::close(fd);
        ::shm_unlink(obj.c_str());
        ThrowError(isolate, "E_NO_SPACE", "not enough space to reserve segment", name);
      }
    }
#endif
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

}  // namespace membridge
