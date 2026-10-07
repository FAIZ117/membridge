// registry.cc — Mapping lifetime and the process-wide registry (§5.4).
//
// The mapping lives while any SAB in any isolate holds it: each BackingStore
// carries a heap shared_ptr<Mapping> in its deleter data; the deleter (which
// may run on any GC thread) only drops that reference. ~Mapping then unmaps
// `base` and releases the attach-table row — lock-protected / atomic because
// it can run off the main thread.

#include "registry.h"

#include <cerrno>

#include "header.h"
#include "liveness.h"
#include "segment.h"

#include <algorithm>
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
  Registry::Get().Forget(this);
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
  // Declared BEFORE the lock: strong references collected while looking for
  // an heir are released only after g_detach_mu is (round-3 C15).
  std::vector<std::shared_ptr<Mapping>> keep;
  std::lock_guard<std::mutex> lock(g_detach_mu);
  if (raw || detached.exchange(true)) {
    return;
  }
  if (attachSlot < 0) {
    // Not the row owner: nothing to release, but an unlinkWhenUnused request
    // made through THIS mapping must survive it (round-4 E4-8) — hand it to
    // a surviving mapping of the same object, which owns or will inherit the
    // row and makes the decision.
    if (unlinkWhenUnused) {
      if (Mapping* heir = Registry::Get().HeirFor(this, &keep).get()) {
        heir->unlinkWhenUnused = true;
      }
    }
    return;
  }
  // Review F20 / round-3 C15: other Mappings over the same segment (e.g.
  // after a grow) share this process's single attach row. Hand the row —
  // and the unlinkWhenUnused decision — to a survivor; only the LAST mapping
  // of the segment in this process releases it. (The old code dropped the
  // row without handing it on, so when the owner detached first the row
  // leaked and unlinkWhenUnused never fired.)
  if (Mapping* heir = Registry::Get().HeirFor(this, &keep).get()) {
    if (heir->attachSlot < 0) {
      heir->attachSlot = attachSlot;
      heir->attachSlotCount = attachSlotCount;
    }
    heir->unlinkWhenUnused = heir->unlinkWhenUnused || unlinkWhenUnused;
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
#if defined(__linux__)
  // Round-3 C22: glibc resolves shm names under /dev/shm, so ONE lstat gives
  // the same answer as shm_open + fstat + close (three syscalls on every
  // registry reuse). lstat, not stat: a symlink planted at the name reports
  // its own inode and never matches (shm_open would refuse it — O_NOFOLLOW).
  // Unexpected errors fall through to the portable path below.
  {
    const std::string path = "/dev/shm" + name;  // name starts with '/'
    struct stat lst{};
    if (::lstat(path.c_str(), &lst) == 0) {
      return static_cast<int64_t>(lst.st_dev) == m.dev &&
             static_cast<int64_t>(lst.st_ino) == m.ino;
    }
    if (errno == ENOENT) return false;  // name gone: nobody can join through it
  }
#endif
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

namespace {
// Raw mappings live under their own key (round-4 E4-10). '\x01' cannot occur
// in a validated segment name.
std::string RegKey(const std::string& name, bool raw) {
  return raw ? std::string("\x01raw:") + name : name;
}
}  // namespace

std::shared_ptr<Mapping> Registry::Find(const std::string& name, bool raw) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = map_.find(RegKey(name, raw));
  if (it == map_.end()) return nullptr;
  return it->second.lock();
}

Registry::ObjectKey Registry::KeyOf(const Mapping& m) {
  ObjectKey k;
#if !defined(_WIN32)
  if (m.ino != 0) {
    k.dev = m.dev;
    k.ino = m.ino;
    return k;
  }
#endif
  k.name = m.name;  // Windows sections have no inode: the name decides
  return k;
}

void Registry::PruneLocked(const ObjectKey& key) {
  auto b = byObject_.find(key);
  if (b == byObject_.end()) return;
  auto& v = b->second;
  // expired() takes no strong reference: nothing can be destroyed under mu_.
  v.erase(std::remove_if(v.begin(), v.end(),
                         [](const std::weak_ptr<Mapping>& w) { return w.expired(); }),
          v.end());
  if (v.empty()) byObject_.erase(b);
}

void Registry::Put(const std::string& name, const std::shared_ptr<Mapping>& m) {
  std::lock_guard<std::mutex> lock(mu_);
  map_[RegKey(name, m->raw)] = m;
  // Only this object's bucket is touched (round-4 P4-3: pruning the whole
  // list on every insert made each cold open O(mappings)).
  const ObjectKey key = KeyOf(*m);
  PruneLocked(key);
  byObject_[key].push_back(m);
}

void Registry::Erase(const std::string& name) {
  std::lock_guard<std::mutex> lock(mu_);
  map_.erase(RegKey(name, false));
  map_.erase(RegKey(name, true));
}

void Registry::Forget(const Mapping* self) {
  std::lock_guard<std::mutex> lock(mu_);
  PruneLocked(KeyOf(*self));
}

void Registry::MarkUnlinkWhenUnused(Mapping& m) {
  std::lock_guard<std::mutex> lock(g_detach_mu);  // Detach reads it under this lock
  m.unlinkWhenUnused = true;
}


std::shared_ptr<Mapping> Registry::HeirFor(const Mapping* self,
                                           std::vector<std::shared_ptr<Mapping>>* keep) {
  std::lock_guard<std::mutex> lock(mu_);
  std::shared_ptr<Mapping> heir;
  auto b = byObject_.find(KeyOf(*self));
  if (b == byObject_.end()) return heir;
  auto& v = b->second;
  for (auto it = v.begin(); it != v.end();) {
    std::shared_ptr<Mapping> m = it->lock();
    if (m == nullptr) {
      it = v.erase(it);
      continue;
    }
    if (heir == nullptr && m.get() != self && !m->raw && !m->detached.load()) heir = m;
    keep->push_back(std::move(m));  // never destroyed under our locks
    ++it;
  }
  if (v.empty()) byObject_.erase(b);
  return heir;
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
