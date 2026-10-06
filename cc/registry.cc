// registry.cc — Mapping lifetime and the process-wide registry (§5.4).
//
// The mapping lives while any SAB in any isolate holds it: each BackingStore
// carries a heap shared_ptr<Mapping> in its deleter data; the deleter (which
// may run on any GC thread) only drops that reference. ~Mapping then unmaps
// `base` and releases the attach-table row — lock-protected / atomic because
// it can run off the main thread.

#include "registry.h"

#include "header.h"
#include "liveness.h"
#include "segment.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
std::mutex g_detach_mu;  // serializes Detach header updates
}  // namespace

Mapping::Mapping() = default;

Mapping::~Mapping() {
  Detach();
  if (base != nullptr) {
#if defined(_WIN32)
    UnmapViewOfFile(base);
    if (section != nullptr) CloseHandle(static_cast<HANDLE>(section));
#else
    munmap(base, mappingBytes);
    if (fd >= 0) ::close(fd);
#endif
    base = nullptr;
  }
}

void Mapping::Detach() {
  std::lock_guard<std::mutex> lock(g_detach_mu);
  if (raw || attachSlot < 0 || detached.exchange(true)) {
    return;
  }
  // Review F20: a second Mapping over the same segment (e.g. after grow)
  // shares the attach row — only the last one in THIS process may release
  // it and decide unlinkWhenUnused.
  if (Registry::Get().OthersShareBase(base, this)) {
    attachSlot = -1;
    return;
  }
  auto* h = static_cast<Header*>(base);
  if (h != nullptr) {
    ReleaseAttachRow(h, attachSlot);  // slot index was validated at claim
    // §9 unlinkWhenUnused: the detach that empties the attach table unlinks
    // the name, so late joiners cannot resurrect an abandoned segment.
    if (unlinkWhenUnused) {
      bool anyAttached = false;
      // Frozen row count (review R4): never re-derive from the shared
      // headerBytes field here — Detach can run on any GC thread.
      const uint32_t n = attachSlotCount != 0
                             ? attachSlotCount
                             : RowCountBounded(h->headerBytes, mappingBytes);
      for (uint32_t i = 0; i < n; i++) {
        if (AtomicSlotRefcount(&h->attachTable[i])->load(std::memory_order_acquire) > 0) {
          anyAttached = true;
          break;
        }
      }
      if (!anyAttached && NameRefersTo(name, *this)) {
        // Review F21: only unlink when the name still refers to OUR object —
        // another process may have unlinked and recreated it.
#if defined(_WIN32)
        h->flags |= kFlagUnlinked;  // the section dies with the last handle
#else
        ::shm_unlink(name.c_str());  // ENOENT (already gone) is fine
#endif
      }
    }
  }
  attachSlot = -1;
}

// POSIX: does `name` still refer to the object `m` mapped? Compares dev/ino
// via a fresh shm_open — closes the unlink+recreate split brain (review
// F16). Always true on Windows (sections have no inode).
bool NameRefersTo(const std::string& name, const Mapping& m) {
#if defined(_WIN32)
  (void)name;
  (void)m;
  return true;
#else
  if (m.ino == 0) return true;  // identity never recorded: cannot judge
  const int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
  if (fd < 0) return false;  // name gone: nobody else can join through it
  struct stat st{};
  const bool same = ::fstat(fd, &st) == 0 &&
                    static_cast<int64_t>(st.st_dev) == m.dev &&
                    static_cast<int64_t>(st.st_ino) == m.ino;
  ::close(fd);
  return same;
#endif
}

// POSIX: record the object identity of m->fd into m.
void RecordIdentity(Mapping& m) {
#if !defined(_WIN32)
  if (m.fd < 0) return;
  struct stat st{};
  if (::fstat(m.fd, &st) == 0) {
    m.dev = static_cast<int64_t>(st.st_dev);
    m.ino = static_cast<int64_t>(st.st_ino);
  }
#endif
}

Registry& Registry::Get() {
  static Registry r;
  return r;
}

std::shared_ptr<Mapping> Registry::Find(const std::string& name) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = map_.find(name);
  if (it == map_.end()) return nullptr;
  return it->second.lock();
}

void Registry::Put(const std::string& name, const std::shared_ptr<Mapping>& m) {
  std::lock_guard<std::mutex> lock(mu_);
  map_[name] = m;
}

void Registry::Erase(const std::string& name) {
  std::lock_guard<std::mutex> lock(mu_);
  map_.erase(name);
}

bool Registry::OthersShareBase(const void* base, const Mapping* self) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto it = map_.begin(); it != map_.end(); ++it) {
    if (auto m = it->second.lock()) {
      if (m.get() != self && m->base == base) return true;
    }
  }
  return false;
}

std::shared_ptr<Mapping> Registry::FindByAddress(const void* addr) {
  for (const std::shared_ptr<Mapping>& m : Get().Live()) {
    const char* base = static_cast<const char*>(m->base);
    if (addr >= base && addr < base + m->mappingBytes) return m;
  }
  return nullptr;
}

std::vector<std::shared_ptr<Mapping>> Registry::Live() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::shared_ptr<Mapping>> out;
  for (auto it = map_.begin(); it != map_.end();) {
    if (auto m = it->second.lock()) {
      out.push_back(std::move(m));
      ++it;
    } else {
      it = map_.erase(it);
    }
  }
  return out;
}

}  // namespace membridge
