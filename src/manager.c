#include "buffer.h"
#include "instrument-data.h"
#include "registry.h"
#include "shm.h"

#include "process.h"
#include <glib.h>
#include <string.h>

static void cleanup_dead_owners(DataBuffer *b) {
  guint32 new_count = 0;

  for (guint32 i = 0; i < b->meta->global_ref_count; i++) {
    guint32 pid = b->meta->owners[i];

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
typedef struct {
  DataBuffer *buf;
} Entry;

static GHashTable *map;
static GMutex lock;
static DataBuffer *lookup_buffer_no_lock(const gchar *id) {
  return g_hash_table_lookup(map, id);
}

static void init(void) {
  static gsize once = 0;
  if (g_once_init_enter(&once)) {
    map = g_hash_table_new(g_str_hash, g_str_equal);
    g_mutex_init(&lock);
    g_once_init_leave(&once, 1);
  }
}

static gchar *gen_id(void) {
  return g_strdup_printf("buffer_%lu", g_get_real_time());
}

gchar *data_manager_create_buffer(const gchar *instrument,
                                  const gchar *command_id, ArrayType type,
                                  size_t count, const void *data) {
  init();

  size_t sz = inst_data_type_size(type);
  if (sz == 0) {
    return NULL;
  }

  size_t bytes = count * sz;

  gchar *id = gen_id();

  InstShmHandle sd;
  InstShmHandle sm;

  void *ptr = inst_shm_create(&sd, bytes);
  SharedMetadata *meta = inst_shm_create(&sm, sizeof(SharedMetadata));

  if (!ptr || !meta) {
    g_free(id);
    return NULL;
  }

  if (data != NULL) {
    memcpy(ptr, data, bytes);
  } else {
    /* zero-copy: leave memory uninitialized or zero it */
    memset(ptr, 0, bytes);
  }

  g_strlcpy(meta->buffer_id, id, INST_MAX_STRING_LEN);
  if (instrument) {
    g_strlcpy(meta->instrument_name, instrument, INST_MAX_STRING_LEN);
  }
  if (command_id) {
    g_strlcpy(meta->command_id, command_id, INST_MAX_STRING_LEN);
  }
  meta->type = type;
  meta->element_count = count;
  meta->byte_size = bytes;
  meta->timestamp_ms = g_get_real_time() / 1000;

  /* global lifetime */
  meta->global_ref_count = 1;
  meta->global_ref_count = 1;
  meta->owners[0] = inst_get_pid();

  DataBuffer *buffer = g_new0(DataBuffer, 1);

  buffer->id = g_strdup(id);
  buffer->data = ptr;
  buffer->meta = meta;
  buffer->shm_data = sd;
  buffer->shm_meta = sm;
  buffer->mutex = inst_ipc_mutex_create(id);
  buffer->ref_count = 1;

  registry_add(buffer);

  g_mutex_lock(&lock);
  g_hash_table_insert(map, g_strdup(id), buffer);
  g_mutex_unlock(&lock);

  return id;
}

gchar *data_manager_create_buffer_zero_copy(const gchar *instrument,
                                            const gchar *command_id,
                                            ArrayType type,
                                            size_t element_count,
                                            void **out_ptr) {
  if (!out_ptr) {
    return NULL;
  }
  gchar *id = data_manager_create_buffer(instrument, command_id, type,
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

DataBuffer *data_manager_get_buffer(const gchar *id) {
  init();

  g_mutex_lock(&lock);
  DataBuffer *b = lookup_buffer_no_lock(id);
  g_mutex_unlock(&lock);

  if (!b) {
    InstShmHandle sd = {0};
    InstShmHandle sm = {0};

    if (!registry_find(id, &sd, &sm)) {
      return NULL;
    }

    void *ptr = inst_shm_map(&sd);
    SharedMetadata *meta = inst_shm_map(&sm);

    b = g_new0(DataBuffer, 1);
    b->id = g_strdup(id);
    b->data = ptr;
    b->meta = meta;
    b->shm_data = sd;
    b->shm_meta = sm;
    b->mutex = inst_ipc_mutex_create(id);
    b->ref_count = 1;

    g_hash_table_insert(map, g_strdup(id), b);
  }

  inst_ipc_mutex_lock(b->mutex);

  /* cleanup dead processes */
  cleanup_dead_owners(b);

  /* add self if not already present */
  guint32 pid = inst_get_pid();
  gboolean found = FALSE;

  for (guint32 i = 0; i < b->meta->global_ref_count; i++) {
    if (b->meta->owners[i] == pid) {
      found = TRUE;
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

void data_manager_release_buffer(const gchar *id) {
  DataBuffer *buffer = NULL;

  g_mutex_lock(&lock);
  buffer = lookup_buffer_no_lock(id);
  g_mutex_unlock(&lock);

  if (!buffer) {
    /* fallback attach WITHOUT owner addition */
    InstShmHandle sd = {0}, sm = {0};

    if (!registry_find(id, &sd, &sm)) {
      return;
    }

    void *ptr = inst_shm_map(&sd);
    SharedMetadata *meta = inst_shm_map(&sm);

    buffer = g_new0(DataBuffer, 1);
    buffer->id = g_strdup(id);
    buffer->data = ptr;
    buffer->meta = meta;
    buffer->shm_data = sd;
    buffer->shm_meta = sm;
    buffer->mutex = inst_ipc_mutex_create(id);
    buffer->ref_count = 1;

    g_mutex_lock(&lock);
    g_hash_table_insert(map, g_strdup(id), buffer);
    g_mutex_unlock(&lock);
  }
  if (!buffer) {
    return;
  }

  inst_ipc_mutex_lock(buffer->mutex);

  guint32 pid = inst_get_pid();

  /* remove this process */
  guint32 new_count = 0;

  for (guint32 i = 0; i < buffer->meta->global_ref_count; i++) {
    if (buffer->meta->owners[i] != pid) {
      buffer->meta->owners[new_count++] = buffer->meta->owners[i];
    }
  }

  buffer->meta->global_ref_count = new_count;

  gboolean last = (new_count == 0);

  if (last) {
    registry_remove(id);
    inst_shm_unlink_name(buffer->shm_data.name);
    inst_shm_unlink_name(buffer->shm_meta.name);
  }

  inst_ipc_mutex_unlock(buffer->mutex);

  data_buffer_unref(buffer);
}
gboolean data_manager_add_offset(const gchar *id, double offset) {
  if (!id) {
    return FALSE;
  }

  DataBuffer *buffer = data_manager_get_buffer(id);
  if (!buffer) {
    return FALSE;
  }

  inst_ipc_mutex_lock(buffer->mutex);

  size_t count = buffer->meta->element_count;

  switch (buffer->meta->type) {

  case INST_DATA_FLOAT32: {
    float *data = (float *)buffer->data;
    for (size_t i = 0; i < count; i++) {
      data[i] += (float)offset;
    }
    break;
  }

  case INST_DATA_FLOAT64: {
    double *data = (double *)buffer->data;
    for (size_t i = 0; i < count; i++) {
      data[i] += offset;
    }
    break;
  }

  default:
    inst_ipc_mutex_unlock(buffer->mutex);
    data_buffer_unref(buffer);
    return FALSE;
  }

  inst_ipc_mutex_unlock(buffer->mutex);
  data_buffer_unref(buffer);

  return TRUE;
}
gboolean data_manager_multiply_gain(const gchar *id, double gain) {
  if (!id) {
    return FALSE;
  }

  DataBuffer *buffer = data_manager_get_buffer(id);
  if (!buffer) {
    return FALSE;
  }

  inst_ipc_mutex_lock(buffer->mutex);

  size_t count = buffer->meta->element_count;

  switch (buffer->meta->type) {

  case INST_DATA_FLOAT32: {
    float *data = (float *)buffer->data;
    for (size_t i = 0; i < count; i++) {
      data[i] *= (float)gain;
    }
    break;
  }

  case INST_DATA_FLOAT64: {
    double *data = (double *)buffer->data;
    for (size_t i = 0; i < count; i++) {
      data[i] *= gain;
    }
    break;
  }

  default:
    inst_ipc_mutex_unlock(buffer->mutex);
    data_buffer_unref(buffer);
    return FALSE;
  }

  inst_ipc_mutex_unlock(buffer->mutex);
  data_buffer_unref(buffer);

  return TRUE;
}
gchar **data_manager_list_buffers(size_t *count) {
  gchar **list = NULL;
  registry_list(&list, count);
  return list;
}

size_t data_manager_total_memory_usage(void) { return registry_total_memory(); }

gboolean data_manager_get_metadata(const gchar *id, SharedMetadata *out_meta) {
  init();

  if (!id || !out_meta) {
    return FALSE;
  }

  g_mutex_lock(&lock);
  DataBuffer *buffer = lookup_buffer_no_lock(id);
  g_mutex_unlock(&lock);

  if (buffer) {
    inst_ipc_mutex_lock(buffer->mutex);

    memcpy(out_meta, buffer->meta, sizeof(SharedMetadata));

    inst_ipc_mutex_unlock(buffer->mutex);
    return TRUE;
  }

  /* 2. Fallback to global registry */
  InstShmHandle sd = {0};
  InstShmHandle sm = {0};

  if (!registry_find(id, &sd, &sm)) {
    return FALSE;
  }

  /* Map metadata SHM */
  SharedMetadata *meta = inst_shm_map(&sm);

  if (!meta) {
    return FALSE;
  }

  /* Copy safely */
  memcpy(out_meta, meta, sizeof(SharedMetadata));

  /* cleanup */
  inst_shm_unmap(&sm, meta);

  return TRUE;
}
