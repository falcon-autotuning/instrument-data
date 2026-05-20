#include "instrument-data.h"
#include "test_config.h"
#include <glib.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif

typedef struct {
  const char *id;
  int ops;
} WorkerArgs;

static gpointer worker_thread(gpointer data) {
  WorkerArgs *args = (WorkerArgs *)data;

  for (int i = 0; i < args->ops; i++) {

    gchar *argv[] = {(gchar *)TEST_BINARY_PATH, "--child", "write",
                     (gchar *)args->id, NULL};

    gint status = 0;

    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
                               NULL, NULL, NULL, &status, NULL);

    g_assert_true(ok);

#ifndef _WIN32
    g_assert_true(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#else
    g_assert_true(status == 0);
#endif
  }

  return NULL;
}

int main(void) {

  const int NUM_THREADS = 8;
  const int OPS = 50;
  const size_t N = 100;

  double *data = g_new0(double, N);

  gchar *id =
      data_manager_create_buffer("stress", "cmd", INST_DATA_FLOAT64, N, data);

  g_free(data);

  GThread *threads[NUM_THREADS];
  WorkerArgs args = {.id = id, .ops = OPS};

  for (int i = 0; i < NUM_THREADS; i++) {
    threads[i] = g_thread_new("worker", worker_thread, &args);
  }

  for (int i = 0; i < NUM_THREADS; i++) {
    g_thread_join(threads[i]);
  }

  DataBuffer *buffer = data_manager_get_buffer(id);
  g_assert_nonnull(buffer);

  double *d = data_buffer_data(buffer);

  double expected = NUM_THREADS * OPS * 5.0;

  for (size_t i = 0; i < N; i++) {
    g_assert_cmpfloat(d[i], ==, expected);
  }

  g_print("✅ Deterministic stress passed: %.2f\n", d[0]);

  data_manager_release_buffer(id);
  g_free(id);

  return 0;
}
