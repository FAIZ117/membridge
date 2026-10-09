// liveness.cc — §7.1: who is still alive?
// Identity = pid + startTime + pid-namespace inode + threadId. PIDs get
// reused, containers that share /dev/shm see different PIDs for the same
// process, and all workers share one PID (threadId separates them).

#include "liveness.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>

#if defined(__linux__)
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace shm_bridge {

#if defined(__linux__)

namespace {

// One /proc/<pid>/stat read returning field 22 (starttime, clock ticks after
// boot) and field 3 (state: 'R','S','D','Z','X',…). The comm field may
// contain spaces and parens, so parsing starts after the last ')'.
bool ReadProcStat(int32_t pid, int64_t* outStart, char* outState) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  FILE* f = std::fopen(path, "r");
  if (f == nullptr) return false;
  char buf[4096];
  size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  buf[n] = '\0';
  const char* p = std::strrchr(buf, ')');
  if (p == nullptr) return false;
  // After ')' the fields continue at index 3 (state). starttime is field 22,
  // i.e. the 20th token after ')'.
  int field = 3;
  char token[128];
  for (const char* q = p + 1; *q != '\0';) {
    while (*q == ' ') q++;
    if (*q == '\0') break;
    size_t i = 0;
    while (*q != '\0' && *q != ' ' && i < sizeof(token) - 1) token[i++] = *q++;
    token[i] = '\0';
    if (field == 3 && outState != nullptr) *outState = token[0];
    if (field == 22) {
      if (outStart != nullptr) *outStart = std::strtoll(token, nullptr, 10);
      return true;
    }
    field++;
  }
  return false;
}

// /proc/self/ns/pid inode — distinguishes pid namespaces (containers sharing
// /dev/shm via --ipc see different PIDs for the same process, F9).
int64_t ReadPidNsInode(int32_t pid) {
  char path[64];
  if (pid == 0) {
    std::snprintf(path, sizeof(path), "/proc/self/ns/pid");
  } else {
    std::snprintf(path, sizeof(path), "/proc/%d/ns/pid", pid);
  }
  char buf[256];
  ssize_t n = readlink(path, buf, sizeof(buf) - 1);
  if (n <= 0) return -1;
  buf[n] = '\0';
  const char* lb = std::strchr(buf, '[');
  if (lb == nullptr) return -1;
  return std::strtoll(lb + 1, nullptr, 10);
}

}  // namespace

Identity SelfIdentity() {
  // Per-thread cache (review P7): startTime and the pid-ns inode are process
  // constants; re-read only when the pid changes. A FAILED read is not cached
  // (review R19): a transient EMFILE at a thread's first read must not pin
  // startTime=-1 (or an unknown ns) on the thread forever — every later claim
  // would stamp -1, and a -1 identity is judged Unknown = never steal.
  static thread_local struct {
    int32_t pid = -1;
    int32_t tid = -1;
    bool startValid = false;
    int64_t start = -1;
    int64_t ns = -1;
  } cache;
  const int32_t pid = static_cast<int32_t>(getpid());
  const int32_t tid = static_cast<int32_t>(syscall(SYS_gettid));
  if (cache.pid != pid || cache.tid != tid) {
    cache.pid = pid;
    cache.tid = tid;
    cache.startValid = false;
    cache.start = -1;
    cache.ns = -1;
  }
  // Round-3 C9: retry whatever is still unknown on EVERY call — recording
  // pid/tid above used to mark the entry fresh even when the reads failed,
  // so the "not cached" retry never happened and a transient EMFILE pinned
  // startTime=-1 on the thread for good.
  if (!cache.startValid) {
    int64_t start = -1;
    if (ReadProcStat(pid, &start, nullptr)) {
      cache.start = start;
      cache.startValid = true;
    }
  }
  if (cache.ns <= 0) {
    const int64_t ns = ReadPidNsInode(0);
    if (ns > 0) cache.ns = ns;  // -1 (unreadable) stays uncached
  }
  Identity id;
  id.pid = cache.pid;
  id.threadId = cache.tid;
  id.startTime = cache.startValid ? cache.start : -1;
  id.pidNsInode = cache.ns;
  return id;
}

