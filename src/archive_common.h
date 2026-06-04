/*
 * File Explorer - shared archive staging and layout helpers.
 */

#pragma once

#include <stdio.h>
#include <stddef.h>

#include "websrv.h"
#include "zip_archive.h"

#define RAR_STAGE_PREFIX ".bfpilot-rar-stage"

typedef struct rar_layout_choice {
  char app_prefix[ZIP_NAME_MAX];
  char dest_folder[256];
  int  app_has_eboot;
  int  app_depth;
  char eboot_parent[ZIP_NAME_MAX];
  int  eboot_depth;
  long files;
} rar_layout_choice_t;

typedef struct archive_backup_ctx {
  int   enabled;
  char  target[1024];
  char  root[1024];
  char  old_root[1024];
  FILE *manifest;
  int   old_count;
  int   added_count;
} archive_backup_ctx_t;

typedef int (*archive_cancel_cb_t)(void *opaque);
typedef void (*archive_progress_cb_t)(void *opaque, const char *path);

int archive_backup_begin(archive_backup_ctx_t *ctx, const char *target,
                         const char *archive_name, char *err,
                         size_t err_size);
void archive_backup_close(archive_backup_ctx_t *ctx, int complete);
int archive_backup_count(const char *target);
int archive_backup_record_added(archive_backup_ctx_t *ctx, const char *rel,
                                int is_dir, char *err, size_t err_size);
int archive_backup_move_existing(archive_backup_ctx_t *ctx, const char *path,
                                 const char *rel, char *err,
                                 size_t err_size);
int archive_rel_from_path(const char *root, const char *path, char *out,
                          size_t out_size);
int archive_backups_list_handler(const http_request_t *req);
int archive_backup_restore_handler(const http_request_t *req);

int rar_make_stage_path(const char *dest, char *out, size_t out_size);
int rar_delete_tree(const char *path);
int rar_merge_move_tree(const char *src, const char *dst);
int archive_chmod_777_recursive(const char *path, archive_cancel_cb_t cancel_cb,
                                void *cancel_opaque,
                                archive_progress_cb_t progress_cb,
                                void *progress_opaque, long *changed,
                                char *err, size_t err_size);
int archive_place_stage(const char *stage, const char *archive_name,
                        const char *dest, char *final_base,
                        size_t final_base_size, long *files,
                        char *err, size_t err_size);
int rar_choose_layout(const char *stage, const char *archive_name,
                      rar_layout_choice_t *layout, char *err,
                      size_t err_size);
int archive_ext_is_zip(const char *path);
int archive_ext_is_rar_first_volume(const char *path);
