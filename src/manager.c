#include "instrument-data.h"
#include "internal/buffer.h"
#include "internal/process.h"
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
static char *inst_make_buffer_id(void) {
  char *id = inst_make_name("buffer");
  fprintf(stderr, "MAKE_BUFFER_ID → %p (%s)\n", (void *)id, id);
  return id;
}

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
  fprintf(stderr, "LOOKUP: id=%s\n", id);
  MapEntry *e;
  HASH_FIND_STR(map, id, e);
  return e ? e->buf : NULL;
}

static void insert_buffer(const char *id, DataBuffer *buffer) {
  fprintf(stderr, "INSERT: id=%s\n", id);
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

static void remove_buffer(const char *id) {
  MapEntry *e;
  HASH_FIND_STR(map, id, e);
  if (e) {
    HASH_DEL(map, e);
    free(e->id);
    free(e);
  }
}

static DataBuffer *_get_buffer_internal(const char *id) {
  if (!id || id[0] == '\0')
    return NULL;

  fprintf(stderr, "INTERNAL GET: id=%s\n", id);

  DataBuffer *b = lookup_buffer_no_lock(id);
  if (b) {
    data_buffer_ref(b);
    return b;
  }

  /* Build names */
  char *data_name = inst_build_shm_name(id, "data");
  char *meta_name = inst_build_shm_name(id, "meta");

  if (!data_name || !meta_name) {
    free(data_name);
    free(meta_name);
    return NULL;
  }

  /* Prepare handles */
  InstShmHandle sd = inst_shm_handle_init();
  InstShmHandle sm = inst_shm_handle_init();

  sd.name = data_name;
  sm.name = meta_name;

  /* ✅ STEP 1: map metadata FIRST (fixed size) */
  sm.size = sizeof(SharedMetadata);

  SharedMetadata *meta = inst_shm_map(&sm);
  if (!meta) {
    fprintf(stderr, "🔥 META MAP FAILED: id=%s\n", id);
    free(data_name);
    free(meta_name);
    return NULL;
  }

  /* ✅ Validate metadata */
  if (meta->byte_size == 0 || meta->element_count == 0) {
    fprintf(stderr, "🔥 INVALID META CONTENT: id=%s\n", id);
    inst_shm_unmap(&sm, meta);
    free(data_name);
    free(meta_name);
    return NULL;
  }

  /* ✅ STEP 2: now map data using real size */
  sd.size = meta->byte_size;

  void *ptr = inst_shm_map(&sd);
  if (!ptr) {
    fprintf(stderr, "🔥 DATA MAP FAILED: id=%s size=%zu\n", id,
            (size_t)sd.size);
    inst_shm_unmap(&sm, meta);
    free(data_name);
    free(meta_name);
    return NULL;
  }

  /* ✅ CRITICAL VALIDATION FIX */
  if (meta->element_count == 0 || meta->byte_size == 0) {
    fprintf(stderr, "🔥 INVALID META AFTER MAP: id=%s count=%zu bytes=%zu\n",
            id, (size_t)meta->element_count, (size_t)meta->byte_size);

    inst_shm_unmap(&sd, ptr);
    inst_shm_unmap(&sm, meta);
    return NULL;
  }

  /* ✅ Construct buffer only after validation */
  DataBuffer *new_buf = calloc(1, sizeof(DataBuffer));
  if (!new_buf) {
    inst_shm_unmap(&sd, ptr);
    inst_shm_unmap(&sm, meta);
    return NULL;
  }

  new_buf->id = inst_strdup(id);
  new_buf->data = ptr;
  new_buf->meta = meta;
  new_buf->shm_data = sd;
  new_buf->shm_meta = sm;
  new_buf->mutex = inst_ipc_mutex_create(id);
  atomic_init(&new_buf->ref_count, 1);

  if (!new_buf->mutex) {
    fprintf(stderr, "🔥 MUTEX CREATE FAILED: id=%s\n", id);
    inst_shm_unmap(&sd, ptr);
    inst_shm_unmap(&sm, meta);
    free(new_buf->id);
    free(new_buf);
    return NULL;
  }

  insert_buffer(id, new_buf);

  return new_buf;
}

/* ============================================================
 * Core API
 * ============================================================ */

const char *data_manager_create_buffer(const char *instrument,
                                       const char *command_id, ArrayType type,
                                       size_t count, const void *data) {
  init();

  size_t sz = inst_data_type_size(type);
  if (sz == 0)
    return NULL;

  size_t bytes = count * sz;

  char *id = inst_make_buffer_id();
  if (!id)
    return NULL;

  InstShmHandle sd, sm;

  void *ptr = inst_shm_create(&sd, bytes, id, "data");
  SharedMetadata *meta =
      inst_shm_create(&sm, sizeof(SharedMetadata), id, "meta");

  if (!ptr || !meta) {
    fprintf(stderr, "🔥 SHM CREATE FAILED: ptr=%p meta=%p\n", ptr, meta);

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

  /* ✅ Ensure deterministic clean metadata */
  memset(meta, 0, sizeof(SharedMetadata));

  inst_strlcpy(meta->buffer_id, id, INST_MAX_STRING_LEN);
  inst_strlcpy(meta->instrument_name, instrument, INST_MAX_STRING_LEN);
  inst_strlcpy(meta->command_id, command_id, INST_MAX_STRING_LEN);

  meta->type = type;
  meta->element_count = count;
  meta->byte_size = bytes;
  meta->timestamp_ms = 0;

  meta->global_ref_count = 1;
  meta->owners[0] = inst_get_pid();

  /* ✅ CRITICAL: visibility across processes */
  atomic_thread_fence(memory_order_seq_cst);

  DataBuffer *buffer = calloc(1, sizeof(DataBuffer));

  buffer->id = inst_strdup(id);
  buffer->data = ptr;
  buffer->meta = meta;
  buffer->shm_data = sd;
  buffer->shm_meta = sm;
  buffer->mutex = inst_ipc_mutex_create(id);
  atomic_init(&buffer->ref_count, 1);

  mtx_lock(&lock);
  insert_buffer(buffer->id, buffer);
  mtx_unlock(&lock);

  free(id);          // free temporary
  return buffer->id; // ✅ canonical id
}
const char *data_manager_create_buffer_zero_copy(const char *instrument,
                                                 const char *command_id,
                                                 ArrayType type,
                                                 size_t element_count,
                                                 void **out_ptr) {
  if (!out_ptr) {
    return NULL;
  }

  const char *id = data_manager_create_buffer(instrument, command_id, type,
                                              element_count, NULL);
  if (!id) {
    *out_ptr = NULL;
    return NULL;
  }

  char safe_id[INST_MAX_STRING_LEN];
  inst_strlcpy(safe_id, id, sizeof(safe_id));

  mtx_lock(&lock);
  DataBuffer *buffer = _get_buffer_internal(safe_id);
  if (buffer)
    data_buffer_ref(buffer);
  mtx_unlock(&lock);

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

  if (!id || strncmp(id, "buffer_", 7) != 0) {
    fprintf(stderr, "🔥 INVALID ID FORMAT: '%s'\n", id ? id : "NULL");
    return NULL;
  }

  char safe_id[INST_MAX_STRING_LEN];
  inst_strlcpy(safe_id, id, sizeof(safe_id));

  mtx_lock(&lock);
  DataBuffer *b = _get_buffer_internal(safe_id);
  mtx_unlock(&lock);

  if (!b) {
    fprintf(stderr, "GET BUFFER FAILED: id=%s\n", safe_id);
    return NULL;
  }

  inst_ipc_mutex_lock(b->mutex);

  /* ✅ SAFETY: validate metadata before use */
  if (!b->meta || b->meta->element_count == 0) {
    fprintf(stderr, "🔥 INVALID BUFFER META: id=%s\n", safe_id);
    inst_ipc_mutex_unlock(b->mutex);
    data_buffer_unref(b);
    return NULL;
  }

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

  return b;
}

/* ------------------------------------------------------------ */

void data_manager_release_buffer(const char *id) {
  DataBuffer *buffer = NULL;

  char safe_id[INST_MAX_STRING_LEN];
  inst_strlcpy(safe_id, id, sizeof(safe_id));

  mtx_lock(&lock);
  buffer = lookup_buffer_no_lock(safe_id);
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

  char *meta_name = inst_build_shm_name(id, "meta");
  if (!meta_name)
    return false;

  InstShmHandle sm = inst_shm_handle_init();
  sm.name = meta_name;
  sm.size = sizeof(SharedMetadata);

  SharedMetadata *meta = inst_shm_map(&sm);

  if (!meta) {
    free(meta_name);
    return false;
  }

  memcpy(out_meta, meta, sizeof(SharedMetadata));

  inst_shm_unmap(&sm, meta);
  free(meta_name);

  return true;
}
