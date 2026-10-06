// liveness.cc — §7.1: who is still alive?
// Identity = pid + startTime + pid-namespace inode + threadId. PIDs get
// reused, containers that share /dev/shm see different PIDs for the same
// process, and all workers share one PID (threadId separates them).

#include "liveness.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

namespace membridge {

#if defined(__linux__)

namespace {

// /proc/<pid>/stat field 22 (starttime, in clock ticks after boot). The comm
// field may contain spaces and parens, so parse after the last ')'.
bool ReadStartTime(int32_t pid, int64_t* out) {
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
  const char* save = nullptr;
  char token[128];
  for (const char* q = p + 1; *q != '\0';) {
    while (*q == ' ') q++;
    if (*q == '\0') break;
    size_t i = 0;
    while (*q != '\0' && *q != ' ' && i < sizeof(token) - 1) token[i++] = *q++;
    token[i] = '\0';
    if (field == 22) {
      *out = std::strtoll(token, nullptr, 10);
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
  Identity id;
  id.pid = static_cast<int32_t>(getpid());
  id.threadId = static_cast<int32_t>(syscall(SYS_gettid));
  int64_t start = -1;
  ReadStartTime(id.pid, &start);
  id.startTime = start;
  id.pidNsInode = ReadPidNsInode(0);
  return id;
}

Liveness CheckLiveness(const Identity& id) {
  if (id.pid <= 0) return Liveness::kDead;
  const int64_t ownNs = ReadPidNsInode(0);
  if (id.pidNsInode > 0 && ownNs > 0 && id.pidNsInode != ownNs) {
    return Liveness::kUnknown;  // foreign pid namespace: never steal (§7.1)
  }
  // kill(pid, 0): success or EPERM = alive, ESRCH = dead.
  if (kill(static_cast<pid_t>(id.pid), 0) == 0 || errno != ESRCH) {
    int64_t start = -1;
    if (ReadStartTime(id.pid, &start) && start != id.startTime) {
      return Liveness::kDead;  // pid reused
    }
    return Liveness::kAlive;
  }
  return Liveness::kDead;
}

#elif defined(__APPLE__)

Identity SelfIdentity() {
  Identity id;
  id.pid = static_cast<int32_t>(getpid());
  id.threadId = static_cast<int32_t>(syscall(SYS_thread_selfid));
  int64_t start = -1;
  proc_bsdinfo info{};
  if (proc_pidinfo(id.pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info)) {
    start = static_cast<int64_t>(info.pbi_start_tvsec);
  }
  id.startTime = start;
  id.pidNsInode = -1;  // no pid namespaces on macOS
  return id;
}

Liveness CheckLiveness(const Identity& id) {
  if (id.pid <= 0) return Liveness::kDead;
  if (kill(static_cast<pid_t>(id.pid), 0) == 0 || errno != ESRCH) {
    proc_bsdinfo info{};
    if (proc_pidinfo(id.pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info) &&
        static_cast<int64_t>(info.pbi_start_tvsec) != id.startTime) {
      return Liveness::kDead;  // pid reused
    }
    return Liveness::kAlive;
  }
  return Liveness::kDead;
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

#else

Identity SelfIdentity() {
  return Identity{0, 0, -1, -1};
}

Liveness CheckLiveness(const Identity&) {
  return Liveness::kUnknown;
}

#endif

}  // namespace membridge
