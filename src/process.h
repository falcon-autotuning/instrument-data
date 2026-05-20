#pragma once

#include <glib.h>

gboolean inst_process_alive(guint32 pid);
guint32 inst_get_pid(void);
