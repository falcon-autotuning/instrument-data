#pragma once

#include <stdbool.h>
#include <stdint.h>

bool inst_process_alive(uint32_t pid);
uint32_t inst_get_pid(void);
