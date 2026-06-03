#include "internal/util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* ============================================================
 * inst_strdup
 * ============================================================ */

char *inst_strdup(const char *s) {
  if (!s)
    return NULL;

  size_t len = strlen(s) + 1;
  char *out = (char *)malloc(len);
  if (!out)
    return NULL;

  memcpy(out, s, len);
  return out;
}

/* ============================================================
 * Random bytes helper
 * ============================================================ */

static int inst_random_bytes(uint8_t *buf, size_t len) {
#ifdef _WIN32
  return BCryptGenRandom(NULL, buf, (ULONG)len,
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
    return 0;

  ssize_t r = read(fd, buf, len);
  close(fd);

  return r == (ssize_t)len;
#endif
}

/* ============================================================
 * UUID v4 generator
 * ============================================================ */

char *inst_uuid_string(void) {
  uint8_t b[16];

  if (!inst_random_bytes(b, sizeof(b))) {
    return NULL;
  }

  /* RFC 4122 UUID v4 adjustments */
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;

  char buf[37]; /* 36 chars + null terminator */

  snprintf(buf, sizeof(buf),
           "%02x%02x%02x%02x-"
           "%02x%02x-"
           "%02x%02x-"
           "%02x%02x-"
           "%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
           b[11], b[12], b[13], b[14], b[15]);

  return inst_strdup(buf);
}

/* ============================================================
 * Name generator
 * ============================================================ */

char *inst_make_name(const char *prefix) {
  if (!prefix)
    return NULL;

  char *uuid = inst_uuid_string();
  if (!uuid)
    return NULL;

  /* prefix + '_' + uuid + null */
  size_t len = strlen(prefix) + 1 + strlen(uuid) + 1;

  char *out = (char *)malloc(len);
  if (!out) {
    free(uuid);
    return NULL;
  }

  snprintf(out, len, "%s_%s", prefix, uuid);

  free(uuid);
  return out;
}

void inst_strlcpy(char *dst, const char *src, size_t size) {
  if (!dst || size == 0) {
    return;
  }

  if (!src) {
    dst[0] = '\0';
    return;
  }

  size_t i = 0;

  /* copy until end or size-1 */
  for (; i < size - 1 && src[i] != '\0'; i++) {
    dst[i] = src[i];
  }

  /* always null terminate */
  dst[i] = '\0';
}

#ifdef _WIN32
#include <windows.h>

/* Windows FILETIME is 100ns since Jan 1 1601 (UTC) */
uint64_t inst_get_timestamp_ms(void) {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);

  ULARGE_INTEGER uli;
  uli.LowPart = ft.dwLowDateTime;
  uli.HighPart = ft.dwHighDateTime;

  /* Convert to Unix epoch (Jan 1 1970) */
  const uint64_t EPOCH_DIFF = 116444736000000000ULL;

  return (uli.QuadPart - EPOCH_DIFF) / 10000ULL; /* 100ns -> ms */
}

#else
#include <time.h>

uint64_t inst_get_timestamp_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);

  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
#endif
