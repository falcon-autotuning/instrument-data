#include "shm.h"

#include "util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <bcrypt.h>
#include <windows.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* ---------- UUID v4 generator ---------- */

static int inst_random_bytes(uint8_t *buf, size_t len) {
#ifdef _WIN32
  return BCryptGenRandom(NULL, buf, (ULONG)len,
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
    return 0;

  ssize_t r = read(fd, buf, len);
  close(fd);

  return r == (ssize_t)len;
#endif
}

/* ============================================================
 * WINDOWS IMPLEMENTATION
 * ============================================================ */
#ifdef _WIN32

/* ---------------- SHARED MEMORY ---------------- */

void *inst_shm_create(InstShmHandle *out, size_t size) {
  char *name = inst_make_name("Global\\inst");
  if (!name)
    return NULL;

  HANDLE h = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                               (DWORD)size, name);

  if (!h) {
    free(name);
    return NULL;
  }

  void *ptr = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);

  if (!ptr) {
    CloseHandle(h);
    free(name);
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

  if (!h)
    return NULL;

  void *ptr = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!ptr) {
    CloseHandle(h);
    return NULL;
  }

  out->name = inst_strdup(name);
  out->size = size;
  out->handle = h;

  return ptr;
}

void *inst_shm_map(InstShmHandle *h) {
  HANDLE hm = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, h->name);
  if (!hm)
    return NULL;

  void *ptr = MapViewOfFile(hm, FILE_MAP_ALL_ACCESS, 0, 0, h->size);
  if (!ptr) {
    CloseHandle(hm);
    return NULL;
  }

  h->handle = hm;
  return ptr;
}

void inst_shm_unmap(InstShmHandle *h, void *ptr) {
  if (ptr)
    UnmapViewOfFile(ptr);
  if (h->handle) {
    CloseHandle(h->handle);
    h->handle = NULL;
  }
}

void inst_shm_unlink_name(const char *name) { (void)name; /* auto-cleanup */ }

/* ---------------- MUTEX ---------------- */

void *inst_ipc_mutex_create(const char *name) {
  char buffer[128];
  snprintf(buffer, sizeof(buffer), "Global\\mtx_%s", name);
  return CreateMutex(NULL, FALSE, buffer);
}

void inst_ipc_mutex_lock(void *h) {
  DWORD res = WaitForSingleObject((HANDLE)h, INFINITE);
  (void)res;
}

void inst_ipc_mutex_unlock(void *h) { ReleaseMutex((HANDLE)h); }

void inst_ipc_mutex_destroy(void *h) { CloseHandle((HANDLE)h); }

#endif /* _WIN32 */

/* ============================================================
 * POSIX IMPLEMENTATION
 * ============================================================ */
#ifndef _WIN32

/* ---------------- SHARED MEMORY ---------------- */

void *inst_shm_create(InstShmHandle *out, size_t size) {
  char *name = inst_make_name("/inst");
  if (!name)
    return NULL;

  int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
  if (fd == -1) {
    free(name);
    return NULL;
  }

  if (ftruncate(fd, size) == -1) {
    close(fd);
    free(name);
    return NULL;
  }

  void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    close(fd);
    free(name);
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
  if (fd == -1)
    return NULL;

  if (ftruncate(fd, size) == -1) {
    close(fd);
    return NULL;
  }

  void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    close(fd);
    return NULL;
  }

  out->name = inst_strdup(name);
  out->size = size;
  out->fd = fd;

  return ptr;
}

void *inst_shm_map(InstShmHandle *h) {
  int fd = shm_open(h->name, O_RDWR, 0666);
  if (fd == -1)
    return NULL;

  void *ptr = mmap(NULL, h->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    close(fd);
    return NULL;
  }

  h->fd = fd;
  return ptr;
}

void inst_shm_unmap(InstShmHandle *h, void *ptr) {
  if (ptr)
    munmap(ptr, h->size);
  if (h->fd >= 0) {
    close(h->fd);
    h->fd = -1;
  }
}

void inst_shm_unlink_name(const char *name) { shm_unlink(name); }

/* ---------------- MUTEX ---------------- */

void *inst_ipc_mutex_create(const char *name) {
  char buffer[128];
  snprintf(buffer, sizeof(buffer), "/mtx_%s", name);

  sem_t *sem = sem_open(buffer, O_CREAT, 0666, 1);
  if (sem == SEM_FAILED)
    return NULL;

  return sem;
}

void inst_ipc_mutex_lock(void *h) { sem_wait((sem_t *)h); }

void inst_ipc_mutex_unlock(void *h) { sem_post((sem_t *)h); }

void inst_ipc_mutex_destroy(void *h) { sem_close((sem_t *)h); }

#endif
