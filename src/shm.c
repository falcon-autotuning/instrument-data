#include "internal/shm.h"
#include "internal/util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ============================================================
 * PLATFORM PREFIX
 * ============================================================ */
#ifdef _WIN32
#define INST_SHM_PREFIX "Local\\"
#else
#define INST_SHM_PREFIX "/"
#endif

char *inst_build_shm_name(const char *id, const char *kind) {
  if (!id || !kind)
    return NULL;

  char buf[256];

  snprintf(buf, sizeof(buf), "%sshm_%s_%s", INST_SHM_PREFIX, kind, id);

  return inst_strdup(buf);
}

/* ============================================================
 * WINDOWS IMPLEMENTATION
 * ============================================================ */
#ifdef _WIN32

void *inst_shm_create(InstShmHandle *out, size_t size, const char *id,
                      const char *kind) {

  if (size == 0)
    size = 1;

  char *name = inst_build_shm_name(id, kind);
  if (!name)
    return NULL;

  HANDLE h = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                               (DWORD)size, name);

  if (!h) {
    fprintf(stderr, "CreateFileMapping failed (%s): %lu\n", name,
            GetLastError());
    free(name);
    return NULL;
  }

  void *ptr = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);

  if (!ptr) {
    fprintf(stderr, "MapViewOfFile failed (%s): %lu\n", name, GetLastError());
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

  if (!name)
    return NULL;
  if (size == 0)
    size = 1;

  HANDLE h = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                               (DWORD)size, name);

  if (!h) {
    fprintf(stderr, "CreateFileMapping(open_or_create) failed (%s): %lu\n",
            name, GetLastError());
    return NULL;
  }

  void *ptr = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);

  if (!ptr) {
    fprintf(stderr, "MapViewOfFile(open_or_create) failed (%s): %lu\n", name,
            GetLastError());
    CloseHandle(h);
    return NULL;
  }

  out->name = inst_strdup(name); // IMPORTANT: duplicate name
  out->size = size;
  out->handle = h;

  return ptr;
}

void *inst_shm_map(InstShmHandle *h) {
  HANDLE hm = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, h->name);

  if (!hm) {
    fprintf(stderr, "OpenFileMapping failed (%s): %lu\n", h->name,
            GetLastError());
    return NULL;
  }

  void *ptr = MapViewOfFile(hm, FILE_MAP_ALL_ACCESS, 0, 0, h->size);

  if (!ptr) {
    fprintf(stderr, "MapViewOfFile(open) failed (%s): %lu\n", h->name,
            GetLastError());
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

void inst_shm_unlink_name(const char *name) {
  /* Windows auto-cleans when handles close */
  (void)name;
}

/* ---------------- MUTEX ---------------- */

void *inst_ipc_mutex_create(const char *name) {
  if (!name)
    return NULL;

  char buffer[256];
  snprintf(buffer, sizeof(buffer), INST_SHM_PREFIX "mtx_%s", name);

  HANDLE h = CreateMutex(NULL, FALSE, buffer);
  if (!h) {
    fprintf(stderr, "CreateMutex failed (%s): %lu\n", buffer, GetLastError());
    return NULL;
  }

  return (void *)h;
}

void inst_ipc_mutex_lock(void *h) {
  if (!h)
    return;

  DWORD res = WaitForSingleObject((HANDLE)h, INFINITE);
  if (res != WAIT_OBJECT_0) {
    fprintf(stderr, "WaitForSingleObject failed: %lu\n", GetLastError());
  }
}

void inst_ipc_mutex_unlock(void *h) {
  if (!h)
    return;

  if (!ReleaseMutex((HANDLE)h)) {
    fprintf(stderr, "ReleaseMutex failed: %lu\n", GetLastError());
  }
}

void inst_ipc_mutex_destroy(void *h) {
  if (!h)
    return;

  CloseHandle((HANDLE)h);
}

#else

/* ============================================================
 * POSIX IMPLEMENTATION
 * ============================================================ */

void *inst_shm_create(InstShmHandle *out, size_t size, const char *id,
                      const char *kind) {

  if (size == 0)
    size = 1;

  char *name = inst_build_shm_name(id, kind);
  if (!name)
    return NULL;

  /* cleanup stale */
  shm_unlink(name); // safe best-effort

  int fd = shm_open(name, O_CREAT | O_RDWR, 0666);

  if (fd == -1) {
    perror("shm_open");
    free(name);
    return NULL;
  }

  if (ftruncate(fd, size) == -1) {
    perror("ftruncate");
    close(fd);
    shm_unlink(name);
    free(name);
    return NULL;
  }

  void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (ptr == MAP_FAILED) {
    perror("mmap");
    close(fd);
    shm_unlink(name);
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

  if (!name)
    return NULL;
  if (size == 0)
    size = 1;

  int fd = shm_open(name, O_RDWR | O_CREAT, 0666);

  if (fd == -1) {
    perror("shm_open(open_or_create)");
    return NULL;
  }

  /* ✅ Only resize if new */
  struct stat st;
  if (fstat(fd, &st) == 0 && st.st_size == 0) {
    if (ftruncate(fd, size) == -1) {
      perror("ftruncate(open_or_create)");
      close(fd);
      return NULL;
    }
  }

  void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (ptr == MAP_FAILED) {
    perror("mmap(open_or_create)");
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

#include <semaphore.h>

void *inst_ipc_mutex_create(const char *name) {
  if (!name)
    return NULL;

  char buffer[256];
  snprintf(buffer, sizeof(buffer), INST_SHM_PREFIX "mtx_%s", name);

  sem_t *sem = sem_open(buffer, O_CREAT, 0666, 1);
  if (sem == SEM_FAILED) {
    perror("sem_open");
    return NULL;
  }

  return (void *)sem;
}

void inst_ipc_mutex_lock(void *h) {
  if (!h)
    return;
  sem_wait((sem_t *)h);
}

void inst_ipc_mutex_unlock(void *h) {
  if (!h)
    return;
  sem_post((sem_t *)h);
}

void inst_ipc_mutex_destroy(void *h) {
  if (!h)
    return;
  sem_close((sem_t *)h);
}
#endif

/* ============================================================
 * COMMON CLOSE
 * ============================================================ */

void inst_shm_close(InstShmHandle *h, void *ptr) {
  if (!h)
    return;

  inst_shm_unmap(h, ptr);

#ifndef _WIN32
  if (h->fd >= 0) {
    close(h->fd);
    h->fd = -1;
  }
#endif

  if (h->name) {
    free(h->name);
    h->name = NULL;
  }
}
