#include "instrument-data.h"
#include "test_config.h"
#include <glib.h>
#include <stddef.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
static gboolean run_child(const char *mode, const char *id) {

  gchar *argv[] = {(gchar *)TEST_BINARY_PATH, "--child", (gchar *)mode,
                   (gchar *)id, NULL};

  gint status = 0;
  gchar *stdout_buf = NULL;
  gchar *stderr_buf = NULL;
  GError *error = NULL;

  gboolean ok = g_spawn_sync(NULL, argv, NULL, 0, NULL, NULL, &stdout_buf,
                             &stderr_buf, &status, &error);

  if (!ok) {
    g_printerr("❌ Spawn failed!\n");

    if (error) {
      g_printerr("GLib error: %s\n", error->message);
      g_error_free(error);
    }

    g_printerr("Attempted to run: %s\n", argv[0]);

    return FALSE;
  }

#ifndef _WIN32
  if (!WIFEXITED(status)) {
    g_printerr("❌ Child did not exit normally\n");
    return FALSE;
  }

  int code = WEXITSTATUS(status);
  if (code != 0) {
    g_printerr("❌ Child exit code: %d\n", code);
  }

  return code == 0;
#else
  return status == 0;
#endif
}
typedef struct {
  GPid pid;
  GIOChannel *in;  // write commands to child
  GIOChannel *out; // read stdout
} ChildProcess;

static ChildProcess spawn_persistent_child(void) {
  ChildProcess proc = {0};

  gchar *argv[] = {(gchar *)TEST_BINARY_PATH, "--child", "server", NULL};

  gint stdin_fd, stdout_fd;

  GError *error = NULL;

  if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
                                NULL, NULL, &proc.pid, &stdin_fd, &stdout_fd,
                                NULL, &error)) {
    g_error("Failed to spawn persistent child: %s", error->message);
  }

  proc.in = g_io_channel_unix_new(stdin_fd);
  proc.out = g_io_channel_unix_new(stdout_fd);

  return proc;
}
static gchar *read_line(GIOChannel *ch) {
  gchar *line = NULL;
  gsize len = 0;
  GError *error = NULL;

  GIOStatus status = g_io_channel_read_line(ch, &line, &len, NULL, &error);

  if (status != G_IO_STATUS_NORMAL) {
    g_error("Failed to read from child");
  }

  g_strchomp(line);
  return line;
}
static void send_command(GIOChannel *ch, const char *cmd) {
  gsize written;

  g_io_channel_write_chars(ch, cmd, -1, &written, NULL);
  g_io_channel_write_chars(ch, "\n", 1, &written, NULL);
  g_io_channel_flush(ch, NULL);
}

/* ============================================================
 * TESTS
 * ============================================================ */

static void test_cross_process_read(void) {
  double data[1] = {42.0};

  gchar *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_FLOAT64, 1, data);

  gboolean ok = run_child("read", id);
  g_assert_true(ok);

  data_manager_release_buffer(id);
  g_free(id);
}

static void test_cross_process_write(void) {
  double data[1] = {10.0};

  gchar *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_FLOAT64, 1, data);

  gboolean ok = run_child("write", id);
  g_assert_true(ok);

  DataBuffer *b = data_manager_get_buffer(id);
  double *d = data_buffer_data(b);

  g_assert_cmpfloat(d[0], ==, 15.0);

  data_manager_release_buffer(id);
  g_free(id);
}

static void test_crash_cleanup(void) {
  gchar *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_UINT8, 10, NULL);

  run_child("crash", id);

  DataBuffer *b = data_manager_get_buffer(id);
  g_assert_nonnull(b);

  SharedMetadata meta;
  data_manager_get_metadata(id, &meta);

  g_assert_cmpint(meta.global_ref_count, >=, 1);

  data_manager_release_buffer(id);
  g_free(id);
}

static void test_large_array_cross_process(void) {

  const size_t N = 100;

  double *data = g_new(double, N);

  for (size_t i = 0; i < N; i++)
    data[i] = (double)i;

  gchar *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_FLOAT64, N, data);

  g_free(data);

  gboolean ok = run_child("write", id);
  g_assert_true(ok);

  DataBuffer *b = data_manager_get_buffer(id);
  double *d = data_buffer_data(b);

  for (size_t i = 0; i < N; i++) {
    g_assert_cmpfloat(d[i], ==, i + 5.0);
  }

  data_manager_release_buffer(id);
  g_free(id);
}

static void test_cross_process_zero_copy(void) {
  double *ptr = NULL;

  gchar *id = data_manager_create_buffer_zero_copy(
      "inst", "zero", INST_DATA_FLOAT64, 1, (void **)&ptr);

  ptr[0] = 123.0;

  gboolean ok = run_child("zero_copy", id);
  g_assert_true(ok);

  data_manager_release_buffer(id);
  g_free(id);
}

typedef struct {
  const char *id;
} WriteArgs;

