// registry.h — process-wide name -> weak_ptr<Mapping> (§5.4).

#ifndef SHM_BRIDGE_REGISTRY_H_
#define SHM_BRIDGE_REGISTRY_H_

#include "shm_bridge.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace shm_bridge {

class Registry {
 public:
  static Registry& Get();

  // Live mapping for `name`, or nullptr. Raw and header (typed) mappings of a
  // name live under separate keys (round-4 E4-10: a raw open used to replace
  // the typed entry, so the next typed open built a second mapping — and a
  // second RingProducer instance on the same thread).
  std::shared_ptr<Mapping> Find(const std::string& name, bool raw = false);

  // Insert or replace the entry for (name, m->raw) (grow-replaces-entry
  // semantics, §5.4). SABs over a replaced mapping keep it alive through
  // their own references.
  void Put(const std::string& name, const std::shared_ptr<Mapping>& m);

  // Drop both entries for `name` (unlink): the next open must not reuse the
  // old mapping even if it is still alive through live SABs.
  void Erase(const std::string& name);

  // Prune `self`'s (expired) entry from the per-object index; called from
  // ~Mapping so the index never accumulates dead entries (round-4 P4-2/3).
  void Forget(const Mapping* self);

  // Mark a mapping reused with { unlinkWhenUnused: true } (round-4 E4-8).
  static void MarkUnlinkWhenUnused(Mapping& m);

  // Live mapping whose range contains `addr`, or nullptr (§6 platform paths
  // derive per-word kernel-object names from the segment name).
  static std::shared_ptr<Mapping> FindByAddress(const void* addr);

  // Another live, not-yet-detached mapping in this process over the same
  // segment object, or nullptr (review F20 / round-3 C15). Searches every
  // mapping ever Put for that OBJECT — not only the current per-name entry,
  // which a grow replaces — via a per-object index (round-4 P4-2: scanning
  // every mapping on every detach made teardown O(n^2)). Compares recorded object identity (dev/ino;
  // the name on Windows) — NOT base pointers, which differ per mmap.
  // Every strong reference taken while scanning is moved into `keep`: the
  // caller must destroy them only AFTER releasing its locks (dropping the
  // last reference runs ~Mapping -> Detach, which takes the detach lock).
  std::shared_ptr<Mapping> HeirFor(const Mapping* self,
                                   std::vector<std::shared_ptr<Mapping>>* keep);

  // All live mappings (debug/ops).
  std::vector<std::shared_ptr<Mapping>> Live();

 private:
  struct ObjectKey {
    int64_t dev = 0;
    int64_t ino = 0;
    std::string name;  // used when no inode was recorded (Windows, fstat failure)
    bool operator<(const ObjectKey& o) const {
      if (dev != o.dev) return dev < o.dev;
      if (ino != o.ino) return ino < o.ino;
      return name < o.name;
    }
  };
  static ObjectKey KeyOf(const Mapping& m);
  // Caller holds mu_. Erases expired weak entries; drops the bucket if empty.
  void PruneLocked(const ObjectKey& key);

  std::mutex mu_;
  std::map<std::string, std::weak_ptr<Mapping>> map_;
  // Every Mapping ever Put, bucketed by segment object (pruned per bucket).
  std::map<ObjectKey, std::vector<std::weak_ptr<Mapping>>> byObject_;
};

// POSIX object-identity helpers (review F16/F21). NameRefersTo compares a
// fresh shm_open+fstat against the Mapping's recorded dev/ino (always true
// on Windows); RecordIdentity captures them from the Mapping's fd.
bool NameRefersTo(const std::string& name, const Mapping& m);
void RecordIdentity(Mapping& m);

}  // namespace shm_bridge

#endif  // SHM_BRIDGE_REGISTRY_H_
