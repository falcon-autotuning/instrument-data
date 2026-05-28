#include "instrument-data.h"
#include <cmocka.h>
#include <stdlib.h>
#include <string.h>

/* Safe strcmp equivalent to g_strcmp0 */
static int str_eq(const char *a, const char *b) {
  if (!a && !b)
    return 1;
  if (!a || !b)
    return 0;
  return strcmp(a, b) == 0;
}

/* ============================================================
 * TESTS
 * ============================================================ */

static void test_list_buffers(void **state) {
  (void)state;

  char *id1 = data_manager_create_buffer("a", "b", INST_DATA_UINT8, 10, NULL);
  char *id2 = data_manager_create_buffer("a", "b", INST_DATA_UINT8, 10, NULL);

  size_t count = 0;
  char **list = data_manager_list_buffers(&count);

  assert_non_null(list);
  assert_true(count >= 2);

  bool found1 = false;
  bool found2 = false;

  for (size_t i = 0; i < count; i++) {
    if (str_eq(list[i], id1)) {
      found1 = true;
    }
    if (str_eq(list[i], id2)) {
      found2 = true;
    }
    free(list[i]);
  }
  free(list);

  assert_true(found1);
  assert_true(found2);

  free(id1);
  free(id2);
}

static void test_total_memory(void **state) {
  (void)state;

  size_t before = data_manager_total_memory_usage();

  char *id = data_manager_create_buffer("a", "b", INST_DATA_UINT8, 100, NULL);

  size_t after = data_manager_total_memory_usage();

  /* replace g_assert_cmpint(after, >, before) */
  assert_true(after > before);

  free(id);
}

/* ============================================================
 * TEST REGISTRATION
 * ============================================================ */

const struct CMUnitTest test_registry_tests[] = {
    cmocka_unit_test(test_list_buffers),
    cmocka_unit_test(test_total_memory),
};

const size_t test_registry_tests_count =
    sizeof(test_registry_tests) / sizeof(test_registry_tests[0]);