static gpointer write_worker(gpointer data) {
  WriteArgs *args = (WriteArgs *)data;

  gboolean ok = run_child("write", args->id);
  g_assert_true(ok);

  return NULL;
}

static void test_concurrent_writes(void) {

  const int N = 10;
  const int PROCS = 10; /* number of concurrent writers */

  double *ptr = NULL;

  gchar *id = data_manager_create_buffer_zero_copy(
      "inst", "stress", INST_DATA_FLOAT64, N, (void **)&ptr);

  for (int i = 0; i < N; i++) {
    ptr[i] = 0.0;
  }

  /* launch threads → concurrent process spawning */
  GThread *threads[PROCS];
  WriteArgs args = {.id = id};

  for (int i = 0; i < PROCS; i++) {
    threads[i] = g_thread_new("write_worker", write_worker, &args);
  }

  /* wait for all to finish */
  for (int i = 0; i < PROCS; i++) {
    g_thread_join(threads[i]);
  }

  /* validate result */
  DataBuffer *buffer = data_manager_get_buffer(id);
  g_assert_nonnull(buffer);

  double *data = data_buffer_data(buffer);

  for (int i = 0; i < N; i++) {
    g_assert_cmpfloat(data[i], ==, PROCS * 5.0);
  }

  data_manager_release_buffer(id);
  g_free(id);
}

typedef struct {
  const char *id;
} RaceArgs;

static gpointer race_worker(gpointer data) {
  RaceArgs *args = (RaceArgs *)data;

  /* spawn child (attach + release) */
  run_child("attach", args->id);

  /* parent also does same thing */
  DataBuffer *b = data_manager_get_buffer(args->id);
  if (b) {
    data_manager_release_buffer(args->id);
  }

  return NULL;
}

static void test_race_open_release(void) {

  gchar *id =
      data_manager_create_buffer("inst", "race", INST_DATA_UINT8, 100, NULL);

  const int NUM_THREADS = 10;

  GThread *threads[NUM_THREADS];
  RaceArgs args = {.id = id};

  for (int i = 0; i < NUM_THREADS; i++) {
    threads[i] = g_thread_new("race_worker", race_worker, &args);
  }

  for (int i = 0; i < NUM_THREADS; i++) {
    g_thread_join(threads[i]);
  }

  g_assert_true(TRUE);

  data_manager_release_buffer(id);
  g_free(id);
}
static void test_write_then_crash(void) {

  double data[1] = {10.0};

  gchar *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_FLOAT64, 1, data);

  /* child writes + crashes */
  run_child("write_kill", id);

  /* attach again (this should fix ownership + recover state) */
  DataBuffer *b = data_manager_get_buffer(id);
  g_assert_nonnull(b);

  double *d = data_buffer_data(b);

  /* the write should still have happened */
  g_assert_cmpfloat(d[0], ==, 15.0);

  /* metadata should still be sane */
  SharedMetadata meta;
  data_manager_get_metadata(id, &meta);

  g_assert_cmpint(meta.global_ref_count, >=, 1);

  data_manager_release_buffer(id);
  g_free(id);
}
static void test_persistent_child_lifecycle(void) {

  ChildProcess proc = spawn_persistent_child();

  gchar *id = read_line(proc.out);
  g_assert_nonnull(id);

  DataBuffer *b = data_manager_get_buffer(id);
  g_assert_nonnull(b);

  double *d = data_buffer_data(b);
  g_assert_cmpfloat(d[0], ==, 99.0);

  send_command(proc.in, "quit");

#ifndef _WIN32
  int status = 0;
  waitpid(proc.pid, &status, 0);
  g_assert_true(WIFEXITED(status));
  g_assert_cmpint(WEXITSTATUS(status), ==, 0);
#else
  /* best-effort: allow process to exit */
  g_spawn_close_pid(proc.pid);
#endif

  DataBuffer *b2 = data_manager_get_buffer(id);
  g_assert_nonnull(b2);

  double *d2 = data_buffer_data(b2);
  g_assert_cmpfloat(d2[0], ==, 99.0);

  data_manager_release_buffer(id);
  g_free(id);

  g_io_channel_unref(proc.in);
  g_io_channel_unref(proc.out);
}

/* ============================================================
 * REGISTER
 * ============================================================ */

void test_multiprocess_register(void) {
  g_test_add_func("/multiprocess/read", test_cross_process_read);
  g_test_add_func("/multiprocess/write", test_cross_process_write);
  g_test_add_func("/multiprocess/crash", test_crash_cleanup);
  g_test_add_func("/multiprocess/large_array", test_large_array_cross_process);
  g_test_add_func("/multiprocess/zero_copy", test_cross_process_zero_copy);
  g_test_add_func("/multiprocess/concurrent_writes", test_concurrent_writes);
  g_test_add_func("/multiprocess/race_open_release", test_race_open_release);
  g_test_add_func("/multiprocess/write_then_crash", test_write_then_crash);
  g_test_add_func("/multiprocess/persistent_child",
                  test_persistent_child_lifecycle);
}
