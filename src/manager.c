#include "instrument-data.h"
#include "internal/buffer.h"
#include "internal/process.h"
#include "internal/registry.h"
#include "internal/shm.h"
#include "internal/util.h"

#include <threads.h>
#include <uthash.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Helpers
 * ============================================================ */
static char *inst_make_buffer_id(void) { return inst_make_name("buffer"); }

/* ============================================================
 * Hash table (uthash)
 * ============================================================ */

typedef struct {
  char *id;
  DataBuffer *buf;
  UT_hash_handle hh;
} MapEntry;

static MapEntry *map = NULL;

static mtx_t lock;
static once_flag init_once = ONCE_FLAG_INIT;

static void init_impl(void) { mtx_init(&lock, mtx_plain); }

static void init(void) { call_once(&init_once, init_impl); }

static DataBuffer *lookup_buffer_no_lock(const char *id) {
  MapEntry *e;
  HASH_FIND_STR(map, id, e);
  return e ? e->buf : NULL;
}

static void insert_buffer(const char *id, DataBuffer *buffer) {
  MapEntry *e = malloc(sizeof(MapEntry));
  e->id = inst_strdup(id);
  e->buf = buffer;
  HASH_ADD_KEYPTR(hh, map, e->id, strlen(e->id), e);
}

/* ============================================================
 * Utilities
 * ============================================================ */

static void cleanup_dead_owners(DataBuffer *b) {
  uint32_t new_count = 0;

  for (uint32_t i = 0; i < b->meta->global_ref_count; i++) {
    uint32_t pid = b->meta->owners[i];

    if (inst_process_alive(pid)) {
      b->meta->owners[new_count++] = pid;
    }
  }

  b->meta->global_ref_count = new_count;
}

static size_t inst_data_type_size(ArrayType type) {
  switch (type) {
  case INST_DATA_FLOAT32:
    return sizeof(float);
  case INST_DATA_FLOAT64:
    return sizeof(double);
  case INST_DATA_INT32:
    return sizeof(int32_t);
  case INST_DATA_INT64:
    return sizeof(int64_t);
  case INST_DATA_UINT32:
    return sizeof(uint32_t);
  case INST_DATA_UINT64:
    return sizeof(uint64_t);
  case INST_DATA_UINT8:
    return sizeof(uint8_t);
  default:
    return 0;
  }
}
// For debugging locks
#include <windows.h> // for GetCurrentThreadId()

static _Thread_local int lock_depth = 0;
static _Thread_local unsigned long owner_tid = 0;

static void debug_lock(void) {
  unsigned long tid = GetCurrentThreadId();

  if (lock_depth > 0 && owner_tid == tid) {
    printf("\n🔥 RECURSIVE LOCK DETECTED 🔥\n");
    printf("Thread: %lu\n", tid);

    // Force crash to get stack trace
    abort();
  }

  mtx_lock(&lock);
  lock_depth++;
  owner_tid = tid;
}

static void debug_unlock(void) {
  lock_depth--;
  mtx_unlock(&lock);
}

static void remove_buffer(const char *id) {
  MapEntry *e;
  HASH_FIND_STR(map, id, e);
  if (e) {
    HASH_DEL(map, e);
    free(e->id);
    free(e);
  }
}

/* ============================================================
 * Core API
 * ============================================================ */

