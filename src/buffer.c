#include "buffer.h"
#include "shm.h"
#include <glib.h>

DataBuffer *data_buffer_ref(DataBuffer *buffer) {
  g_atomic_int_inc(&buffer->ref_count);
  return buffer;
}

void data_buffer_unref(DataBuffer *buffer) {
  if (!buffer) {
    return;
  }

  if (g_atomic_int_dec_and_test(&buffer->ref_count)) {

    if (buffer->data) {
      inst_shm_unmap(&buffer->shm_data, buffer->data);
      buffer->data = NULL;
    }

    if (buffer->meta) {
      inst_shm_unmap(&buffer->shm_meta, buffer->meta);
      buffer->meta = NULL;
    }

    if (buffer->mutex) {
      inst_ipc_mutex_destroy(buffer->mutex);
      buffer->mutex = NULL;
    }

    g_free(buffer->id);
    g_free(buffer);
  }
}

void *data_buffer_data(DataBuffer *buffer) { return buffer->data; }

size_t data_buffer_element_count(const DataBuffer *buffer) {
  return buffer->meta->element_count;
}

ArrayType data_buffer_type(const DataBuffer *buffer) {
  return buffer->meta->type;
}
