#include "instrument-data.h"
#include "test_common.h"
#include "test_config.h"

#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif

typedef struct {
  const char *id;
  int ops;
} WorkerArgs;

/* ============================================================
 * Worker thread
 * ============================================================ */

static int worker_thread(void *data) {
  WorkerArgs *args = (WorkerArgs *)data;

  char cmd[512];

  for (int i = 0; i < args->ops; i++) {

    /* build command line */
    snprintf(cmd, sizeof(cmd), "%s --child write %s", TEST_BINARY_PATH,
             args->id);

    int status = system(cmd);

    assert_true(status != -1);

#ifndef _WIN32
    assert_true(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#else
    assert_true(status == 0);
#endif
  }

  return 0;
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(void) {

  const int NUM_THREADS = 8;
  const int OPS = 50;
  const size_t N = 100;

  double *data = calloc(N, sizeof(double));
  assert_non_null(data);

  char *id =
      data_manager_create_buffer("stress", "cmd", INST_DATA_FLOAT64, N, data);

  free(data);

  assert_non_null(id);

  thrd_t threads[NUM_THREADS];
  WorkerArgs args = {
      .id = id,
      .ops = OPS,
  };

  /* create threads */
  for (int i = 0; i < NUM_THREADS; i++) {
    int rc = thrd_create(&threads[i], worker_thread, &args);
    assert_int_equal(rc, thrd_success);
  }

  /* join threads */
  for (int i = 0; i < NUM_THREADS; i++) {
    int rc;
    thrd_join(threads[i], &rc);
  }

  /* verify results */
  DataBuffer *buffer = data_manager_get_buffer(id);
  assert_non_null(buffer);

  double *d = data_buffer_data(buffer);

  double expected = NUM_THREADS * OPS * 5.0;

  for (size_t i = 0; i < N; i++) {
    assert_float_equal(d[i], expected, TEST_EPSILON);
  }

  printf("✅ Deterministic stress passed: %.2f\n", d[0]);

  data_manager_release_buffer(id);
  free(id);

  return 0;
}
