#pragma once

#include <stddef.h>

/**
 * @file util.h
 * @brief Internal utility helpers for instrument-data.
 *
 * @details
 * This header defines shared internal helper functions used across
 * multiple implementation files (e.g., shm.c, manager.c). These
 * functions are NOT part of the public API and should not be exposed
 * to external users of the library.
 *
 * All functions here return heap-allocated memory unless otherwise noted.
 * The caller is responsible for freeing returned strings.
 */

/**
 * @brief Duplicate a string using heap allocation.
 *
 * @param s Input string (may be NULL).
 * @return Newly allocated copy, or NULL if input is NULL or allocation fails.
 *
 * @note Caller must free the returned string.
 */
char *inst_strdup(const char *s);

/**
 * @brief Generate a UUID version 4 (random) string.
 *
 * @return Newly allocated UUID string in standard format:
 *         xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
 *
 * @note
 * - Uses platform entropy source:
 *   - Windows: BCryptGenRandom
 *   - POSIX: /dev/urandom
 *
 * @note Caller must free the returned string.
 */
char *inst_uuid_string(void);

/**
 * @brief Generate a unique name using a prefix and UUID.
 *
 * @param prefix String prefix (e.g., "/inst", "Global\\inst", "buffer").
 * @return Newly allocated string in the format:
 *         prefix_uuid
 *
 * @example
 *   inst_make_name("buffer")
 *   -> "buffer_550e8400-e29b-41d4-a716-446655440000"
 *
 * @note Caller must free the returned string.
 */
char *inst_make_name(const char *prefix);

/**
 * @brief Safe string copy with guaranteed null termination.
 *
 * @param dst Destination buffer.
 * @param src Source string (may be NULL).
 * @param size Size of destination buffer.
 *
 * @details
 * Copies up to size-1 characters from src into dst and always
 * null-terminates the result (if size > 0).
 *
 * If src is NULL, dst becomes an empty string.
 */
void inst_strlcpy(char *dst, const char *src, size_t size);