char *data_manager_create_buffer(const char *instrument, const char *command_id,
                                 ArrayType type, size_t count,
                                 const void *data) {
  init();

  size_t sz = inst_data_type_size(type);
  if (sz == 0)
    return NULL;

  size_t bytes = count * sz;

  char *id = inst_make_buffer_id();
  if (!id)
    return NULL;

  InstShmHandle sd, sm;

  void *ptr = inst_shm_create(&sd, bytes);
  SharedMetadata *meta = inst_shm_create(&sm, sizeof(SharedMetadata));

  if (!ptr || !meta) {
    if (ptr)
      inst_shm_close(&sd, ptr);
    if (meta)
      inst_shm_close(&sm, meta);
    free(id);
    return NULL;
  }

  if (data)
    memcpy(ptr, data, bytes);
  else
    memset(ptr, 0, bytes);

  inst_strlcpy(meta->buffer_id, id, INST_MAX_STRING_LEN);
  inst_strlcpy(meta->instrument_name, instrument, INST_MAX_STRING_LEN);
  inst_strlcpy(meta->command_id, command_id, INST_MAX_STRING_LEN);

  meta->type = type;
  meta->element_count = count;
  meta->byte_size = bytes;
  meta->timestamp_ms = 0; /* you can plug in your time helper */

  meta->global_ref_count = 1;
  meta->owners[0] = inst_get_pid();

  DataBuffer *buffer = calloc(1, sizeof(DataBuffer));

  buffer->id = inst_strdup(id);
  buffer->data = ptr;
  buffer->meta = meta;
  buffer->shm_data = sd;
  buffer->shm_meta = sm;
  buffer->mutex = inst_ipc_mutex_create(id);
  atomic_init(&buffer->ref_count, 1);

  registry_add(buffer);

  debug_lock();
  insert_buffer(id, buffer);
  debug_unlock();

  return id;
}
char *data_manager_create_buffer_zero_copy(const char *instrument,
                                           const char *command_id,
                                           ArrayType type, size_t element_count,
                                           void **out_ptr) {
  if (!out_ptr) {
    return NULL;
  }

  char *id = data_manager_create_buffer(instrument, command_id, type,
                                        element_count, NULL);
  if (!id) {
    *out_ptr = NULL;
    return NULL;
  }

  char safe_id[INST_MAX_STRING_LEN];
  inst_strlcpy(safe_id, id, sizeof(safe_id));

  debug_lock();
  DataBuffer *buffer = lookup_buffer_no_lock(safe_id);
  if (buffer)
    data_buffer_ref(buffer);
  debug_unlock();

  if (!buffer) {
    *out_ptr = NULL;
    return id;
  }

  *out_ptr = buffer->data;

  return id;
}

/* ------------------------------------------------------------ */

DataBuffer *data_manager_get_buffer(const char *id) {
  init();

  static _Thread_local int in_get_buffer = 0;

  if (in_get_buffer > 0) {
    printf("🔥 Recursive get_buffer detected for id=%s\n", id);
    return NULL;
  }

  in_get_buffer++;

  if (!id) {
    in_get_buffer--;
    return NULL;
  }

  char safe_id[INST_MAX_STRING_LEN];
  inst_strlcpy(safe_id, id, sizeof(safe_id));

  debug_lock();
  DataBuffer *b = lookup_buffer_no_lock(safe_id);

  if (!b) {
    debug_unlock(); // release before expensive work

    InstShmHandle sd = inst_shm_handle_init();
    InstShmHandle sm = inst_shm_handle_init();

    if (!registry_find(safe_id, &sd, &sm)) {
      in_get_buffer--; // ✅ IMPORTANT
      return NULL;
    }

    void *ptr = inst_shm_map(&sd);
    SharedMetadata *meta = inst_shm_map(&sm);

    DataBuffer *new_buf = calloc(1, sizeof(DataBuffer));
    new_buf->id = inst_strdup(safe_id);
    new_buf->data = ptr;
    new_buf->meta = meta;
    new_buf->shm_data = sd;
    new_buf->shm_meta = sm;
    new_buf->mutex = inst_ipc_mutex_create(safe_id);
    atomic_init(&new_buf->ref_count, 1);

    debug_lock(); // reacquire *fresh*

    DataBuffer *existing = lookup_buffer_no_lock(safe_id);
    if (existing) {
      debug_unlock();

      data_buffer_unref(new_buf);
      b = existing;
    } else {
      insert_buffer(safe_id, new_buf);
      b = new_buf;
      debug_unlock();
    }
  } else {
    debug_unlock();
  }

  data_buffer_ref(b);

  inst_ipc_mutex_lock(b->mutex);

  cleanup_dead_owners(b);

  uint32_t pid = inst_get_pid();
  bool found = false;

  for (uint32_t i = 0; i < b->meta->global_ref_count; i++) {
    if (b->meta->owners[i] == pid) {
      found = true;
      break;
    }
  }

  if (!found && b->meta->global_ref_count < INST_MAX_OWNERS) {
    b->meta->owners[b->meta->global_ref_count++] = pid;
  }

  inst_ipc_mutex_unlock(b->mutex);

  in_get_buffer--; // ✅ ADD THIS at end

  return b;
}

