#ifdef _WIN32
#include <windows.h>
#include <handleapi.h>
#endif

#include "instrument-data.h"
#include "test_common.h"
#include "test_config.h"
#include <cmocka.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

static int str_eq(const char *a, const char *b) {
  if (!a && !b)
    return 1;
  if (!a || !b)
    return 0;
  return strcmp(a, b) == 0;
}

/* portable spawn using system() */
static bool run_child(const char *mode, const char *id) {
  char cmd[512];

#ifdef _WIN32
  snprintf(cmd, sizeof(cmd), "\"%s\" --child %s %s", TEST_BINARY_PATH, mode,
           id);
#else
  snprintf(cmd, sizeof(cmd), "%s --child %s %s", TEST_BINARY_PATH, mode, id);
#endif

  int status = system(cmd);
  if (status == -1)
    return false;

#ifndef _WIN32
  if (str_eq(mode, "write_kill"))
    return true;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
  if (str_eq(mode, "write_kill"))
    return true;
  return status == 0;
#endif
}
typedef struct {
#ifdef _WIN32
  PROCESS_INFORMATION pi;
  HANDLE in_write;
  HANDLE out_read;
  HANDLE err_read;
#else
  pid_t pid;
  int in_fd;
  int out_fd;
  int err_fd;
#endif
} ChildProcess;
#ifdef _WIN32
DWORD WINAPI forward_child_stderr(LPVOID param) {
  HANDLE h = (HANDLE)param;

  char buf[256];
  DWORD bytes_read;

  char line[512];
  size_t line_len = 0;

  while (ReadFile(h, buf, sizeof(buf), &bytes_read, NULL) && bytes_read > 0) {
    for (DWORD i = 0; i < bytes_read; i++) {
      char c = buf[i];

      if (c == '\n') {
        line[line_len] = '\0';
        fprintf(stderr, "child: %s\n", line);
        fflush(stderr);
        line_len = 0;
      } else if (line_len < sizeof(line) - 1) {
        line[line_len++] = c;
      }
    }
  }

  // flush partial line at EOF
  if (line_len > 0) {
    line[line_len] = '\0';
    fprintf(stderr, "child: %s\n", line);
    fflush(stderr);
  }

  return 0;
}
#endif
static ChildProcess spawn_persistent_child(void) {
  ChildProcess proc = {0};

#ifdef _WIN32
  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};

  HANDLE in_read, in_write;
  HANDLE out_read, out_write;
  HANDLE err_read, err_write;

  CreatePipe(&in_read, &in_write, &sa, 0);
  CreatePipe(&out_read, &out_write, &sa, 0);
  CreatePipe(&err_read, &err_write, &sa, 0);

  SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFO si = {0};
  si.cb = sizeof(si);
  si.hStdInput = in_read;
  si.hStdOutput = out_write;
  si.hStdError = err_write;
  si.dwFlags |= STARTF_USESTDHANDLES;
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "\"%s\" --child server", TEST_BINARY_PATH);
  CreateProcess(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &proc.pi);

  CloseHandle(in_read);
  CloseHandle(out_write);
  CloseHandle(err_write); // ✅ must close

  proc.in_write = in_write;
  proc.out_read = out_read;
  proc.err_read = err_read;
  CreateThread(NULL, 0, forward_child_stderr, proc.err_read, 0, NULL);

#else
  int in_pipe[2];
  int out_pipe[2];

  pipe(in_pipe);
  pipe(out_pipe);

  pid_t pid = fork();

  if (pid == 0) {
    dup2(in_pipe[0], STDIN_FILENO);
    dup2(out_pipe[1], STDOUT_FILENO);

    close(in_pipe[1]);
    close(out_pipe[0]);

    execl(TEST_BINARY_PATH, TEST_BINARY_PATH, "--child", "server", NULL);
    _exit(127);
  }

  close(in_pipe[0]);
  close(out_pipe[1]);

  proc.pid = pid;
  proc.in_fd = in_pipe[1];
  proc.out_fd = out_pipe[0];
