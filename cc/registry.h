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

  // All live mappings (debug/ops).
  std::vector<std::shared_ptr<Mapping>> Live();

 private:
  std::mutex mu_;
  std::map<std::string, std::weak_ptr<Mapping>> map_;
};

}  // namespace membridge

#endif  // MEMBRIDGE_REGISTRY_H_
