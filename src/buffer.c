#include "internal/buffer.h"
#include "internal/shm.h"
#include "internal/util.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

DataBuffer *data_buffer_ref(DataBuffer *buffer) {
  atomic_fetch_add(&buffer->ref_count, 1);
  return buffer;
}

void data_buffer_unref(DataBuffer *buffer) {
  if (!buffer) {
    return;
  }

  if (atomic_fetch_sub(&buffer->ref_count, 1) == 1) {

    if (buffer->data) {
      inst_shm_close(&buffer->shm_data, buffer->data);
      buffer->data = NULL;
    }

    if (buffer->meta) {
      inst_shm_close(&buffer->shm_meta, buffer->meta);
      buffer->meta = NULL;
    }

    if (buffer->mutex) {
      inst_ipc_mutex_destroy(buffer->mutex);
      buffer->mutex = NULL;
    }

    free(buffer->id);
    free(buffer);
  }
}

void *data_buffer_data(DataBuffer *b) {
  if (!b || !b->data) {
    fprintf(stderr, "🔥 data_buffer_data NULL access\n");
    return NULL;
  }
  return b->data;
}

size_t data_buffer_element_count(const DataBuffer *buffer) {
  return buffer->meta->element_count;
}

ArrayType data_buffer_type(const DataBuffer *buffer) {
  return buffer->meta->type;
}
