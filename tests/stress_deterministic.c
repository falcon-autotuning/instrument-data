#include "instrument-data.h"
#include "test_config.h"
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <sched.h>
#include <pthread.h>
#endif

#define NUM_WORKERS 8
#define OPS_PER_WORKER 10000
#define BUFFER_SIZE 128

/* ============================================================
 * Timing helper
 * ============================================================ */

static uint64_t now_ns(void) {
#ifdef _WIN32
  LARGE_INTEGER freq, counter;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&counter);
  return (uint64_t)((1e9 * counter.QuadPart) / freq.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
#endif
}

/* ============================================================
 * Worker implementation
 * ============================================================ */

static int worker_main(const char *id) {

  if (!id || strncmp(id, "buffer_", 7) != 0) {
    fprintf(stderr, "BAD ID RECEIVED: %s\n", id);
    exit(1);
  }
  /* attach once */
  DataBuffer *b = NULL;

  for (int retry = 0; retry < 1000; retry++) {
    b = data_manager_get_buffer(id);
    if (b)
      break;
#ifdef _WIN32
    Sleep(0);
#else
    sched_yield();
#endif
  }

  if (!b) {
    fprintf(stderr, "worker: failed to attach\n");
    return 1;
  }

  SharedMetadata meta;
  if (!data_manager_get_metadata(id, &meta)) {
    fprintf(stderr, "worker: metadata failed\n");
    return 2;
  }

  double *data = data_buffer_data(b);

  char buf[32];

  while (1) {
    if (!fgets(buf, sizeof(buf), stdin))
      break;

    if (strncmp(buf, "work", 4) == 0) {

      for (size_t i = 0; i < meta.element_count; i++) {
        data[i] += 1.0;
      }

      printf("done\n");
      fflush(stdout);
    } else if (strncmp(buf, "quit", 4) == 0) {
      break;
    }
  }

  data_manager_release_buffer(id);
  return 0;
}

/* ============================================================
 * Process abstraction
 * ============================================================ */

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

  char tag[64]; // "worker<uuid>"
} WorkerProc;
static void make_worker_tag(char *out, size_t sz) {
#ifdef _WIN32
  unsigned int r = GetTickCount();
#else
  unsigned int r = (unsigned int)rand();
#endif
  snprintf(out, sz, "worker_%x", r);
}

/* ============================================================
 * Spawn worker
 * ============================================================ */
#ifdef _WIN32
DWORD WINAPI forward_stderr_thread(LPVOID param) {
  WorkerProc *wp = (WorkerProc *)param;

  char buf[256];
  DWORD bytes_read;

  char line[512];
  size_t line_len = 0;

  while (ReadFile(wp->err_read, buf, sizeof(buf), &bytes_read, NULL) &&
         bytes_read > 0) {
    for (DWORD i = 0; i < bytes_read; i++) {
      char c = buf[i];

      if (c == '\n') {
        line[line_len] = '\0';
        fprintf(stderr, "%s: %s\n", wp->tag, line);
        fflush(stderr);
        line_len = 0;
      } else if (line_len < sizeof(line) - 1) {
        line[line_len++] = c;
      }
    }
  }

  if (line_len > 0) {
    line[line_len] = '\0';
    fprintf(stderr, "%s: %s\n", wp->tag, line);
    fflush(stderr);
  }

  return 0;
}
static WorkerProc *spawn_worker(const char *binary_path, const char *id) {
  WorkerProc *wp = calloc(1, sizeof(WorkerProc));

  make_worker_tag(wp->tag, sizeof(wp->tag));

  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};

  HANDLE in_r, in_w;
  HANDLE out_r, out_w;
  HANDLE err_r, err_w;

  CreatePipe(&in_r, &in_w, &sa, 0);
  CreatePipe(&out_r, &out_w, &sa, 0);
  CreatePipe(&err_r, &err_w, &sa, 0);

  SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFO si = {0};
  si.cb = sizeof(si);
  si.hStdInput = in_r;
  si.hStdOutput = out_w;
  si.hStdError = err_w;
  si.dwFlags |= STARTF_USESTDHANDLES;

  char args[512];
  snprintf(args, sizeof(args), "--worker %s", id);

  BOOL ok = CreateProcess(binary_path, args, NULL, NULL, TRUE, 0, NULL, NULL,
                          &si, &wp.pi);

  if (!ok) {
    fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
  }

  CloseHandle(in_r);
  CloseHandle(out_w);
  CloseHandle(err_w);

  wp->in_write = in_w;
  wp->out_read = out_r;
  wp->err_read = err_r;

  // start stderr forward thread
  CreateThread(NULL, 0, forward_stderr_thread, wp, 0, NULL);

  return wp;
}
#else

