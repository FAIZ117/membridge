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

// Pid-only liveness for slot RECOVERY (review F23): a slot mid-publish carries
// a half-written identity — the pid word is the reserver's, but the start/ns
// words may still be the previous owner's — so the startTime comparison must
// be skipped. Only ESRCH / zombie evidence marks a pid dead; a recycled pid
// reads alive, in which case the slot leaks until process exit (accepted and
// documented, §7.2). kUnknown = never steal.
Liveness CheckPidAlive(int32_t pid);

// JS-facing wrapper (§9 stat): 'alive' | 'dead' | 'unknown'.
const char* CheckLivenessJs(int32_t pid, int64_t startTime, int64_t pidNsInode);

}  // namespace membridge

#endif  // MEMBRIDGE_LIVENESS_H_
