#pragma once

#include "instrument-data.h"
#include "shm.h"
#include <stdatomic.h>

struct DataBuffer {
  char *id;

  void *data; /* pointer to the data */
  SharedMetadata *meta;

  InstShmHandle shm_data;
  InstShmHandle shm_meta;

  void *mutex; /* pointer to the interprocess lock */

  atomic_int ref_count; /* local process ref */
};
DataBuffer *data_buffer_ref(DataBuffer *buffer);
void data_buffer_unref(DataBuffer *buffer);
