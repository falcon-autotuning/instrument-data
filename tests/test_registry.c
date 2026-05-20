#include "instrument-data.h"
#include <glib.h>

static void test_list_buffers(void) {
  gchar *id1 = data_manager_create_buffer("a", "b", INST_DATA_UINT8, 10, NULL);
  gchar *id2 = data_manager_create_buffer("a", "b", INST_DATA_UINT8, 10, NULL);

  size_t count = 0;
  gchar **list = data_manager_list_buffers(&count);

  g_assert_true(count >= 2);

  gboolean found1 = FALSE;
  gboolean found2 = FALSE;

  for (size_t i = 0; i < count; i++) {
    if (g_strcmp0(list[i], id1) == 0) {
      found1 = TRUE;
    }
    if (g_strcmp0(list[i], id2) == 0) {
      found2 = TRUE;
    }
    g_free(list[i]);
  }
  g_free(list);

  g_assert_true(found1);
  g_assert_true(found2);

  g_free(id1);
  g_free(id2);
}

static void test_total_memory(void) {
  size_t before = data_manager_total_memory_usage();

  gchar *id = data_manager_create_buffer("a", "b", INST_DATA_UINT8, 100, NULL);

  size_t after = data_manager_total_memory_usage();

  g_assert_cmpint(after, >, before);

  g_free(id);
}

void test_registry_register(void) {
  g_test_add_func("/registry/list_buffers", test_list_buffers);
  g_test_add_func("/registry/total_memory", test_total_memory);
}