static void *forward_stderr_thread(void *param) {
  WorkerProc *wp = (WorkerProc *)param;

  char buf[256];
  ssize_t n;

  char line[512];
  size_t line_len = 0;

  while ((n = read(wp->err_fd, buf, sizeof(buf))) > 0) {
    for (ssize_t i = 0; i < n; i++) {
      char c = buf[i];

      if (c == '\n') {
        line[line_len] = '\0';
        fprintf(stderr, "%s: %s\n", wp->tag, line);
        fflush(stderr);
        line_len = 0;
      } else if (line_len < sizeof(line) - 1) {
        line[line_len++] = c;
      }
    }
  }

  if (line_len > 0) {
    line[line_len] = '\0';
    fprintf(stderr, "%s: %s\n", wp->tag, line);
    fflush(stderr);
  }

  return NULL;
}
static WorkerProc *spawn_worker(const char *binary_path, const char *id) {
  WorkerProc *wp = calloc(1, sizeof(WorkerProc));

  make_worker_tag(wp->tag, sizeof(wp->tag));

  int in_pipe[2];
  int out_pipe[2];
  int err_pipe[2];

  pipe(in_pipe);
  pipe(out_pipe);
  pipe(err_pipe);

  pid_t pid = fork();

  if (pid == 0) {
    dup2(in_pipe[0], STDIN_FILENO);
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);

    close(in_pipe[1]);
    close(out_pipe[0]);
    close(err_pipe[0]);

    execl(binary_path, binary_path, "--worker", id, (char *)NULL);

    _exit(127);
  }

  close(in_pipe[0]);
  close(out_pipe[1]);
  close(err_pipe[1]);

  wp->pid = pid;
  wp->in_fd = in_pipe[1];
  wp->out_fd = out_pipe[0];
  wp->err_fd = err_pipe[0];

  // launch forwarding thread
  pthread_t tid;
  pthread_create(&tid, NULL, forward_stderr_thread, wp);
  pthread_detach(tid);

  return wp;
}
#endif

/* ============================================================
 * Send / receive helpers
 * ============================================================ */

static void send_cmd(WorkerProc *wp, const char *cmd) {
#ifdef _WIN32
  DWORD written;
  WriteFile(wp->in_write, cmd, (DWORD)strlen(cmd), &written, NULL);
#else
  write(wp->in_fd, cmd, strlen(cmd));
#endif
}

static void wait_done(WorkerProc *wp) {
  char buf[16];

#ifdef _WIN32
  DWORD read;
  ReadFile(wp->out_read, buf, sizeof(buf), &read, NULL);
#else
  read(wp->out_fd, buf, sizeof(buf));
#endif
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
 * MAIN benchmark
 * ============================================================ */

#define MAX_WORKER_ARG 1024

int main(int argc, char **argv) {
  fprintf(stderr, "argc=%d\n", argc);
  for (int i = 0; i < argc; i++) {
    fprintf(stderr, "argv[%d]=%s\n", i, argv[i]);
  }
  char binary_path[MAX_WORKER_ARG];
  strcpy(binary_path, argv[0]);
  if (argc >= 3 && strcmp(argv[1], "--worker") == 0) {
    if (strlen(argv[2]) >= MAX_WORKER_ARG) {
      fprintf(stderr, "Error: Argument is too long.\n");
      return 1;
    }
    char safe_arg[MAX_WORKER_ARG];
    strcpy(safe_arg, argv[2]);
    str_chomp(safe_arg);
    fprintf(stderr, "The ID for the worker is %s\n", safe_arg);
    return worker_main(safe_arg);
  } else if (argc != 1) {
    fprintf(stderr, "Usage: %s [--worker <id>]\n", argv[0]);
    return 1;
  }

  /* setup */
  double data[BUFFER_SIZE] = {0};

  const char *id = data_manager_create_buffer("bench", "cmd", INST_DATA_FLOAT64,
                                              BUFFER_SIZE, data);
  fprintf(stderr, "spawn id: %s\n", id);
  if (!id) {
    fprintf(stderr, "failed to create buffer\n");
    return 1;
  }

  WorkerProc *workers[NUM_WORKERS];

  for (int i = 0; i < NUM_WORKERS; i++) {
    workers[i] = spawn_worker(binary_path, id);
  }

  /* warmup */
  for (int i = 0; i < 100; i++) {
    for (int j = 0; j < NUM_WORKERS; j++) {
      send_cmd(workers[j], "work\n");
    }
    for (int j = 0; j < NUM_WORKERS; j++) {
      wait_done(workers[j]);
    }
  }

  /* measure */
  uint64_t start = now_ns();

  for (int iter = 0; iter < OPS_PER_WORKER; iter++) {
    for (int j = 0; j < NUM_WORKERS; j++) {
      send_cmd(workers[j], "work\n");
    }
    for (int j = 0; j < NUM_WORKERS; j++) {
      wait_done(workers[j]);
    }
  }

  uint64_t end = now_ns();

  double seconds = (end - start) / 1e9;

  double total_ops = (double)NUM_WORKERS * OPS_PER_WORKER;
  double ops_sec = total_ops / seconds;

  printf("\n=== BENCH RESULTS ===\n");
  printf("Workers: %d\n", NUM_WORKERS);
  printf("Total ops: %.0f\n", total_ops);
  printf("Time: %.3f sec\n", seconds);
  printf("Throughput: %.2f ops/sec\n", ops_sec);
  printf("=====================\n\n");

  /* cleanup */
  for (int i = 0; i < NUM_WORKERS; i++) {
    send_cmd(workers[i], "quit\n");
  }

#ifdef _WIN32
  for (int i = 0; i < NUM_WORKERS; i++) {
    WaitForSingleObject(workers[i].pi.hProcess, INFINITE);
  }
#else
  for (int i = 0; i < NUM_WORKERS; i++) {
    wait(NULL);
  }
#endif

  data_manager_release_buffer(id);

  return 0;
}