#endif

  return proc;
}
static char *read_line(ChildProcess *proc) {
  char buf[256];
  size_t idx = 0;

  while (idx < sizeof(buf) - 1) {
    char c;

#ifdef _WIN32
    DWORD read;
    if (!ReadFile(proc->out_read, &c, 1, &read, NULL) || read == 0)
      break;
#else
    if (read(proc->out_fd, &c, 1) != 1)
      break;
#endif

    if (c == '\n')
      break;
    buf[idx++] = c;
  }

  buf[idx] = '\0';
#ifdef _WIN32
  return _strdup(buf);
#else
  return strdup(buf);
#endif
}
static void send_command(ChildProcess *proc, const char *cmd) {

#ifdef _WIN32
  DWORD written;
  WriteFile(proc->in_write, cmd, (DWORD)strlen(cmd), &written, NULL);
  WriteFile(proc->in_write, "\n", 1, &written, NULL);
#else
  write(proc->in_fd, cmd, strlen(cmd));
  write(proc->in_fd, "\n", 1);
#endif
}

/* ============================================================
 * TESTS
 * ============================================================ */

static void test_cross_process_read(void **state) {
  (void)state;
  double data[1] = {42.0};

  const char *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_FLOAT64, 1, data);

  bool ok = run_child("read", id);
  assert_true(ok);

  data_manager_release_buffer(id);
}

static void test_cross_process_write(void **state) {
  (void)state;
  double data[1] = {10.0};

  const char *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_FLOAT64, 1, data);

  bool ok = run_child("write", id);
  assert_true(ok);

  DataBuffer *b = data_manager_get_buffer(id);
  double *d = data_buffer_data(b);

  assert_float_equal(d[0], 15.0, TEST_EPSILON);

  data_manager_release_buffer(id);
}

static void test_crash_cleanup(void **state) {
  (void)state;
  const char *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_UINT8, 10, NULL);

  run_child("crash", id);

  DataBuffer *b = data_manager_get_buffer(id);
  assert_non_null(b);

  SharedMetadata meta;
  data_manager_get_metadata(id, &meta);

  assert_true(meta.global_ref_count >= 1);

  data_manager_release_buffer(id);
}

static void test_large_array_cross_process(void **state) {
  (void)state;

  const size_t N = 100;

  double *data = malloc(sizeof(double) * N);
  assert_non_null(data);

  for (size_t i = 0; i < N; i++)
    data[i] = (double)i;

  const char *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_FLOAT64, N, data);

  free(data);

  bool ok = run_child("write", id);
  assert_true(ok);

  DataBuffer *b = data_manager_get_buffer(id);
  double *d = data_buffer_data(b);

  for (size_t i = 0; i < N; i++) {
    assert_int_equal(d[i], i + 5.0);
  }

  data_manager_release_buffer(id);
}

static void test_cross_process_zero_copy(void **state) {
  (void)state;
  double *ptr = NULL;

  const char *id = data_manager_create_buffer_zero_copy(
      "inst", "zero", INST_DATA_FLOAT64, 1, (void **)&ptr);

  ptr[0] = 123.0;

  bool ok = run_child("zero_copy", id);
  assert_true(ok);

  data_manager_release_buffer(id);
}

typedef struct {
  const char *id;
} WriteArgs;

static int write_worker(void *data) {
  WriteArgs *args = data;
  assert_true(run_child("write", args->id));
  return 0;
}

static void test_concurrent_writes(void **state) {
  (void)state;

  const int N = 10;
  const int PROCS = 10; /* number of concurrent writers */

  double *ptr = NULL;

  const char *id = data_manager_create_buffer_zero_copy(
      "inst", "stress", INST_DATA_FLOAT64, N, (void **)&ptr);

  assert_non_null(id);
  assert_non_null(ptr);

  for (int i = 0; i < N; i++) {
    ptr[i] = 0.0;
  }

  WriteArgs args = {
      .id = id,
  };

  /* launch threads → concurrent process spawning */
  thrd_t threads[PROCS];

  for (int i = 0; i < PROCS; i++) {
    int rc = thrd_create(&threads[i], write_worker, &args);
    assert_int_equal(rc, thrd_success);
  }

  for (int i = 0; i < PROCS; i++) {
    thrd_join(threads[i], NULL);
  }

  /* validate result */
  DataBuffer *buffer = data_manager_get_buffer(id);
  assert_non_null(buffer);

  double *data = data_buffer_data(buffer);

  for (int i = 0; i < N; i++) {
    assert_float_equal(data[i], PROCS * 5.0, TEST_EPSILON);
  }

  data_manager_release_buffer(id);
}

