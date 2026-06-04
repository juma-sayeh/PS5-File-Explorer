/*
 * BFpilot - thin C ABI around RARLab UnRAR streaming extraction.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*rar_extract_read_cb_t)(void *opaque, void *data, size_t size,
                                     size_t *read_size);
typedef int (*rar_extract_cancel_cb_t)(void *opaque);

typedef struct rar_extract_opts {
  const char             *archive_name;
  const char             *dest_dir;
  const char             *password;
  uint64_t                dictionary_limit;
  rar_extract_read_cb_t   read_cb;
  rar_extract_cancel_cb_t cancel_cb;
  void                   *opaque;
} rar_extract_opts_t;

int rar_extract_stream(const rar_extract_opts_t *opts,
                       char *err, size_t err_size);

int rar_extract_file(const rar_extract_opts_t *opts,
                     char *err, size_t err_size);

#ifdef __cplusplus
}
#endif
