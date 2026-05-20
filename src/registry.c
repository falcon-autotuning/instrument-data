#include "registry.h"
#include "instrument-data.h"
#include "shm.h"
#include <glib.h>
#include <string.h>

#define REG_NAME "/inst_registry"
#define MAX_BUF 1024
#define MAGIC 0x494E5354

typedef struct {
  guint32 magic;
  guint32 count;

  struct {
    gchar id[INST_MAX_STRING_LEN];
    gchar data[INST_MAX_STRING_LEN];
    gchar meta[INST_MAX_STRING_LEN];
    size_t data_size;
    guint8 active;
  } entries[MAX_BUF];

} Registry;

static Registry *reg = NULL;
static gpointer mutex = NULL;

static void init(void) {
  static gsize once = 0;
  if (g_once_init_enter(&once)) {

    InstShmHandle shm;
    reg = inst_shm_open_or_create(&shm, REG_NAME, sizeof(Registry));
    mutex = inst_ipc_mutex_create("inst_registry");

    inst_ipc_mutex_lock(mutex);

    if (reg->magic != MAGIC) {
      memset(reg, 0, sizeof(Registry));
      reg->magic = MAGIC;
    }

    inst_ipc_mutex_unlock(mutex);
    g_once_init_leave(&once, 1);
  }
}

void registry_add(DataBuffer *buf) {
  init();
  inst_ipc_mutex_lock(mutex);

  for (int i = 0; i < MAX_BUF; i++) {
    if (!reg->entries[i].active) {
      g_strlcpy(reg->entries[i].id, buf->id, INST_MAX_STRING_LEN);
      g_strlcpy(reg->entries[i].data, buf->shm_data.name, INST_MAX_STRING_LEN);
      g_strlcpy(reg->entries[i].meta, buf->shm_meta.name, INST_MAX_STRING_LEN);
      reg->entries[i].data_size = buf->meta->byte_size;
      reg->entries[i].active = 1;
      break;
    }
  }

  inst_ipc_mutex_unlock(mutex);
}

gboolean registry_find(const gchar *id, InstShmHandle *data,
                       InstShmHandle *meta) {
  init();
  inst_ipc_mutex_lock(mutex);

  for (int i = 0; i < MAX_BUF; i++) {
    if (reg->entries[i].active && g_strcmp0(reg->entries[i].id, id) == 0) {

      data->name = g_strdup(reg->entries[i].data);
      data->size = reg->entries[i].data_size;

      meta->name = g_strdup(reg->entries[i].meta);
      meta->size = sizeof(SharedMetadata);

      inst_ipc_mutex_unlock(mutex);
      return TRUE;
    }
  }

  inst_ipc_mutex_unlock(mutex);
  return FALSE;
}

void registry_remove(const gchar *id) {
  init();
  inst_ipc_mutex_lock(mutex);

  for (int i = 0; i < MAX_BUF; i++) {
    if (reg->entries[i].active && g_strcmp0(reg->entries[i].id, id) == 0) {
      reg->entries[i].active = 0;
    }
  }

  inst_ipc_mutex_unlock(mutex);
}
gboolean registry_list(gchar ***out, size_t *count) {
  init();
  inst_ipc_mutex_lock(mutex);

  GPtrArray *arr = g_ptr_array_new();

  for (int i = 0; i < MAX_BUF; i++) {
    if (reg->entries[i].active) {
      g_ptr_array_add(arr, g_strdup(reg->entries[i].id));
    }
  }

  *count = arr->len;
  *out = (gchar **)g_ptr_array_free(arr, FALSE);

  inst_ipc_mutex_unlock(mutex);
  return TRUE;
}

size_t registry_total_memory(void) {
  init();
  inst_ipc_mutex_lock(mutex);

  size_t total = 0;

  for (int i = 0; i < MAX_BUF; i++) {
    if (!reg->entries[i].active) {
      continue;
    }

    InstShmHandle m = {0};
    m.name = reg->entries[i].meta;

    SharedMetadata *meta =
        inst_shm_open_or_create(&m, m.name, sizeof(SharedMetadata));

    total += meta->byte_size;
  }

  inst_ipc_mutex_unlock(mutex);
  return total;
}