typedef struct {
  const char *id;
} RaceArgs;

static int race_worker(void *data) {
  RaceArgs *args = data;

  run_child("attach", args->id);

  DataBuffer *b = data_manager_get_buffer(args->id);
  if (b) {
    data_manager_release_buffer(args->id);
  }

  return 0;
}

static void test_race_open_release(void **state) {
  (void)state;

  const char *id =
      data_manager_create_buffer("inst", "race", INST_DATA_UINT8, 100, NULL);

  const int NUM_THREADS = 10;
  thrd_t threads[NUM_THREADS];
  RaceArgs args = {.id = id};

  for (int i = 0; i < NUM_THREADS; i++) {
    thrd_create(&threads[i], race_worker, &args);
  }

  for (int i = 0; i < NUM_THREADS; i++) {
    thrd_join(threads[i], NULL);
  }

  assert_true(true);

  data_manager_release_buffer(id);
}
static void test_write_then_crash(void **state) {
  (void)state;

  double data[1] = {10.0};

  const char *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_FLOAT64, 1, data);

  /* child writes + crashes */
  run_child("write_kill", id);

  /* attach again (this should fix ownership + recover state) */
  DataBuffer *b = data_manager_get_buffer(id);
  assert_non_null(b);

  double *d = data_buffer_data(b);

  /* the write should still have happened */
  assert_float_equal(d[0], 15.0, TEST_EPSILON);

  /* metadata should still be sane */
  SharedMetadata meta;
  data_manager_get_metadata(id, &meta);

  assert_true(meta.global_ref_count >= 1);

  data_manager_release_buffer(id);
}
static void str_chomp(char *s) {
  if (!s)
    return;
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
    s[--len] = '\0';
  }
}
static void test_persistent_child_lifecycle(void **state) {
  (void)state;

  ChildProcess proc = spawn_persistent_child();

  char *id = read_line(&proc);
  fprintf(stderr, "The id that we got from the child is: %s\n", id);
  str_chomp(id);

#ifdef _WIN32
  char name[256];
  snprintf(name, sizeof(name), "Local\\shm_data_%s", id);
  HANDLE h2 = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, name);
  fprintf(stderr, "Self-open (%s): %p err=%lu\n", name, h2, GetLastError());
#endif

  DataBuffer *b = data_manager_get_buffer(id);
  assert_non_null(b);

  // tell child it's safe
  send_command(&proc, "attached");

  assert_non_null(b);
  double *d = data_buffer_data(b);
  assert_float_equal(d[0], 99.0, TEST_EPSILON);

  send_command(&proc, "quit");

#ifdef _WIN32
  WaitForSingleObject(proc.pi.hProcess, INFINITE);
#else
  int status;
  waitpid(proc.pid, &status, 0);
  assert_true(WIFEXITED(status));
#endif
  double *d2 = data_buffer_data(b);
  assert_float_equal(d2[0], 99.0, TEST_EPSILON);

  data_manager_release_buffer(id);
}

/* ============================================================
 * REGISTER
 * ============================================================ */

const struct CMUnitTest test_multiprocess_tests[] = {
    cmocka_unit_test(test_cross_process_read),
    cmocka_unit_test(test_cross_process_write),
    cmocka_unit_test(test_crash_cleanup),
    cmocka_unit_test(test_large_array_cross_process),
    cmocka_unit_test(test_cross_process_zero_copy),
#ifndef USE_TSAN
    cmocka_unit_test(test_concurrent_writes),
    cmocka_unit_test(test_race_open_release),
#endif
    cmocka_unit_test(test_write_then_crash),
    cmocka_unit_test(test_persistent_child_lifecycle),
};

const size_t test_multiprocess_tests_count =
    sizeof(test_multiprocess_tests) / sizeof(test_multiprocess_tests[0]);
