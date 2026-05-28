
#include "process.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32

#include <windows.h>

uint32_t inst_get_pid(void) { return (uint32_t)GetCurrentProcessId(); }

bool inst_process_alive(uint32_t pid) {
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!h)
    return false;

  DWORD ret = WaitForSingleObject(h, 0);
  CloseHandle(h);

  return ret == WAIT_TIMEOUT;
}

#else

#include <errno.h>
#include <signal.h>
#include <unistd.h>

uint32_t inst_get_pid(void) { return (uint32_t)getpid(); }

bool inst_process_alive(uint32_t pid) {
  if (kill((pid_t)pid, 0) == 0)
    return true;

  return errno != ESRCH;
}

#endif
