// liveness.h — §7.1 participant identity and liveness checking.

#ifndef MEMBRIDGE_LIVENESS_H_
#define MEMBRIDGE_LIVENESS_H_

#include "membridge.h"

namespace membridge {

Identity SelfIdentity();  // threadId = OS thread id (gettid) for lock slots

enum class Liveness { kAlive, kDead, kUnknown };

// Process-level liveness of an identity row. kUnknown means liveness is
// unknowable (foreign pid namespace) — callers must treat it as alive and
// never steal (§7.1). `threadId` is ignored here; dead *threads* inside a
// live process are detected by the env-cleanup hook (M4).
Liveness CheckLiveness(const Identity& id);

// JS-facing wrapper (§9 stat): 'alive' | 'dead' | 'unknown'.
const char* CheckLivenessJs(int32_t pid, int64_t startTime, int64_t pidNsInode);

}  // namespace membridge

#endif  // MEMBRIDGE_LIVENESS_H_
