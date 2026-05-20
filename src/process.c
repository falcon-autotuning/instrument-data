#include "process.h"

#ifdef _WIN32

#include <windows.h>

guint32 inst_get_pid(void) { return GetCurrentProcessId(); }

gboolean inst_process_alive(guint32 pid) {
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!h)
    return FALSE;

  DWORD ret = WaitForSingleObject(h, 0);
  CloseHandle(h);

  return ret == WAIT_TIMEOUT;
}

#else

#include <signal.h>
#include <unistd.h>

guint32 inst_get_pid(void) { return (guint32)getpid(); }

gboolean inst_process_alive(guint32 pid) { return kill(pid, 0) == 0; }

#endif
