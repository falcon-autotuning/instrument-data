#pragma once

#include "instrument-data-export.h"
#include <stdbool.h>
#include <stdint.h>

bool inst_process_alive(uint32_t pid);
INSTRUMENT_DATA_EXPORT uint32_t inst_get_pid(void);
