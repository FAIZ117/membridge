// registry.h — process-wide name -> weak_ptr<Mapping> (§5.4).

#ifndef MEMBRIDGE_REGISTRY_H_
#define MEMBRIDGE_REGISTRY_H_

#include "membridge.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace membridge {

class Registry {
 public:
  static Registry& Get();

  // Live mapping for `name`, or nullptr.
  std::shared_ptr<Mapping> Find(const std::string& name);

  // Insert or replace the entry (grow-replaces-entry semantics, §5.4). SABs
  // over a replaced mapping keep it alive through their own references.
  void Put(const std::string& name, const std::shared_ptr<Mapping>& m);

  // Drop the entry (unlink): the next open must not reuse the old mapping
  // even if it is still alive through live SABs.
  void Erase(const std::string& name);

  // Live mapping whose range contains `addr`, or nullptr (§6 platform paths
  // derive per-word kernel-object names from the segment name).
  static std::shared_ptr<Mapping> FindByAddress(const void* addr);

  // Another live, not-yet-detached mapping in this process over the same
  // segment object, or nullptr (review F20 / round-3 C15). Searches EVERY
  // mapping ever Put — not only the current per-name entry, which a grow
  // replaces — so the attach row is handed to a survivor instead of being
  // released early or leaked. Compares recorded object identity (dev/ino;
  // the name on Windows) — NOT base pointers, which differ per mmap.
  // Every strong reference taken while scanning is moved into `keep`: the
  // caller must destroy them only AFTER releasing its locks (dropping the
  // last reference runs ~Mapping -> Detach, which takes the detach lock).
  std::shared_ptr<Mapping> HeirFor(const Mapping* self,
                                   std::vector<std::shared_ptr<Mapping>>* keep);

  // All live mappings (debug/ops).
  std::vector<std::shared_ptr<Mapping>> Live();

 private:
  std::mutex mu_;
  std::map<std::string, std::weak_ptr<Mapping>> map_;
  std::vector<std::weak_ptr<Mapping>> all_;  // every mapping Put, pruned lazily
};

// POSIX object-identity helpers (review F16/F21). NameRefersTo compares a
// fresh shm_open+fstat against the Mapping's recorded dev/ino (always true
// on Windows); RecordIdentity captures them from the Mapping's fd.
bool NameRefersTo(const std::string& name, const Mapping& m);
void RecordIdentity(Mapping& m);

}  // namespace membridge

#endif  // MEMBRIDGE_REGISTRY_H_
