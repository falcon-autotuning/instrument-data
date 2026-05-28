#include "registry.h"
#include "buffer.h"
#include "shm.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define REG_NAME "/inst_registry"
#define MAX_BUF 1024
#define MAGIC 0x494E5354

typedef struct {
  uint32_t magic;
  uint32_t count;

  struct {
    char id[INST_MAX_STRING_LEN];
    char data[INST_MAX_STRING_LEN];
    char meta[INST_MAX_STRING_LEN];
    size_t data_size;
    uint8_t active;
  } entries[MAX_BUF];

} Registry;

static Registry *reg = NULL;
static void *mutex = NULL;

/* Init once */
static once_flag init_once = ONCE_FLAG_INIT;

/* ============================================================
 * Helpers
 * ============================================================ */

static int inst_strcmp0(const char *a, const char *b) {
  if (!a && !b)
    return 0;
  if (!a)
    return -1;
  if (!b)
    return 1;
  return strcmp(a, b);
}

static void init_impl(void) {

  InstShmHandle shm;
  reg = inst_shm_open_or_create(&shm, REG_NAME, sizeof(Registry));
  mutex = inst_ipc_mutex_create("inst_registry");

  inst_ipc_mutex_lock(mutex);

  if (reg->magic != MAGIC) {
    memset(reg, 0, sizeof(Registry));
    reg->magic = MAGIC;
  }

  inst_ipc_mutex_unlock(mutex);
}

static void init(void) { call_once(&init_once, init_impl); }

/* ============================================================
 * API
 * ============================================================ */

void registry_add(DataBuffer *buf) {
  init();
  inst_ipc_mutex_lock(mutex);

  for (int i = 0; i < MAX_BUF; i++) {
    if (!reg->entries[i].active) {

      inst_strlcpy(reg->entries[i].id, buf->id, INST_MAX_STRING_LEN);
      inst_strlcpy(reg->entries[i].data, buf->shm_data.name,
                   INST_MAX_STRING_LEN);
      inst_strlcpy(reg->entries[i].meta, buf->shm_meta.name,
                   INST_MAX_STRING_LEN);

      reg->entries[i].data_size = buf->meta->byte_size;
      reg->entries[i].active = 1;
      break;
    }
  }

  inst_ipc_mutex_unlock(mutex);
}

bool registry_find(const char *id, InstShmHandle *data, InstShmHandle *meta) {
  init();
  inst_ipc_mutex_lock(mutex);

  for (int i = 0; i < MAX_BUF; i++) {
    if (reg->entries[i].active && inst_strcmp0(reg->entries[i].id, id) == 0) {

      data->name = inst_strdup(reg->entries[i].data);
      data->size = reg->entries[i].data_size;

      meta->name = inst_strdup(reg->entries[i].meta);
      meta->size = sizeof(SharedMetadata);

      inst_ipc_mutex_unlock(mutex);
      return true;
    }
  }

  inst_ipc_mutex_unlock(mutex);
  return false;
}

void registry_remove(const char *id) {
  init();
  inst_ipc_mutex_lock(mutex);

  for (int i = 0; i < MAX_BUF; i++) {
    if (reg->entries[i].active && inst_strcmp0(reg->entries[i].id, id) == 0) {

      reg->entries[i].active = 0;
    }
  }

  inst_ipc_mutex_unlock(mutex);
}

/* ------------------------------------------------------------ */

char **registry_list(size_t *count) {
  init();
  inst_ipc_mutex_lock(mutex);

  size_t n = 0;

  for (int i = 0; i < MAX_BUF; i++) {
    if (reg->entries[i].active) {
      n++;
    }
  }

  char **list = malloc(sizeof(char *) * n);

  if (!list) {
    inst_ipc_mutex_unlock(mutex);
    *count = 0;
    return NULL;
  }

  size_t idx = 0;
  for (int i = 0; i < MAX_BUF; i++) {
    if (reg->entries[i].active) {
      list[idx++] = inst_strdup(reg->entries[i].id);
    }
  }

  *count = n;

  inst_ipc_mutex_unlock(mutex);
  return list;
}

/* ------------------------------------------------------------ */

size_t registry_total_memory(void) {
  init();
  inst_ipc_mutex_lock(mutex);

  size_t total = 0;

  for (int i = 0; i < MAX_BUF; i++) {
    if (!reg->entries[i].active)
      continue;

    InstShmHandle m = {0};
    m.name = reg->entries[i].meta;

    SharedMetadata *meta =
        inst_shm_open_or_create(&m, m.name, sizeof(SharedMetadata));

    if (meta) {
      total += meta->byte_size;
      inst_shm_unmap(&m, meta);
    }
  }

  inst_ipc_mutex_unlock(mutex);
  return total;
}
