#pragma once

#include "instrument-data/instrument-data-export.h"
#include <glib.h>
#include <stddef.h>
#include <stdint.h>

/** Maximum length for string fields in metadata (including null terminator). */
#define INST_MAX_STRING_LEN 64

/** Maximum number of simultaneous process owners of a buffer. */
#define INST_MAX_OWNERS 32

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Supported data types for buffers.
 */
typedef enum {
  INST_DATA_FLOAT32 = 0,
  INST_DATA_FLOAT64,
  INST_DATA_INT32,
  INST_DATA_INT64,
  INST_DATA_UINT32,
  INST_DATA_UINT64,
  INST_DATA_UINT8
} ArrayType;

/**
 * @brief Opaque handle representing a shared memory-backed data buffer.
 *
 * The actual memory is stored in shared memory and may be accessed by
 * multiple processes. The DataBuffer object itself is local to the process.
 */
typedef struct DataBuffer DataBuffer;

/**
 * @brief Metadata associated with a shared buffer.
 *
 * This struct resides in shared memory and is visible across processes.
 */
typedef struct {
  gchar buffer_id[INST_MAX_STRING_LEN];
  gchar instrument_name[INST_MAX_STRING_LEN];
  gchar command_id[INST_MAX_STRING_LEN];

  ArrayType type;

  size_t element_count;
  size_t byte_size;

  guint64 timestamp_ms;

  /**
   * @brief Number of active owning processes.
   *
   * This represents the global lifetime of the buffer. The buffer will
   * be destroyed when this reaches 0.
   */
  volatile guint32 global_ref_count;

  /**
   * @brief List of owning process IDs.
   *
   * Used for crash-safe cleanup and ownership tracking.
   */
  guint32 owners[INST_MAX_OWNERS];

} SharedMetadata;

/* ============================================================
 * Buffer API
 * ============================================================ */

/**
 * @brief Get a pointer to the raw buffer data.
 *
 * @param buffer The buffer handle.
 * @return Pointer to the underlying data in shared memory.
 *
 * @note The returned pointer is valid only while the buffer is held.
 *       It is shared across processes.
 */
INSTRUMENT_DATA_EXPORT void *data_buffer_data(DataBuffer *buffer);

/**
 * @brief Get the number of elements in the buffer.
 *
 * @param buffer The buffer handle.
 * @return Number of elements.
 */
INSTRUMENT_DATA_EXPORT size_t
data_buffer_element_count(const DataBuffer *buffer);

/**
 * @brief Get the data type of the buffer.
 *
 * @param buffer The buffer handle.
 * @return The data type.
 */
INSTRUMENT_DATA_EXPORT ArrayType data_buffer_type(const DataBuffer *buffer);

/* ============================================================
 * Manager API
 * ============================================================ */

/**
 * @brief Create a new shared buffer.
 *
 * @param instrument Name of the originating instrument.
 * @param command_id Command identifier associated with this data.
 * @param type Data type of the buffer.
 * @param element_count Number of elements.
 * @param data Optional pointer to initial data.
 *
 * @return Newly created buffer ID string, or NULL on failure.
 *
 * @details
 * - If @p data is not NULL, the data is copied into shared memory.
 * - If @p data is NULL, the buffer is allocated and zero-initialized.
 *
 * @note The caller owns the returned string and must free it.
 */
INSTRUMENT_DATA_EXPORT gchar *
data_manager_create_buffer(const gchar *instrument, const gchar *command_id,
                           ArrayType type, size_t element_count,
                           const void *data);

/**
 * @brief Create a buffer using zero-copy semantics.
 *
 * @param instrument Name of the originating instrument.
 * @param command_id Command identifier.
 * @param type Data type of the buffer.
 * @param element_count Number of elements.
 * @param[out] out_ptr Pointer that will receive the shared memory address.
 *
 * @return Newly created buffer ID, or NULL on failure.
 *
 * @details
 * The caller receives direct access to the shared memory region and may
 * write to it directly without any copy.
 *
 * @warning The caller is responsible for initializing the memory properly.
 *
 * @note The returned pointer is shared across processes.
 */
INSTRUMENT_DATA_EXPORT gchar *
data_manager_create_buffer_zero_copy(const gchar *instrument,
                                     const gchar *command_id, ArrayType type,
                                     size_t element_count, void **out_ptr);

/**
 * @brief Retrieve a buffer by ID.
 *
 * @param id Buffer identifier.
 * @return Buffer handle, or NULL if not found.
 *
 * @details
 * This attaches the current process as an owner of the buffer.
 * The caller must eventually call data_manager_release_buffer().
 */
INSTRUMENT_DATA_EXPORT DataBuffer *data_manager_get_buffer(const gchar *id);

/**
 * @brief Release a buffer previously obtained by this process.
 *
 * @param id Buffer identifier.
 *
 * @details
 * This removes the current process from the ownership list.
 * If no processes remain, the shared memory is destroyed.
 *
 * This does NOT immediately free memory if other processes still hold it.
 */
INSTRUMENT_DATA_EXPORT void data_manager_release_buffer(const gchar *id);

/**
 * @brief Add a constant offset to all elements in the buffer.
 *
 * @param id Buffer identifier.
 * @param offset Value to add.
 *
 * @return TRUE on success, FALSE on failure.
 *
 * @note Only supports FLOAT32 and FLOAT64 buffers.
 */
INSTRUMENT_DATA_EXPORT gboolean data_manager_add_offset(const gchar *id,
                                                        double offset);

/**
 * @brief Multiply all elements in the buffer by a gain value.
 *
 * @param id Buffer identifier.
 * @param gain Multiplicative factor.
 *
 * @return TRUE on success, FALSE on failure.
 *
 * @note Only supports FLOAT32 and FLOAT64 buffers.
 */
INSTRUMENT_DATA_EXPORT gboolean data_manager_multiply_gain(const gchar *id,
                                                           double gain);

/**
 * @brief List all active buffer IDs.
 *
 * @param[out] count Number of buffers returned.
 * @return Array of buffer IDs (NULL-terminated).
 *
 * @note The caller must free the returned array and each string.
 */
INSTRUMENT_DATA_EXPORT gchar **data_manager_list_buffers(size_t *count);

/**
 * @brief Get total memory usage across all buffers.
 *
 * @return Total number of bytes used by all buffers.
 */
INSTRUMENT_DATA_EXPORT size_t data_manager_total_memory_usage(void);

/**
 * @brief Retrieve metadata for a buffer.
 *
 * @param id Buffer identifier.
 * @param[out] out_meta Output struct to receive metadata.
 *
 * @return TRUE if successful, FALSE if buffer does not exist.
 *
 * @note Metadata is copied from shared memory into @p out_meta.
 */
INSTRUMENT_DATA_EXPORT gboolean
data_manager_get_metadata(const gchar *id, SharedMetadata *out_meta);

#ifdef __cplusplus
}
#endif
