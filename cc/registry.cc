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
#include <unistd.h>
#endif

namespace membridge {

namespace {
std::mutex g_detach_mu;  // serializes Detach/ProcessDetach header updates
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
  auto* h = static_cast<Header*>(base);
  if (h != nullptr) {
    ReleaseAttachRow(h, attachSlot);
    // §9 unlinkWhenUnused: the detach that empties the attach table unlinks
    // the name, so late joiners cannot resurrect an abandoned segment.
    // Best-effort: a racing creator/joiner is protected by the header magic
    // check on its side.
    if (unlinkWhenUnused) {
      bool anyAttached = false;
      const uint32_t n = AttachSlotCount(h->headerBytes);
      for (uint32_t i = 0; i < n; i++) {
        if (AtomicSlotRefcount(&h->attachTable[i])->load(std::memory_order_acquire) > 0) {
          anyAttached = true;
          break;
        }
      }
      if (!anyAttached) {
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
