#pragma once

#include "instrument-data.h"
#include "shm.h"

struct DataBuffer {
  gchar *id;

  gpointer data;
  SharedMetadata *meta;

  InstShmHandle shm_data;
  InstShmHandle shm_meta;

  gpointer mutex; /* interprocess lock */

  volatile gint ref_count; /* local process ref */
};
DataBuffer *data_buffer_ref(DataBuffer *buffer);
void data_buffer_unref(DataBuffer *buffer);
