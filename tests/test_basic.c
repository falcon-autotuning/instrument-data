#include "instrument-data.h"
#include <glib.h>

static void test_create_and_metadata(void) {
  float data[4] = {1, 2, 3, 4};

  gchar *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_FLOAT32, 4, data);

  g_assert_nonnull(id);

  SharedMetadata meta;
  gboolean ok = data_manager_get_metadata(id, &meta);
  g_assert_true(ok);

  g_assert_cmpint(meta.element_count, ==, 4);
  g_assert_cmpint(meta.byte_size, ==, sizeof(data));

  g_free(id);
}

static void test_data_integrity(void) {
  double data[3] = {10.0, 20.0, 30.0};

  gchar *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_FLOAT64, 3, data);

  DataBuffer *buffer = data_manager_get_buffer(id);
  g_assert_nonnull(buffer);

  double *ptr = data_buffer_data(buffer);

  g_assert_cmpfloat(ptr[0], ==, 10.0);
  g_assert_cmpfloat(ptr[2], ==, 30.0);

  data_manager_release_buffer(id);
  g_free(id);
}

static void test_invalid_id(void) {
  DataBuffer *buffer = data_manager_get_buffer("does_not_exist");
  g_assert_null(buffer);

  SharedMetadata meta;
  gboolean ok = data_manager_get_metadata("bad", &meta);
  g_assert_false(ok);
}
static void test_add_offset(void) {
  double data[5] = {1, 2, 3, 4, 5};

  gchar *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_FLOAT64, 5, data);

  g_assert_nonnull(id);

  gboolean ok = data_manager_add_offset(id, 2.0);
  g_assert_true(ok);

  DataBuffer *b = data_manager_get_buffer(id);
  double *ptr = data_buffer_data(b);

  for (int i = 0; i < 5; i++) {
    g_assert_cmpfloat(ptr[i], ==, data[i] + 2.0);
  }

  data_manager_release_buffer(id);
  g_free(id);
}

static void test_multiply_gain(void) {
  double data[4] = {2, 4, 6, 8};

  gchar *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_FLOAT64, 4, data);

  gboolean ok = data_manager_multiply_gain(id, 0.5);
  g_assert_true(ok);

  DataBuffer *b = data_manager_get_buffer(id);
  double *ptr = data_buffer_data(b);

  for (int i = 0; i < 4; i++) {
    g_assert_cmpfloat(ptr[i], ==, data[i] * 0.5);
  }

  data_manager_release_buffer(id);
  g_free(id);
}
static void test_zero_copy_create(void) {
  double *ptr = NULL;

  gchar *id = data_manager_create_buffer_zero_copy(
      "inst", "zero", INST_DATA_FLOAT64, 10, (void **)&ptr);

  g_assert_nonnull(id);
  g_assert_nonnull(ptr);

  for (int i = 0; i < 10; i++) {
    ptr[i] = (double)i;
  }

  DataBuffer *b = data_manager_get_buffer(id);
  double *read = data_buffer_data(b);

  for (int i = 0; i < 10; i++) {
    g_assert_cmpfloat(read[i], ==, (double)i);
  }

  data_manager_release_buffer(id);
  g_free(id);
}

void test_basic_register(void) {
  g_test_add_func("/basic/create_metadata", test_create_and_metadata);
  g_test_add_func("/basic/data_integrity", test_data_integrity);
  g_test_add_func("/basic/invalid_id", test_invalid_id);
  g_test_add_func("/basic/add_offset", test_add_offset);
  g_test_add_func("/basic/multiply_gain", test_multiply_gain);
  g_test_add_func("/basic/zero_copy_create", test_zero_copy_create);
}