/* ------------------------------------------------------------ */

void data_manager_release_buffer(const char *id) {
  DataBuffer *buffer = NULL;

  char safe_id[INST_MAX_STRING_LEN];
  inst_strlcpy(safe_id, id, sizeof(safe_id));

  debug_lock();
  buffer = lookup_buffer_no_lock(safe_id);
  debug_unlock();

  if (!buffer)
    return;

  bool should_remove = false;

  inst_ipc_mutex_lock(buffer->mutex);

  uint32_t pid = inst_get_pid();
  uint32_t new_count = 0;

  for (uint32_t i = 0; i < buffer->meta->global_ref_count; i++) {
    if (buffer->meta->owners[i] != pid) {
      buffer->meta->owners[new_count++] = buffer->meta->owners[i];
    }
  }

  buffer->meta->global_ref_count = new_count;

  should_remove = (new_count == 0);

  inst_ipc_mutex_unlock(buffer->mutex);

  if (should_remove) {
    debug_lock();
    remove_buffer(id);
    debug_unlock();

    registry_remove(id);
    inst_shm_unlink_name(buffer->shm_data.name);
    inst_shm_unlink_name(buffer->shm_meta.name);
  }

  data_buffer_unref(buffer);
}

/* ------------------------------------------------------------ */

bool data_manager_add_offset(const char *id, double offset) {
  DataBuffer *buffer = data_manager_get_buffer(id);
  if (!buffer)
    return false;

  inst_ipc_mutex_lock(buffer->mutex);

  size_t count = buffer->meta->element_count;

  if (buffer->meta->type == INST_DATA_FLOAT32) {
    float *d = buffer->data;
    for (size_t i = 0; i < count; i++)
      d[i] += (float)offset;
  } else if (buffer->meta->type == INST_DATA_FLOAT64) {
    double *d = buffer->data;
    for (size_t i = 0; i < count; i++)
      d[i] += offset;
  } else {
    inst_ipc_mutex_unlock(buffer->mutex);
    data_buffer_unref(buffer);
    return false;
  }

  inst_ipc_mutex_unlock(buffer->mutex);
  data_buffer_unref(buffer);
  return true;
}

/* ------------------------------------------------------------ */

bool data_manager_multiply_gain(const char *id, double gain) {
  DataBuffer *buffer = data_manager_get_buffer(id);
  if (!buffer)
    return false;

  inst_ipc_mutex_lock(buffer->mutex);

  size_t count = buffer->meta->element_count;

  if (buffer->meta->type == INST_DATA_FLOAT32) {
    float *d = buffer->data;
    for (size_t i = 0; i < count; i++)
      d[i] *= (float)gain;
  } else if (buffer->meta->type == INST_DATA_FLOAT64) {
    double *d = buffer->data;
    for (size_t i = 0; i < count; i++)
      d[i] *= gain;
  } else {
    inst_ipc_mutex_unlock(buffer->mutex);
    data_buffer_unref(buffer);
    return false;
  }

  inst_ipc_mutex_unlock(buffer->mutex);
  data_buffer_unref(buffer);
  return true;
}

/* ------------------------------------------------------------ */

char **data_manager_list_buffers(size_t *count) { return registry_list(count); }

size_t data_manager_total_memory_usage(void) { return registry_total_memory(); }

bool data_manager_get_metadata(const char *id, SharedMetadata *out_meta) {
  init();

  if (!id || !out_meta) {
    return false;
  }

  debug_lock();
  DataBuffer *buffer = lookup_buffer_no_lock(id);
  debug_unlock();

  if (buffer) {
    inst_ipc_mutex_lock(buffer->mutex);

    memcpy(out_meta, buffer->meta, sizeof(SharedMetadata));

    inst_ipc_mutex_unlock(buffer->mutex);
    return true;
  }

  /* Fallback: use registry */
  InstShmHandle sd = inst_shm_handle_init();
  InstShmHandle sm = inst_shm_handle_init();

  if (!registry_find(id, &sd, &sm)) {
    return false;
  }

  /* Map metadata shared memory */
  SharedMetadata *meta = inst_shm_map(&sm);

  if (!meta) {
    return false;
  }

  memcpy(out_meta, meta, sizeof(SharedMetadata));

  inst_shm_unmap(&sm, meta);

  return true;
}
