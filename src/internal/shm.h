#pragma once

#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {
  char *name;
  size_t size;

#ifdef _WIN32
  HANDLE handle;
#else
  int fd;
#endif

} InstShmHandle;
static inline InstShmHandle inst_shm_handle_init(void) {
  InstShmHandle h;
  h.name = NULL;
  h.size = 0;
#ifdef _WIN32
  h.handle = NULL;
#else
  h.fd = -1;
#endif
  return h;
}

/* Shared memory */

void *inst_shm_create(InstShmHandle *out, size_t size, const char *id,
                      const char *kind);
void *inst_shm_open_or_create(InstShmHandle *out, const char *name,
                              size_t size);
void *inst_shm_map(InstShmHandle *h);
void inst_shm_unmap(InstShmHandle *h, void *ptr);
void inst_shm_unlink_name(const char *name);
void inst_shm_close(InstShmHandle *h, void *ptr);

/* IPC mutex */
void *inst_ipc_mutex_create(const char *name);
void inst_ipc_mutex_lock(void *handle);
void inst_ipc_mutex_unlock(void *handle);
void inst_ipc_mutex_destroy(void *handle);

char *inst_build_shm_name(const char *id, const char *kind);