Liveness CheckLiveness(const Identity& id) {
  if (id.pid <= 0) return Liveness::kDead;
  if (id.startTime < 0) return Liveness::kUnknown;  // identity never recorded: never steal (F25)
  // Own pid-ns inode, read once per process (review R18: readlink'ing
  // /proc/self/ns/pid on every contended mutex pass dominated the steal
  // path; the namespace of a running process cannot change under it).
  // A failed read is NOT cached (R19 rule): the cache only settles on a
  // positive inode, so an unreadable /proc retries on later calls instead
  // of silently dropping the foreign-namespace never-steal guard forever.
  static std::atomic<int64_t> ownNsCache{0};  // 0 = unsettled
  int64_t ownNs = ownNsCache.load(std::memory_order_relaxed);
  if (ownNs <= 0) {
    ownNs = ReadPidNsInode(0);
    if (ownNs > 0) ownNsCache.store(ownNs, std::memory_order_relaxed);
  }
  if (id.pidNsInode > 0 && ownNs > 0 && id.pidNsInode != ownNs) {
    return Liveness::kUnknown;  // foreign pid namespace: never steal (§7.1)
  }
  // kill(pid, 0): ESRCH = definitely dead; success or EPERM = the pid exists
  // (EPERM = alive, per §7.1).
  if (kill(static_cast<pid_t>(id.pid), 0) != 0 && errno == ESRCH) {
    return Liveness::kDead;
  }
  // The pid exists — judge WHAT it is from /proc (one read):
  //   unreadable (hidepid)         -> Unknown: liveness unverifiable, never steal (review F7)
  //   state 'Z' (zombie) or 'X'    -> Dead: the process exited; only its
  //                                   unreaped corpse remains (review F4 — a
  //                                   zombie answers kill(pid,0) as alive and
  //                                   keeps a holder unstealable forever)
  //   starttime differs            -> Dead: the pid was recycled
  int64_t start = -1;
  char state = '?';
  if (!ReadProcStat(id.pid, &start, &state)) return Liveness::kUnknown;
  if (state == 'Z' || state == 'X') return Liveness::kDead;
  if (start != id.startTime) return Liveness::kDead;  // pid reused
  return Liveness::kAlive;
}


Liveness CheckPidAlive(int32_t pid) {
  // Pid-only judgment (review F23): no startTime comparison — a slot
  // mid-publish carries the previous owner's start words.
  if (pid <= 0) return Liveness::kUnknown;
  if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) return Liveness::kDead;
  int64_t start = -1;
  char state = '?';
  if (!ReadProcStat(pid, &start, &state)) return Liveness::kUnknown;
  if (state == 'Z' || state == 'X') return Liveness::kDead;
  return Liveness::kAlive;
}

#elif defined(__APPLE__)

namespace {

#ifndef SZOMB
#define SZOMB 5  // <sys/proc.h>: process exited, not yet reaped
#endif

// One proc_pidinfo read: start time (µs — second granularity would let a pid
// recycled within the same second look like the original) and zombie state.
// proc_pidinfo returns however many bytes the kernel filled, which can be
// SHORTER than the SDK's struct on an older runtime. A short fill zeroed the
// start fields, and a zero start must never be compared against a real row
// start (it would read "pid reused" for a LIVE holder) — report start = -1,
// which CheckLiveness maps to kUnknown = never steal (safe, §7.1).
bool ReadBsdInfo(int32_t pid, int64_t* outStartUs, bool* outZombie) {
  proc_bsdinfo info{};
  const int r = static_cast<int>(proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)));
  if (r == 0) {
    // The kernel returned NO info for a pid that kill() says exists: the
    // process has exited (an unreaped zombie corpse carries no BSD info).
    // That is exactly the §7.1 dead verdict the steal path needs.
    *outStartUs = 0;
    *outZombie = true;
    return true;
  }
  if (r < 0) return false;  // call error (EPERM/…): unknown, never steal
  if (r < static_cast<int>(sizeof(info))) {
    // Short fill (older runtime): pbi_status (@4) is still valid; the deep
    // start fields may not be. A short-filled zombie is dead; a live short
    // read reports an unknown start (never a fake mismatch) so CheckLiveness
    // maps it to kUnknown = never steal.
    if (info.pbi_status == SZOMB) {
      *outZombie = true;
      *outStartUs = 0;
      return true;
    }
    *outZombie = false;
    if (info.pbi_start_tvsec == 0 && info.pbi_start_tvusec == 0) {
      *outStartUs = -1;
      return true;
    }
  }
  *outStartUs = static_cast<int64_t>(info.pbi_start_tvsec) * 1000000 +
                static_cast<int64_t>(info.pbi_start_tvusec);
  *outZombie = info.pbi_status == SZOMB;
  return true;
}
}  // namespace

Identity SelfIdentity() {
  Identity id;
  id.pid = static_cast<int32_t>(getpid());
  id.threadId = static_cast<int32_t>(syscall(SYS_thread_selfid));
  int64_t start = -1;
  bool zombie = false;
  if (!ReadBsdInfo(id.pid, &start, &zombie)) start = -1;  // claims refuse -1 (§7.1)
  id.startTime = start;
  id.pidNsInode = -1;  // no pid namespaces on macOS
  return id;
}

