#include "instrument-data.h"
#include "test_config.h"
#include <glib.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#define JITTER_US 1000 /* up to 1ms */
#define ITERATIONS 10  /* will also be repeated via CTest */

typedef struct {
  const char *id;
  int ops;
} WorkerArgs;

static gpointer worker_thread(gpointer data) {
  WorkerArgs *args = (WorkerArgs *)data;

  for (int i = 0; i < args->ops; i++) {

    int r = g_random_int_range(0, 100);

    const char *mode;
    if (r < 70) {
      mode = "write";
    } else if (r < 90) {
      mode = "attach";
    } else {
      mode = "write_kill";
    }

    gchar *argv[] = {(gchar *)TEST_BINARY_PATH, "--child", (gchar *)mode,
                     (gchar *)args->id, NULL};

    gint status = 0;

    g_usleep(g_random_int_range(0, JITTER_US));

    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
                               NULL, NULL, NULL, &status, NULL);

    g_assert_true(ok);

#ifndef _WIN32
    /* write_kill exits non-zero → allow it */
    if (g_strcmp0(mode, "write_kill") != 0) {
      g_assert_true(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
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
      data_manager_create_buffer("chaos", "cmd", INST_DATA_FLOAT64, N, data);

  g_free(data);

  GThread *threads[NUM_THREADS];
  WorkerArgs args = {.id = id, .ops = OPS};

  for (int i = 0; i < NUM_THREADS; i++) {
    threads[i] = g_thread_new("chaos_worker", worker_thread, &args);
  }

  for (int i = 0; i < NUM_THREADS; i++) {
    g_thread_join(threads[i]);
  }

  DataBuffer *buffer = data_manager_get_buffer(id);
  g_assert_nonnull(buffer);

  double *d = data_buffer_data(buffer);

  /* ✅ relaxed validation */
  for (size_t i = 0; i < N; i++) {
    g_assert_true(d[i] >= 0.0);
    g_assert_true(d[i] <= (NUM_THREADS * OPS * 5.0));
  }

  g_print("✅ Chaos stress completed (value: %.2f)\n", d[0]);

  data_manager_release_buffer(id);
  g_free(id);

  return 0;
}
