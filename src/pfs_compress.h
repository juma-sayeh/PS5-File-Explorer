/*
 * File Explorer - streamed PFS/PFSC app compression.
 */

#pragma once

#include <stddef.h>

typedef struct pfs_app_info {
  char title_id[64];
  char source_path[1024];
  char output_path[1024];
  char nested_name[256];
  int  format;
  int  delete_policy;
  int  output_exists;
} pfs_app_info_t;

typedef struct pfs_decompress_info {
  char source_path[1024];
  char output_path[1024];
  char nested_name[256];
  int  nested_type;
  int  delete_policy;
  int  output_exists;
} pfs_decompress_info_t;

#define PFS_COMPRESS_FORMAT_PFS   0
#define PFS_COMPRESS_FORMAT_EXFAT 1

#define PFS_DELETE_KEEP   0
#define PFS_DELETE_AFTER  1
#define PFS_DELETE_STREAM 2

#define PFS_NESTED_UNKNOWN 0
#define PFS_NESTED_PFS     1
#define PFS_NESTED_EXFAT   2

#define PFS_COMPRESS_DEFAULT_WORKERS 4
#define PFS_COMPRESS_MAX_WORKERS 64

int pfs_app_probe(const char *path, pfs_app_info_t *info,
                  char *err, size_t err_size);

int pfs_compress_app_to_ffpfsc(const char *path, int overwrite,
                               pfs_app_info_t *info,
                               char *err, size_t err_size);

int pfs_compress_app_to_ffpfsc_ex(const char *path, int overwrite,
                                  int workers, int convert,
                                  pfs_app_info_t *info,
                                  char *err, size_t err_size);

int pfs_compress_app_to_ffpfsc_opts(const char *path, int overwrite,
                                    int workers, int format,
                                    int delete_policy,
                                    pfs_app_info_t *info,
                                    char *err, size_t err_size);

int pfs_decompress_probe(const char *path, pfs_decompress_info_t *info,
                         char *err, size_t err_size);

int pfs_decompress_detect_nested(const char *path, pfs_decompress_info_t *info,
                                 char *err, size_t err_size);

int pfs_decompress_ffpfsc_to_app(const char *path, int overwrite,
                                 pfs_decompress_info_t *info,
                                 char *err, size_t err_size);

int pfs_decompress_ffpfsc_to_app_ex(const char *path, int overwrite,
                                    int workers, int convert,
                                    pfs_decompress_info_t *info,
                                    char *err, size_t err_size);

int pfs_decompress_ffpfsc_to_app_opts(const char *path, int overwrite,
                                      int workers, int delete_policy,
                                      pfs_decompress_info_t *info,
                                      char *err, size_t err_size);
