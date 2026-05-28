#include "instrument-data.h"
#include "registry.h"

#include <cmocka.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

/* ============================================================
 * Helpers (GLib replacements)
 * ============================================================ */

static int str_eq(const char *a, const char *b) {
  if (!a && !b)
    return 1;
  if (!a || !b)
    return 0;
  return strcmp(a, b) == 0;
}

static void str_chomp(char *s) {
  if (!s)
    return;
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
    s[--len] = '\0';
  }
}

/* ============================================================
 * CHILD TEST IMPLEMENTATIONS
 * ============================================================ */

static int child_read(int argc, char **argv) {
  (void)argc;

  const char *id = argv[3];

  DataBuffer *buffer = data_manager_get_buffer(id);
  if (!buffer)
    return 2;

  double *d = data_buffer_data(buffer);
  printf("child read value = %f\n", d[0]);

  return (d[0] == 42.0) ? 0 : 3;
}

static int child_server(int argc, char **argv) {
  (void)argc;
  (void)argv;

  double data[1] = {99.0};

  char *id = data_manager_create_buffer("inst", "persistent", INST_DATA_FLOAT64,
                                        1, data);

  if (!id)
    return 2;

  printf("%s\n", id);
  fflush(stdout);

  char buf[32];

  while (fgets(buf, sizeof(buf), stdin)) {
    str_chomp(buf);

    if (str_eq(buf, "quit"))
      break;
  }

  data_manager_release_buffer(id);
  free(id);

  return 0;
}

static int child_write(int argc, char **argv) {
  (void)argc;

  const char *id = argv[3];
  DataBuffer *buffer = data_manager_get_buffer(id);
  if (!buffer)
    return 2;

  bool ok = data_manager_add_offset(id, 5.0);
  data_manager_release_buffer(id);
  return !ok;
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
  if (!buffer)
    return 2;

  double *d = data_buffer_data(buffer);
  return (d[0] == 123.0) ? 0 : 3;
}

static int child_write_and_kill(int argc, char **argv) {
  (void)argc;
  const char *id = argv[3];

  DataBuffer *buffer = data_manager_get_buffer(id);
  if (!buffer)
    return 2;

  data_manager_add_offset(id, 5.0);

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
    {"server", child_server},
};

/* ============================================================
 * CLEANUP
 * ============================================================ */

static void cleanup_ipc(void) {
#ifndef _WIN32
  system("rm -f /dev/shm/sem.mtx_* 2>/dev/null");
  system("rm -f /dev/shm/inst_* 2>/dev/null");
#endif
}
static void handle_signal(int sig) {
  (void)sig;
  cleanup_ipc();

#ifndef _WIN32
  _exit(1); // safer than exit() in signal context
#else
  ExitProcess(1);
#endif
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(int argc, char **argv) {

#ifndef _WIN32
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);
#endif

  if (argc > 2 && str_eq(argv[1], "--child")) {
    const char *name = argv[2];

    for (size_t i = 0; i < sizeof(child_table) / sizeof(child_table[0]); i++) {
      if (str_eq(name, child_table[i].name)) {
        int rc = child_table[i].fn(argc, argv);
        registry_shutdown();
        return rc;
      }
    }
    registry_shutdown();
    return 1;
  }

  cleanup_ipc();

  /* import test arrays */
  extern const struct CMUnitTest test_basic_tests[];
  extern const size_t test_basic_tests_count;

  extern const struct CMUnitTest test_registry_tests[];
  extern const size_t test_registry_tests_count;

  extern const struct CMUnitTest test_multiprocess_tests[];
  extern const size_t test_multiprocess_tests_count;

  size_t total = test_basic_tests_count + test_registry_tests_count +
                 test_multiprocess_tests_count;

  struct CMUnitTest *all = malloc(sizeof(struct CMUnitTest) * total);

  size_t idx = 0;

  memcpy(&all[idx], test_basic_tests,
         test_basic_tests_count * sizeof(struct CMUnitTest));
  idx += test_basic_tests_count;

  memcpy(&all[idx], test_registry_tests,
         test_registry_tests_count * sizeof(struct CMUnitTest));
  idx += test_registry_tests_count;

  memcpy(&all[idx], test_multiprocess_tests,
         test_multiprocess_tests_count * sizeof(struct CMUnitTest));

  int rc = _cmocka_run_group_tests("all_tests", all, total, NULL, NULL);

  registry_shutdown();

  free(all);
  return rc;
}
