#include "shm.h"
#include <glib.h>

#ifdef _WIN32

/* ========================= WINDOWS IMPLEMENTATION ========================= */

#include <windows.h>

/* ---------------- SHARED MEMORY ---------------- */

void *inst_shm_create(InstShmHandle *out, size_t size) {
  gchar *name = g_strdup_printf("Global\\inst_%u", g_random_int());

  HANDLE h = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                               (DWORD)size, name);

  if (!h) {
    g_free(name);
    return NULL;
  }

  void *ptr = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);

  if (!ptr) {
    CloseHandle(h);
    g_free(name);
    return NULL;
  }

  out->name = name;
  out->size = size;
  out->handle = h;

  return ptr;
}

void *inst_shm_open_or_create(InstShmHandle *out, const char *name,
                              size_t size) {

  HANDLE h = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                               (DWORD)size, name);

  if (!h) {
    return NULL;
  }

  void *ptr = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);

  if (!ptr) {
    CloseHandle(h);
    return NULL;
  }

  out->name = g_strdup(name);
  out->size = size;
  out->handle = h;

  return ptr;
}

void *inst_shm_map(InstShmHandle *h) {

  HANDLE hMap = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, h->name);
  if (!hMap) {
    return NULL;
  }

  void *ptr = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, h->size);

  if (!ptr) {
    CloseHandle(hMap);
    return NULL;
  }

  /* ✅ keep handle so we can close it later */
  h->handle = hMap;

  return ptr;
}

void inst_shm_unmap(InstShmHandle *h, void *ptr) {
  if (ptr) {
    UnmapViewOfFile(ptr);
  }

  if (h->handle) {
    CloseHandle(h->handle);
    h->handle = NULL;
  }
}

void inst_shm_unlink_name(const char *name) {
  /* Windows does automatic cleanup when last handle closes */
  (void)name;
}

/* ---------------- MUTEX ---------------- */

void *inst_ipc_mutex_create(const char *name) {

  gchar *full = g_strdup_printf("Global\\mtx_%s", name);

  HANDLE h = CreateMutex(NULL, FALSE, full);

  g_free(full);
  return h;
}

void inst_ipc_mutex_lock(void *h) {

  DWORD res = WaitForSingleObject((HANDLE)h, INFINITE);

  if (res == WAIT_ABANDONED) {
    /* previous owner crashed → safe to continue */
  }
}

void inst_ipc_mutex_unlock(void *h) { ReleaseMutex((HANDLE)h); }

void inst_ipc_mutex_destroy(void *h) { CloseHandle((HANDLE)h); }

/* ========================= LINUX / POSIX IMPLEMENTATION
 * ========================= */

#else

#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---------------- SHARED MEMORY ---------------- */

void *inst_shm_create(InstShmHandle *out, size_t size) {

  gchar *name = g_strdup_printf("/inst_%u", g_random_int());

  int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
  if (fd == -1) {
    g_free(name);
    return NULL;
  }

  if (ftruncate(fd, size) == -1) {
    close(fd);
    g_free(name);
    return NULL;
  }

  void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (ptr == MAP_FAILED) {
    close(fd);
    g_free(name);
    return NULL;
  }

  out->name = name;
  out->size = size;
  out->fd = fd;

  return ptr;
}

void *inst_shm_open_or_create(InstShmHandle *out, const char *name,
                              size_t size) {

  int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
  if (fd == -1) {
    return NULL;
  }

  if (ftruncate(fd, size) == -1) {
    close(fd);
    return NULL;
  }

  void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (ptr == MAP_FAILED) {
    close(fd);
    return NULL;
  }

  out->name = g_strdup(name);
  out->size = size;
  out->fd = fd;

  return ptr;
}

void *inst_shm_map(InstShmHandle *h) {

  int fd = shm_open(h->name, O_RDWR, 0666);
  if (fd == -1) {
    return NULL;
  }

  void *ptr = mmap(NULL, h->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (ptr == MAP_FAILED) {
    close(fd);
    return NULL;
  }

  h->fd = fd;

  return ptr;
}

void inst_shm_unmap(InstShmHandle *h, void *ptr) {
  if (ptr) {
    munmap(ptr, h->size);
  }

  if (h->fd >= 0) {
    close(h->fd);
    h->fd = -1;
  }
}

void inst_shm_unlink_name(const char *name) { shm_unlink(name); }

/* ---------------- MUTEX ---------------- */

void *inst_ipc_mutex_create(const char *name) {

  gchar *n = g_strdup_printf("/mtx_%s", name);

  sem_t *sem = sem_open(n, O_CREAT, 0666, 1);

  if (sem == SEM_FAILED) {
    g_free(n);
    return NULL;
  }

  int val = 0;
  if (sem_getvalue(sem, &val) == 0 && val == 0) {
    sem_post(sem);
  }

  g_free(n);
  return sem;
}

void inst_ipc_mutex_lock(void *h) { sem_wait((sem_t *)h); }

void inst_ipc_mutex_unlock(void *h) { sem_post((sem_t *)h); }

void inst_ipc_mutex_destroy(void *h) { sem_close((sem_t *)h); }

#endif
