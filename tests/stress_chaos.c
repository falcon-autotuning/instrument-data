#include "instrument-data.h"
#include "test_config.h"

#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#define JITTER_US 1000 /* up to 1ms */
#define ITERATIONS 10

typedef struct {
  const char *id;
  int ops;
} WorkerArgs;

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

/* simple random range */
static int rand_range(int min, int max) { return min + rand() % (max - min); }

/* sleep in microseconds */
static void sleep_us(int us) {
  struct timespec ts;
  ts.tv_sec = us / 1000000;
  ts.tv_nsec = (us % 1000000) * 1000;
  thrd_sleep(&ts, NULL);
}

/* ============================================================
 * Worker thread
 * ============================================================ */

static int worker_thread(void *data) {
  WorkerArgs *args = (WorkerArgs *)data;

  char cmd[512];

  for (int i = 0; i < args->ops; i++) {

    int r = rand_range(0, 100);

    const char *mode;
    if (r < 70) {
      mode = "write";
    } else if (r < 90) {
      mode = "attach";
    } else {
      mode = "write_kill";
    }

    sleep_us(rand_range(0, JITTER_US));

    snprintf(cmd, sizeof(cmd), "%s --child %s %s", TEST_BINARY_PATH, mode,
             args->id);

    int status = system(cmd);

    assert_true(status != -1);

#ifndef _WIN32
    /* write_kill intentionally exits non-zero */
    if (!str_eq(mode, "write_kill")) {
      assert_true(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
#else
    if (!str_eq(mode, "write_kill")) {
      assert_true(status == 0);
    }
#endif
  }

  return 0;
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(void) {

  srand((unsigned int)time(NULL));

  const int NUM_THREADS = 8;
  const int OPS = 50;
  const size_t N = 100;

  double *data = calloc(N, sizeof(double));
  assert_non_null(data);

  const char *id =
      data_manager_create_buffer("chaos", "cmd", INST_DATA_FLOAT64, N, data);

  free(data);
  assert_non_null(id);

  thrd_t threads[NUM_THREADS];
  WorkerArgs args = {.id = id, .ops = OPS};

  /* spawn threads */
  for (int i = 0; i < NUM_THREADS; i++) {
    int rc = thrd_create(&threads[i], worker_thread, &args);
    assert_int_equal(rc, thrd_success);
  }

  /* join */
  for (int i = 0; i < NUM_THREADS; i++) {
    int rc;
    thrd_join(threads[i], &rc);
  }

  /* validate buffer */
  DataBuffer *buffer = data_manager_get_buffer(id);
  assert_non_null(buffer);

  double *d = data_buffer_data(buffer);

  for (size_t i = 0; i < N; i++) {
    assert_true(d[i] >= 0.0);
    assert_true(d[i] <= (NUM_THREADS * OPS * 5.0));
  }

  printf("✅ Chaos stress completed (value: %.2f)\n", d[0]);

  data_manager_release_buffer(id);

  return 0;
}
