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

  mtx_lock(&lock);
  insert_buffer(id, buffer);
  mtx_unlock(&lock);

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

  DataBuffer *buffer = data_manager_get_buffer(id);
  if (!buffer) {
    *out_ptr = NULL;
    return id;
  }
  *out_ptr = buffer->data;
  data_buffer_unref(buffer);

  return id;
}

/* ------------------------------------------------------------ */

DataBuffer *data_manager_get_buffer(const char *id) {
  init();

  mtx_lock(&lock);
  DataBuffer *b = lookup_buffer_no_lock(id);
  mtx_unlock(&lock);

  if (!b) {
    InstShmHandle sd = {
        .name = NULL,
        .size = 0,
#ifdef _WIN32
        .handle = NULL,
#else
        .fd = -1,
#endif
    };
    InstShmHandle sm = inst_shm_handle_init();

    if (!registry_find(id, &sd, &sm)) {
      return NULL;
    }

    void *ptr = inst_shm_map(&sd);
    SharedMetadata *meta = inst_shm_map(&sm);

    b = calloc(1, sizeof(DataBuffer));
    b->id = inst_strdup(id);
    b->data = ptr;
    b->meta = meta;
    b->shm_data = sd;
    b->shm_meta = sm;
    b->mutex = inst_ipc_mutex_create(id);
    atomic_init(&b->ref_count, 1);

    mtx_lock(&lock);
    insert_buffer(id, b);
    mtx_unlock(&lock);
  }

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

  data_buffer_ref(b);
  return b;
}

/* ------------------------------------------------------------ */

void data_manager_release_buffer(const char *id) {
  DataBuffer *buffer = NULL;

  mtx_lock(&lock);
  buffer = lookup_buffer_no_lock(id);
  mtx_unlock(&lock);

  if (!buffer)
    return;

  inst_ipc_mutex_lock(buffer->mutex);

  uint32_t pid = inst_get_pid();
  uint32_t new_count = 0;

  for (uint32_t i = 0; i < buffer->meta->global_ref_count; i++) {
    if (buffer->meta->owners[i] != pid) {
      buffer->meta->owners[new_count++] = buffer->meta->owners[i];
    }
  }

  buffer->meta->global_ref_count = new_count;

  if (new_count == 0) {
    registry_remove(id);
    inst_shm_unlink_name(buffer->shm_data.name);
    inst_shm_unlink_name(buffer->shm_meta.name);
  }

  inst_ipc_mutex_unlock(buffer->mutex);

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

  mtx_lock(&lock);
  DataBuffer *buffer = lookup_buffer_no_lock(id);
  mtx_unlock(&lock);

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