// Same rules as Linux (round-3 F4/F7 parity): ESRCH = dead; an unreadable
// process = unknown (never steal — the start-time guard cannot run); a
// zombie = dead (it answers kill(pid,0) until reaped); start mismatch = dead.
Liveness CheckLiveness(const Identity& id) {
  if (id.pid <= 0) return Liveness::kDead;
  if (id.pid == static_cast<int32_t>(getpid())) {
    // Self: alive iff the row's start matches ours (a stale row carrying our
    // pid with a foreign start is a recycled-pid simulation — dead, F62).
    const int64_t selfStart = SelfIdentity().startTime;
    if (id.startTime < 0 || selfStart < 0) return Liveness::kUnknown;
    return id.startTime == selfStart ? Liveness::kAlive : Liveness::kDead;
  }
  if (id.startTime < 0) return Liveness::kUnknown;
  if (kill(static_cast<pid_t>(id.pid), 0) != 0 && errno == ESRCH) return Liveness::kDead;
  int64_t start = -1;
  bool zombie = false;
  if (!ReadBsdInfo(id.pid, &start, &zombie)) return Liveness::kUnknown;
  if (zombie) return Liveness::kDead;
  if (start != id.startTime) return Liveness::kDead;  // pid reused
  return Liveness::kAlive;
}


Liveness CheckPidAlive(int32_t pid) {
  if (pid <= 0) return Liveness::kUnknown;
  if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) return Liveness::kDead;
  int64_t start = -1;
  bool zombie = false;
  if (!ReadBsdInfo(pid, &start, &zombie)) return Liveness::kUnknown;
  return zombie ? Liveness::kDead : Liveness::kAlive;
}

#elif defined(_WIN32)

Identity SelfIdentity() {
  Identity id;
  id.pid = static_cast<int32_t>(GetCurrentProcessId());
  id.threadId = static_cast<int32_t>(GetCurrentThreadId());
  FILETIME created{}, exited{}, kernel{}, user{};
  int64_t start = -1;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id.pid);
  if (h != nullptr) {
    if (GetProcessTimes(h, &created, &exited, &kernel, &user)) {
      start = (static_cast<int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }
    CloseHandle(h);
  }
  id.startTime = start;
  id.pidNsInode = -1;  // no pid namespaces on Windows
  return id;
}

Liveness CheckLiveness(const Identity& id) {
  if (id.pid <= 0) return Liveness::kDead;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(id.pid));
  if (h == nullptr) {
    return GetLastError() == ERROR_ACCESS_DENIED ? Liveness::kUnknown : Liveness::kDead;
  }
  DWORD exitCode = 0;
  BOOL ok = GetExitCodeProcess(h, &exitCode);
  FILETIME created{}, exited{}, kernel{}, user{};
  BOOL times = GetProcessTimes(h, &created, &exited, &kernel, &user);
  CloseHandle(h);
  if (!ok) return Liveness::kUnknown;
  if (exitCode != STILL_ACTIVE) return Liveness::kDead;
  if (times) {
    int64_t start = (static_cast<int64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    if (start != id.startTime) return Liveness::kDead;  // pid reused
  }
  return Liveness::kAlive;
}


Liveness CheckPidAlive(int32_t pid) {
  if (pid <= 0) return Liveness::kUnknown;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (h == nullptr) {
    return GetLastError() == ERROR_ACCESS_DENIED ? Liveness::kUnknown : Liveness::kDead;
  }
  DWORD exitCode = 0;
  BOOL ok = GetExitCodeProcess(h, &exitCode);
  CloseHandle(h);
  if (!ok) return Liveness::kUnknown;
  return exitCode != STILL_ACTIVE ? Liveness::kDead : Liveness::kAlive;
}

#else

Identity SelfIdentity() {
  return Identity{0, 0, -1, -1};
}

Liveness CheckLiveness(const Identity&) {
  return Liveness::kUnknown;
}


Liveness CheckPidAlive(int32_t) {
  return Liveness::kUnknown;
}

#endif

const char* CheckLivenessJs(int32_t pid, int64_t startTime, int64_t pidNsInode) {
  const Identity id{pid, 0, startTime, pidNsInode};
  switch (CheckLiveness(id)) {
    case Liveness::kAlive: return "alive";
    case Liveness::kDead: return "dead";
    case Liveness::kUnknown: return "unknown";
  }
  return "unknown";
}

}  // namespace shm_bridge
