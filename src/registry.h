#pragma once

#include "buffer.h"
#include "shm.h"

void registry_add(DataBuffer *buf);
gboolean registry_find(const gchar *id, InstShmHandle *data,
                       InstShmHandle *meta);
void registry_remove(const gchar *id);

gboolean registry_list(gchar ***out, size_t *count);
size_t registry_total_memory(void);
