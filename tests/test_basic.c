#include "instrument-data.h"
#include "process.h"
#include "util.h"
#include "test_common.h"
#include <cmocka.h>
#include <stdlib.h>
#include <string.h>
static void test_create_and_metadata(void **state) {
  (void)state;

  float data[4] = {1, 2, 3, 4};

  const char *instrument = "inst";
  const char *command_id = "cmd";

  const char *id = data_manager_create_buffer(instrument, command_id,
                                              INST_DATA_FLOAT32, 4, data);
  assert_non_null(id);

  SharedMetadata meta;
  bool ok = data_manager_get_metadata(id, &meta);
  assert_true(ok);

  assert_string_equal(meta.buffer_id, id);
  assert_string_equal(meta.instrument_name, instrument);
  assert_string_equal(meta.command_id, command_id);
  assert_int_equal(meta.type, INST_DATA_FLOAT32);
  assert_int_equal(meta.element_count, 4);
  assert_int_equal(meta.byte_size, sizeof(data));
  assert_true(meta.timestamp_ms > 0);

  uint64_t now = inst_get_timestamp_ms();
  assert_true(meta.timestamp_ms <= now);
  assert_true((now - meta.timestamp_ms) < 500);

  assert_int_equal(meta.global_ref_count, 1);

  uint32_t pid = inst_get_pid();
  assert_int_equal(meta.owners[0], pid);

  for (size_t i = 1; i < INST_MAX_OWNERS; i++) {
    assert_int_equal(meta.owners[i], 0);
  }

  data_manager_release_buffer(id);

  SharedMetadata meta_after;
  memset(&meta_after, 0xAB, sizeof(meta_after)); // poison to detect writes

  bool ok_after = data_manager_get_metadata(id, &meta_after);
  assert_false(ok_after);
  for (size_t i = 0; i < sizeof(meta_after); i++) {
    assert_int_equal(((unsigned char *)&meta_after)[i], 0xAB);
  }
}
static void test_reference_counting(void **state) {
  (void)state;

  float data[2] = {1.0f, 2.0f};

  const char *id =
      data_manager_create_buffer("Test", "CMD", INST_DATA_FLOAT32, 2, data);

  assert_non_null(id);

  DataBuffer *buffer1 = data_manager_get_buffer(id);
  DataBuffer *buffer2 = data_manager_get_buffer(id);
  DataBuffer *buffer3 = data_manager_get_buffer(id);

  assert_non_null(buffer1);
  assert_non_null(buffer2);
  assert_non_null(buffer3);

  SharedMetadata meta;
  assert_true(data_manager_get_metadata(id, &meta));
  assert_int_equal(meta.global_ref_count, 1);

  data_manager_release_buffer(id);

  DataBuffer *buffer4 = data_manager_get_buffer(id);
  assert_null(buffer4);
  assert_false(data_manager_get_metadata(id, &meta));
}

static void test_data_integrity(void **state) {
  (void)state;
  double data[3] = {10.0, 20.0, 30.0};

  const char *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_FLOAT64, 3, data);

  DataBuffer *buffer = data_manager_get_buffer(id);
  assert_non_null(buffer);

  double *ptr = data_buffer_data(buffer);

  assert_float_equal(ptr[0], 10.0, TEST_EPSILON);
  assert_float_equal(ptr[2], 30.0, TEST_EPSILON);

  data_manager_release_buffer(id);
}

static void test_invalid_id(void **state) {
  (void)state;
  DataBuffer *buffer = data_manager_get_buffer("does_not_exist");
  assert_null(buffer);

  SharedMetadata meta;
  bool ok = data_manager_get_metadata("bad", &meta);
  assert_false(ok);
}
static void test_add_offset(void **state) {
  (void)state;
  double data[5] = {1, 2, 3, 4, 5};

  const char *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_FLOAT64, 5, data);

  assert_non_null(id);

  bool ok = data_manager_add_offset(id, 2.0);
  assert_true(ok);

  DataBuffer *b = data_manager_get_buffer(id);
  double *ptr = data_buffer_data(b);

  for (int i = 0; i < 5; i++) {
    assert_float_equal(ptr[i], data[i] + 2.0, TEST_EPSILON);
  }

  data_manager_release_buffer(id);
}

static void test_multiply_gain(void **state) {
  (void)state;
  double data[4] = {2, 4, 6, 8};

  const char *id =
      data_manager_create_buffer("inst", "cmd", INST_DATA_FLOAT64, 4, data);

  bool ok = data_manager_multiply_gain(id, 0.5);
  assert_true(ok);

  DataBuffer *b = data_manager_get_buffer(id);
  double *ptr = data_buffer_data(b);

  for (int i = 0; i < 4; i++) {
    assert_float_equal(ptr[i], data[i] * 0.5, TEST_EPSILON);
  }

  data_manager_release_buffer(id);
}
static void test_zero_copy_create(void **state) {
  (void)state;
  double *ptr = NULL;

  const char *id = data_manager_create_buffer_zero_copy(
      "inst", "zero", INST_DATA_FLOAT64, 10, (void **)&ptr);

  assert_non_null(id);
  assert_non_null(ptr);

  for (int i = 0; i < 10; i++) {
    ptr[i] = (double)i;
  }

  DataBuffer *b = data_manager_get_buffer(id);
  double *read = data_buffer_data(b);

  for (int i = 0; i < 10; i++) {
    assert_float_equal(ptr[i], (double)i, TEST_EPSILON);
  }

  data_manager_release_buffer(id);
}

const struct CMUnitTest test_basic_tests[] = {
    cmocka_unit_test(test_create_and_metadata),
    cmocka_unit_test(test_data_integrity),
    cmocka_unit_test(test_invalid_id),
    cmocka_unit_test(test_add_offset),
    cmocka_unit_test(test_multiply_gain),
    cmocka_unit_test(test_zero_copy_create),
    cmocka_unit_test(test_reference_counting),
};

const size_t test_basic_tests_count =
    sizeof(test_basic_tests) / sizeof(test_basic_tests[0]);
