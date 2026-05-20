#include "instrument-data.h"
#include <glib.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================
 * CHILD TEST IMPLEMENTATIONS
 * ============================================================ */

static int child_read(int argc, char **argv) {
  (void)argc;

  const char *id = argv[3];

  DataBuffer *buffer = data_manager_get_buffer(id);
  if (!buffer) {
    return 2;
  }

  double *d = data_buffer_data(buffer);
  g_print("child read value = %f\n", d[0]);

  /* default read expectation used by tests (42.0) */
  if (d[0] == 42.0) {
    return 0;
  }

  return 3;
}

static int child_write(int argc, char **argv) {
  (void)argc;

  const char *id = argv[3];
  DataBuffer *buffer = data_manager_get_buffer(id);
  if (!buffer) {
    return 2;
  }

  gboolean ok = data_manager_add_offset(id, 5.0);
  data_manager_release_buffer(id);
  return ok ? 0 : 1;
}

static int child_crash(int argc, char **argv) {
  (void)argc;

  const char *id = argv[3];
  DataBuffer *buffer = data_manager_get_buffer(id);
  (void)buffer;

#ifdef _WIN32
  ExitProcess(0);
#else
  _exit(0);
#endif
}

static int child_attach(int argc, char **argv) {
  (void)argc;

  const char *id = argv[3];

  DataBuffer *buffer = data_manager_get_buffer(id);
  if (buffer) {
    data_manager_release_buffer(id);
    return 0;
  }

  return 1;
}

static int child_zero_copy(int argc, char **argv) {
  (void)argc;

  const char *id = argv[3];

  DataBuffer *buffer = data_manager_get_buffer(id);
  if (!buffer) {
    return 2;
  }

  double *d = data_buffer_data(buffer);

  if (d[0] == 123.0) {
    return 0;
  }

  return 3;
}

static int child_write_and_kill(int argc, char **argv) {
  (void)argc;
  const char *id = argv[3];
  DataBuffer *buffer = data_manager_get_buffer(id);
  if (!buffer) {
    return 2;
  }
  data_manager_add_offset(id, 5.0);

  /* simulate crash immediately after write */
#ifdef _WIN32
  ExitProcess(1);
#else
  _exit(1);
#endif
}

/* ============================================================
 * DISPATCH TABLE
 * ============================================================ */

typedef int (*ChildFn)(int, char **);

typedef struct {
  const char *name;
  ChildFn fn;
} ChildEntry;

static ChildEntry child_table[] = {
    {"read", child_read},           {"write", child_write},
    {"crash", child_crash},         {"attach", child_attach},
    {"zero_copy", child_zero_copy}, {"write_kill", child_write_and_kill},
};

/* ============================================================
 * TEST REGISTRATION
 * ============================================================ */

void test_basic_register(void);
void test_registry_register(void);
void test_multiprocess_register(void);

/* ============================================================
 * CLEANUP
 * ============================================================ */

static void cleanup_ipc(void) {
#ifndef _WIN32
  system("rm -f /dev/shm/sem.mtx_* 2>/dev/null");
  system("rm -f /dev/shm/inst_* 2>/dev/null");
#endif
}

static void handle_sigint(int sig) {
  (void)sig;
  cleanup_ipc();
  _exit(1);
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(int argc, char **argv) {

#ifndef _WIN32
  signal(SIGINT, handle_sigint);
  signal(SIGTERM, handle_sigint);
#endif

  if (argc > 2 && g_strcmp0(argv[1], "--child") == 0) {

    const char *name = argv[2];

    for (size_t i = 0; i < sizeof(child_table) / sizeof(child_table[0]); i++) {
      if (g_strcmp0(name, child_table[i].name) == 0) {
        return child_table[i].fn(argc, argv);
      }
    }

    return 1; /* unknown child */
  }

  cleanup_ipc();

  g_test_init(&argc, &argv, NULL);

  test_basic_register();
  test_registry_register();
  test_multiprocess_register();

  return g_test_run();
}
