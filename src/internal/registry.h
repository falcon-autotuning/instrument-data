#pragma once

#include "instrument-data.h"
#include "instrument-data/instrument-data-export.h"
#include "shm.h"
#include <stdbool.h>

void registry_add(DataBuffer *buf);
bool registry_find(const char *id, InstShmHandle *data, InstShmHandle *meta);
void registry_remove(const char *id);

char **registry_list(size_t *count);
size_t registry_total_memory(void);

INSTRUMENT_DATA_EXPORT void registry_shutdown(void);
