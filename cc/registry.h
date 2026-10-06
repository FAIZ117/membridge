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

  // True when some OTHER live mapping in this process refers to the same
  // segment object (review F20): the attach row and any unlinkWhenUnused
  // decision must not be made unilaterally. Compares recorded object
  // identity (dev/ino; same name on Windows) — NOT base pointers, which
  // differ per mmap even for the same segment.
  bool OthersShareSegment(const Mapping* self);

  // All live mappings (debug/ops).
  std::vector<std::shared_ptr<Mapping>> Live();

 private:
  std::mutex mu_;
  std::map<std::string, std::weak_ptr<Mapping>> map_;
};

// POSIX object-identity helpers (review F16/F21). NameRefersTo compares a
// fresh shm_open+fstat against the Mapping's recorded dev/ino (always true
// on Windows); RecordIdentity captures them from the Mapping's fd.
bool NameRefersTo(const std::string& name, const Mapping& m);
void RecordIdentity(Mapping& m);

}  // namespace membridge

#endif  // MEMBRIDGE_REGISTRY_H_
