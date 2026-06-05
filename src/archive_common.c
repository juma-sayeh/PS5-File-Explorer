/*
 * File Explorer - shared archive staging and layout helpers.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "archive_common.h"
#include "transfer_internal.h"
#include "zip_archive.h"

static atomic_int g_rar_stage_counter;
static atomic_int g_archive_backup_counter;

#define BACKUP_DIR_NAME ".bfpilot-backups"
#define BACKUP_MANIFEST "manifest.txt"
#define BACKUP_OLD_DIR "old"

static int rar_segment_safe(const char *seg);
static int archive_app_dir_candidate(const char *path, char *title,
                                     size_t title_size);
static int archive_job_cancel_cb(void *opaque);


static int
archive_token_safe(const char *name) {
  if(!name || !*name || strlen(name) > 180 || strstr(name, "..")) return 0;
  for(const unsigned char *p = (const unsigned char *)name; *p; p++) {
    if(!isalnum(*p) && *p != '-' && *p != '_' && *p != '.') return 0;
  }
  return 1;
}


static int
archive_parent_path(const char *path, char *parent, size_t parent_size,
                    char *base, size_t base_size) {
  const char *slash;
  size_t parent_len;
  if(!path || !*path || !parent || parent_size == 0 ||
     !base || base_size == 0) {
    errno = EINVAL;
    return -1;
  }
  slash = strrchr(path, '/');
  if(!slash || !slash[1]) {
    errno = EINVAL;
    return -1;
  }
  parent_len = (slash == path) ? 1 : (size_t)(slash - path);
  if(parent_len >= parent_size || strlen(slash + 1) >= base_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(parent, path, parent_len);
  parent[parent_len] = 0;
  snprintf(base, base_size, "%s", slash + 1);
  return 0;
}


static int
archive_backup_app_root(const char *target, char *out, size_t out_size,
                        char *base_out, size_t base_out_size) {
  char parent[1024];
  char base[256];
  int n;
  if(archive_parent_path(target, parent, sizeof(parent), base, sizeof(base)) != 0) {
    return -1;
  }
  if(!upload_segment_safe(base)) {
    errno = EINVAL;
    return -1;
  }
  n = snprintf(out, out_size, "%s%s%s/%s",
               parent, parent[1] ? "/" : "", BACKUP_DIR_NAME, base);
  if(n < 0 || (size_t)n >= out_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if(base_out && base_out_size > 0) {
    snprintf(base_out, base_out_size, "%s", base);
  }
  return 0;
}


int
archive_rel_from_path(const char *root, const char *path, char *out,
                      size_t out_size) {
  size_t n = strlen(root);
  while(n > 1 && root[n - 1] == '/') n--;
  if(strncmp(root, path, n) != 0 || (path[n] != '/' && path[n] != 0)) {
    errno = EINVAL;
    return -1;
  }
  const char *rel = path[n] == '/' ? path + n + 1 : path + n;
  if(!*rel || strlen(rel) >= out_size) {
    errno = rel[0] ? ENAMETOOLONG : EINVAL;
    return -1;
  }
  snprintf(out, out_size, "%s", rel);
  return 0;
}


static int
archive_rel_path_safe(const char *rel) {
  char tmp[ZIP_NAME_MAX];
  char *seg;
  if(!rel || !*rel || strlen(rel) >= sizeof(tmp) || rel[0] == '/') return 0;
  snprintf(tmp, sizeof(tmp), "%s", rel);
  seg = tmp;
  while(seg && *seg) {
    char *slash = strchr(seg, '/');
    if(slash) *slash = 0;
    if(!rar_segment_safe(seg)) return 0;
    if(!slash) break;
    seg = slash + 1;
  }
  return 1;
}


static int
archive_mkdir_parent(const char *path) {
  char parent[1024], base[256];
  if(archive_parent_path(path, parent, sizeof(parent), base, sizeof(base)) != 0) {
    return -1;
  }
  return mkdirs(parent);
}


static int
archive_backup_path(char *out, size_t out_size, const char *root,
                    const char *rel) {
  int n;
  if(!archive_rel_path_safe(rel)) {
    errno = EINVAL;
    return -1;
  }
  n = snprintf(out, out_size, "%s/%s", root, rel);
  if(n < 0 || (size_t)n >= out_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}


int
archive_backup_begin(archive_backup_ctx_t *ctx, const char *target,
                     const char *archive_name, char *err, size_t err_size) {
  char app_root[1024];
  char manifest_path[1024];
  char base[256];
  time_t now = time(NULL);
  int seq = atomic_fetch_add(&g_archive_backup_counter, 1);
  int n;

  if(!ctx) return 0;
  memset(ctx, 0, sizeof(*ctx));
  if(archive_backup_app_root(target, app_root, sizeof(app_root),
                             base, sizeof(base)) != 0) {
    snprintf(err, err_size, "backup path: %s", strerror(errno));
    return -1;
  }
  if(mkdirs(app_root) != 0) {
    snprintf(err, err_size, "backup root: %s", strerror(errno));
    return -1;
  }
  n = snprintf(ctx->root, sizeof(ctx->root), "%s/%ld-%ld-%d",
               app_root, (long)now, (long)getpid(), seq);
  if(n < 0 || (size_t)n >= sizeof(ctx->root)) {
    snprintf(err, err_size, "backup path too long");
    return -1;
  }
  n = snprintf(ctx->old_root, sizeof(ctx->old_root), "%s/%s",
               ctx->root, BACKUP_OLD_DIR);
  if(n < 0 || (size_t)n >= sizeof(ctx->old_root)) {
    snprintf(err, err_size, "backup path too long");
    return -1;
  }
  if(mkdirs(ctx->old_root) != 0) {
    snprintf(err, err_size, "backup old root: %s", strerror(errno));
    return -1;
  }
  n = snprintf(manifest_path, sizeof(manifest_path), "%s/%s",
               ctx->root, BACKUP_MANIFEST);
  if(n < 0 || (size_t)n >= sizeof(manifest_path)) {
    snprintf(err, err_size, "backup manifest path too long");
    return -1;
  }
  ctx->manifest = fopen(manifest_path, "w");
  if(!ctx->manifest) {
    snprintf(err, err_size, "backup manifest: %s", strerror(errno));
    return -1;
  }
  snprintf(ctx->target, sizeof(ctx->target), "%s", target);
  ctx->enabled = 1;
  fprintf(ctx->manifest, "File Explorer archive backup v1\n");
  fprintf(ctx->manifest, "Target: %s\n", target);
  fprintf(ctx->manifest, "Archive: %s\n", archive_name ? archive_name : "");
  fprintf(ctx->manifest, "Created: %ld\n", (long)now);
  fprintf(ctx->manifest, "App: %s\n", base);
  fflush(ctx->manifest);
  return 0;
}


void
archive_backup_close(archive_backup_ctx_t *ctx, int complete) {
  if(!ctx || !ctx->manifest) return;
  int empty = ctx->old_count == 0 && ctx->added_count == 0;
  fprintf(ctx->manifest, "OldCount: %d\n", ctx->old_count);
  fprintf(ctx->manifest, "AddedCount: %d\n", ctx->added_count);
  fprintf(ctx->manifest, "Complete: %d\n", complete ? 1 : 0);
  fclose(ctx->manifest);
  ctx->manifest = NULL;
  if(empty && ctx->root[0]) {
    rar_delete_tree(ctx->root);
  }
}


int
archive_backup_record_added(archive_backup_ctx_t *ctx, const char *rel,
                            int is_dir, char *err, size_t err_size) {
  if(!ctx || !ctx->enabled || !ctx->manifest) return 0;
  if(!archive_rel_path_safe(rel)) {
    snprintf(err, err_size, "backup added path is unsafe");
    return -1;
  }
  fprintf(ctx->manifest, "A\t%s%s\n", rel, is_dir ? "/" : "");
  fflush(ctx->manifest);
  ctx->added_count++;
  return 0;
}


int
archive_backup_move_existing(archive_backup_ctx_t *ctx, const char *path,
                             const char *rel, char *err, size_t err_size) {
  char backup_path[1024];
  if(!ctx || !ctx->enabled || !ctx->manifest) return 0;
  if(archive_backup_path(backup_path, sizeof(backup_path),
                         ctx->old_root, rel) != 0) {
    snprintf(err, err_size, "backup path: %s", strerror(errno));
    return -1;
  }
  if(archive_mkdir_parent(backup_path) != 0) {
    snprintf(err, err_size, "backup parent: %s", strerror(errno));
    return -1;
  }
  if(rename(path, backup_path) != 0) {
    snprintf(err, err_size, "backup existing file: %s", strerror(errno));
    return -1;
  }
  fprintf(ctx->manifest, "O\t%s\n", rel);
  fflush(ctx->manifest);
  ctx->old_count++;
  return 0;
}


int
archive_backup_count(const char *target) {
  char app_root[1024];
  DIR *d;
  int count = 0;
  if(archive_backup_app_root(target, app_root, sizeof(app_root), NULL, 0) != 0) {
    return 0;
  }
  d = opendir(app_root);
  if(!d) return 0;
  struct dirent *ent;
  while((ent = readdir(d))) {
    char manifest[1024];
    struct stat st;
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    if(!archive_token_safe(ent->d_name)) continue;
    if(snprintf(manifest, sizeof(manifest), "%s/%s/%s",
                app_root, ent->d_name, BACKUP_MANIFEST) >=
       (int)sizeof(manifest)) {
      continue;
    }
    if(lstat(manifest, &st) == 0 && S_ISREG(st.st_mode)) count++;
  }
  closedir(d);
  return count;
}


typedef struct backup_manifest_info {
  char archive[256];
  long created;
  int  old_count;
  int  added_count;
  int  complete;
} backup_manifest_info_t;


static void
archive_chomp(char *s) {
  size_t n = strlen(s);
  while(n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
    s[--n] = 0;
  }
}


static int
archive_read_manifest_info(const char *manifest,
                           backup_manifest_info_t *info) {
  FILE *f = fopen(manifest, "r");
  if(!f) return -1;
  memset(info, 0, sizeof(*info));
  char line[1024];
  int saw_old_count = 0;
  int saw_added_count = 0;
  while(fgets(line, sizeof(line), f)) {
    archive_chomp(line);
    if(!strncmp(line, "Archive: ", 9)) {
      snprintf(info->archive, sizeof(info->archive), "%s", line + 9);
    } else if(!strncmp(line, "Created: ", 9)) {
      info->created = strtol(line + 9, NULL, 10);
    } else if(!strncmp(line, "OldCount: ", 10)) {
      info->old_count = atoi(line + 10);
      saw_old_count = 1;
    } else if(!strncmp(line, "AddedCount: ", 12)) {
      info->added_count = atoi(line + 12);
      saw_added_count = 1;
    } else if(!strncmp(line, "Complete: ", 10)) {
      info->complete = atoi(line + 10) != 0;
    } else if(!strncmp(line, "O\t", 2) && !saw_old_count) {
      info->old_count++;
    } else if(!strncmp(line, "A\t", 2) && !saw_added_count) {
      info->added_count++;
    }
  }
  fclose(f);
  return 0;
}


int
archive_backups_list_handler(const http_request_t *req) {
  char target[1024];
  char app_root[1024];
  DIR *d;
  if(!websrv_get_query_arg(req, "path", target, sizeof(target)) ||
     !path_is_safe(target)) {
    return serve_error(req, 400, "bad backup target");
  }
  if(archive_backup_app_root(target, app_root, sizeof(app_root), NULL, 0) != 0) {
    return serve_error(req, 400, "bad backup target");
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, target) != 0 ||
     json_append(&b, ",\"backups\":[") != 0) {
    free(b.data);
    return -1;
  }

  d = opendir(app_root);
  int first = 1;
  if(d) {
    struct dirent *ent;
    while((ent = readdir(d))) {
      char manifest[1024];
      struct stat st;
      backup_manifest_info_t info;
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      if(!archive_token_safe(ent->d_name)) continue;
      if(snprintf(manifest, sizeof(manifest), "%s/%s/%s",
                  app_root, ent->d_name, BACKUP_MANIFEST) >=
         (int)sizeof(manifest)) {
        continue;
      }
      if(lstat(manifest, &st) != 0 || !S_ISREG(st.st_mode)) continue;
      if(archive_read_manifest_info(manifest, &info) != 0) continue;
      if(!first && json_append(&b, ",") != 0) break;
      first = 0;
      if(json_append(&b, "{\"id\":") != 0 ||
         json_string(&b, ent->d_name) != 0 ||
         json_append(&b, ",\"archive\":") != 0 ||
         json_string(&b, info.archive) != 0 ||
         json_appendf(&b,
                      ",\"created\":%ld,\"old\":%d,\"added\":%d,"
                      "\"complete\":%s}",
                      info.created, info.old_count, info.added_count,
                      info.complete ? "true" : "false") != 0) {
        break;
      }
    }
    closedir(d);
  }

  if(json_append(&b, "]}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


typedef struct path_list {
  char **items;
  int    count;
  int    cap;
} path_list_t;


static void
path_list_free(path_list_t *list) {
  if(!list) return;
  for(int i = 0; i < list->count; i++) free(list->items[i]);
  free(list->items);
  memset(list, 0, sizeof(*list));
}


static int
path_list_add(path_list_t *list, const char *rel) {
  size_t n;
  if(!archive_rel_path_safe(rel)) return -1;
  if(list->count == list->cap) {
    int next = list->cap ? list->cap * 2 : 32;
    char **items = realloc(list->items, (size_t)next * sizeof(*items));
    if(!items) return -1;
    list->items = items;
    list->cap = next;
  }
  n = strlen(rel);
  while(n > 0 && rel[n - 1] == '/') n--;
  if(n == 0) return -1;
  list->items[list->count] = malloc(n + 1);
  if(!list->items[list->count]) return -1;
  memcpy(list->items[list->count], rel, n);
  list->items[list->count][n] = 0;
  list->count++;
  return 0;
}


static int
archive_restore_read_manifest(const char *manifest, path_list_t *added,
                              path_list_t *old, char *err, size_t err_size) {
  FILE *f = fopen(manifest, "r");
  if(!f) {
    snprintf(err, err_size, "backup manifest: %s", strerror(errno));
    return -1;
  }
  char line[1024];
  while(fgets(line, sizeof(line), f)) {
    archive_chomp(line);
    if(!strncmp(line, "A\t", 2)) {
      if(path_list_add(added, line + 2) != 0) {
        snprintf(err, err_size, "bad backup added path");
        fclose(f);
        return -1;
      }
    } else if(!strncmp(line, "O\t", 2)) {
      if(path_list_add(old, line + 2) != 0) {
        snprintf(err, err_size, "bad backup old path");
        fclose(f);
        return -1;
      }
    }
  }
  fclose(f);
  return 0;
}


static int
archive_join_rel_checked(char *out, size_t out_size, const char *root,
                         const char *rel) {
  return archive_backup_path(out, out_size, root, rel);
}


int
archive_backup_restore_handler(const http_request_t *req) {
  char target[1024];
  char id[256];
  char app_root[1024];
  char backup_dir[1024];
  char manifest[1024];
  char old_root[1024];
  char err[240] = {0};
  path_list_t added = {0};
  path_list_t old = {0};
  int rc = -1;

  if(!websrv_get_query_arg(req, "path", target, sizeof(target)) ||
     !path_is_safe(target) ||
     !websrv_get_query_arg(req, "id", id, sizeof(id)) ||
     !archive_token_safe(id)) {
    return serve_error(req, 400, "bad backup restore request");
  }
  if(archive_backup_app_root(target, app_root, sizeof(app_root), NULL, 0) != 0 ||
     snprintf(backup_dir, sizeof(backup_dir), "%s/%s", app_root, id) >=
       (int)sizeof(backup_dir) ||
     snprintf(manifest, sizeof(manifest), "%s/%s", backup_dir,
              BACKUP_MANIFEST) >= (int)sizeof(manifest) ||
     snprintf(old_root, sizeof(old_root), "%s/%s", backup_dir,
              BACKUP_OLD_DIR) >= (int)sizeof(old_root)) {
    return serve_error(req, 400, "backup path too long");
  }

  if(archive_restore_read_manifest(manifest, &added, &old,
                                   err, sizeof(err)) != 0) {
    goto done;
  }

  job_set_current("Restoring backup");
  for(int i = added.count - 1; i >= 0; i--) {
    char live[1024];
    struct stat st;
    if(archive_join_rel_checked(live, sizeof(live), target,
                                added.items[i]) != 0) {
      snprintf(err, sizeof(err), "restore path: %s", strerror(errno));
      goto done;
    }
    if(lstat(live, &st) == 0 && rar_delete_tree(live) != 0) {
      snprintf(err, sizeof(err), "remove added path: %s", strerror(errno));
      goto done;
    }
  }

  if(mkdirs(target) != 0) {
    snprintf(err, sizeof(err), "restore target: %s", strerror(errno));
    goto done;
  }
  for(int i = 0; i < old.count; i++) {
    char live[1024], backup_path[1024];
    struct stat st;
    if(archive_join_rel_checked(live, sizeof(live), target, old.items[i]) != 0 ||
       archive_join_rel_checked(backup_path, sizeof(backup_path), old_root,
                                old.items[i]) != 0) {
      snprintf(err, sizeof(err), "restore path: %s", strerror(errno));
      goto done;
    }
    if(lstat(backup_path, &st) != 0) {
      snprintf(err, sizeof(err), "backup file missing: %s", old.items[i]);
      goto done;
    }
    if(lstat(live, &st) == 0 && rar_delete_tree(live) != 0) {
      snprintf(err, sizeof(err), "remove current path: %s", strerror(errno));
      goto done;
    }
    if(archive_mkdir_parent(live) != 0) {
      snprintf(err, sizeof(err), "restore parent: %s", strerror(errno));
      goto done;
    }
    if(rename(backup_path, live) != 0) {
      snprintf(err, sizeof(err), "restore file: %s", strerror(errno));
      goto done;
    }
  }

  if(rar_delete_tree(backup_dir) != 0) {
    snprintf(err, sizeof(err), "remove restored backup: %s", strerror(errno));
    goto done;
  }
  rc = 0;

done:
  path_list_free(&added);
  path_list_free(&old);
  if(rc != 0) {
    return serve_error(req, 500, err[0] ? err : "backup restore failed");
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, target) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static void
archive_remove_empty_backup_parent(const char *app_root) {
  char parent[1024];
  char *slash;
  if(!app_root || !app_root[0]) return;
  snprintf(parent, sizeof(parent), "%s", app_root);
  slash = strrchr(parent, '/');
  if(!slash || slash == parent) return;
  *slash = 0;
  (void)rmdir(parent);
}


int
archive_backup_remove_handler(const http_request_t *req) {
  char target[1024];
  char id[256];
  char all_arg[32];
  char app_root[1024];
  char remove_path[1024];
  char manifest[1024];
  struct stat st;
  int remove_all = 0;
  int removed = 0;

  id[0] = 0;
  if(!websrv_get_query_arg(req, "path", target, sizeof(target)) ||
     !path_is_safe(target)) {
    return serve_error(req, 400, "bad backup remove request");
  }
  if(websrv_get_query_arg(req, "all", all_arg, sizeof(all_arg)) &&
     strcmp(all_arg, "0") != 0) {
    remove_all = 1;
  }
  if(!remove_all &&
     (!websrv_get_query_arg(req, "id", id, sizeof(id)) ||
      !archive_token_safe(id))) {
    return serve_error(req, 400, "bad backup remove request");
  }
  if(archive_backup_app_root(target, app_root, sizeof(app_root), NULL, 0) != 0) {
    return serve_error(req, 400, "bad backup target");
  }

  if(remove_all) {
    if(lstat(app_root, &st) != 0) {
      if(errno != ENOENT) return serve_error(req, 500, strerror(errno));
    } else {
      removed = archive_backup_count(target);
      if(rar_delete_tree(app_root) != 0) {
        return serve_error(req, 500, strerror(errno));
      }
      archive_remove_empty_backup_parent(app_root);
    }
  } else {
    if(snprintf(remove_path, sizeof(remove_path), "%s/%s", app_root, id) >=
         (int)sizeof(remove_path) ||
       snprintf(manifest, sizeof(manifest), "%s/%s", remove_path,
                BACKUP_MANIFEST) >= (int)sizeof(manifest)) {
      return serve_error(req, 400, "backup path too long");
    }
    if(lstat(manifest, &st) != 0 || !S_ISREG(st.st_mode)) {
      return serve_error(req, 404, "backup not found");
    }
    if(rar_delete_tree(remove_path) != 0) {
      return serve_error(req, 500, strerror(errno));
    }
    archive_remove_empty_backup_parent(app_root);
    removed = 1;
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, target) != 0 ||
     json_appendf(&b, ",\"removed\":%d}", removed) != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}

static int
rar_segment_safe(const char *seg) {
  if(!upload_segment_safe(seg)) return 0;
  for(const unsigned char *p = (const unsigned char *)seg; *p; p++) {
    if(*p == ':' || *p < 0x20) return 0;
  }
  return 1;
}


static int
rar_join_rel(char *out, size_t out_size, const char *base,
             const char *name) {
  size_t base_len = base ? strlen(base) : 0;
  size_t name_len = strlen(name);
  if(base_len + (base_len ? 1 : 0) + name_len + 1 > out_size) return -1;
  if(base_len) snprintf(out, out_size, "%s/%s", base, name);
  else snprintf(out, out_size, "%s", name);
  return 0;
}


static int
rar_rel_depth(const char *rel) {
  int depth = 0;
  int in_seg = 0;
  for(const char *p = rel; p && *p; p++) {
    if(*p == '/') {
      in_seg = 0;
    } else if(!in_seg) {
      depth++;
      in_seg = 1;
    }
  }
  return depth;
}


int
rar_make_stage_path(const char *dest, char *out, size_t out_size) {
  char clean[1024];
  int seq = atomic_fetch_add(&g_rar_stage_counter, 1);
  snprintf(clean, sizeof(clean), "%s", dest);
  size_t n = strlen(clean);
  while(n > 1 && clean[n - 1] == '/') clean[--n] = 0;

  for(int i = 0; i < 32; i++) {
    int rc = snprintf(out, out_size, "%s%s%s-%ld-%d",
                      clean, clean[1] ? "/" : "", RAR_STAGE_PREFIX,
                      (long)getpid(), seq + i);
    if(rc < 0 || (size_t)rc >= out_size) {
      errno = ENAMETOOLONG;
      return -1;
    }
    if(mkdir(out, 0777) == 0) return 0;
    if(errno != EEXIST) return -1;
  }
  errno = EEXIST;
  return -1;
}


int
rar_delete_tree(const char *path) {
  struct stat st;
  if(lstat(path, &st) != 0) return -1;
  if(S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if(!d) return -1;
    struct dirent *ent;
    int rc = 0;
    while((ent = readdir(d))) {
      char child[1024];
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      if(strlen(path) + strlen(ent->d_name) + 2 >= sizeof(child)) {
        errno = ENAMETOOLONG;
        rc = -1;
        break;
      }
      join_path(child, sizeof(child), path, ent->d_name);
      if(rar_delete_tree(child) != 0) rc = -1;
    }
    closedir(d);
    if(rc == 0 && rmdir(path) != 0) rc = -1;
    return rc;
  }
  return unlink(path);
}


static int
archive_merge_move_tree(const char *src, const char *dst, const char *rel,
                        archive_backup_ctx_t *backup,
                        char *err, size_t err_size) {
  struct stat st;
  if(job_cancelled()) {
    errno = ECANCELED;
    return -1;
  }
  if(lstat(src, &st) != 0) return -1;

  if(S_ISDIR(st.st_mode)) {
    struct stat dst_st;
    if(lstat(dst, &dst_st) != 0) {
      if(errno != ENOENT) return -1;
      if(backup && backup->enabled && rel && rel[0] &&
         archive_backup_record_added(backup, rel, 1, err, err_size) != 0) {
        return -1;
      }
      return rename(src, dst);
    }
    if(!S_ISDIR(dst_st.st_mode)) {
      if(!rel || !rel[0]) {
        errno = ENOTDIR;
        return -1;
      }
      if(archive_backup_move_existing(backup, dst, rel, err, err_size) != 0) {
        return -1;
      }
      return rename(src, dst);
    }

    DIR *d = opendir(src);
    if(!d) return -1;
    struct dirent *ent;
    int rc = 0;
    while((ent = readdir(d))) {
      char child_src[1024], child_dst[1024];
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      if(strlen(src) + strlen(ent->d_name) + 2 >= sizeof(child_src) ||
         strlen(dst) + strlen(ent->d_name) + 2 >= sizeof(child_dst)) {
        errno = ENAMETOOLONG;
        rc = -1;
        break;
      }
      join_path(child_src, sizeof(child_src), src, ent->d_name);
      join_path(child_dst, sizeof(child_dst), dst, ent->d_name);
      char child_rel[ZIP_NAME_MAX];
      if(rar_join_rel(child_rel, sizeof(child_rel), rel ? rel : "",
                      ent->d_name) != 0 ||
         archive_merge_move_tree(child_src, child_dst, child_rel,
                                 backup, err, err_size) != 0) {
        rc = -1;
        break;
      }
    }
    closedir(d);
    if(rc == 0 && rmdir(src) != 0) rc = -1;
    return rc;
  }

  if(S_ISREG(st.st_mode)) {
    struct stat dst_st;
    if(lstat(dst, &dst_st) == 0) {
      if(!rel || !rel[0]) {
        errno = EINVAL;
        return -1;
      }
      if(archive_backup_move_existing(backup, dst, rel, err, err_size) != 0) {
        return -1;
      }
    } else if(errno == ENOENT) {
      if(backup && backup->enabled && rel && rel[0] &&
         archive_backup_record_added(backup, rel, 0, err, err_size) != 0) {
        return -1;
      }
    } else {
      return -1;
    }
    if(rename(src, dst) != 0) return -1;
    atomic_fetch_add(&g_job.done_files, 1);
    return 0;
  }

  errno = EINVAL;
  return -1;
}


int
rar_merge_move_tree(const char *src, const char *dst) {
  char err[160] = {0};
  return archive_merge_move_tree(src, dst, "", NULL, err, sizeof(err));
}


static int
archive_chmod_777_walk(const char *path, archive_cancel_cb_t cancel_cb,
                       void *cancel_opaque,
                       archive_progress_cb_t progress_cb,
                       void *progress_opaque, long *changed,
                       char *err, size_t err_size) {
  struct stat st;

  if(cancel_cb && cancel_cb(cancel_opaque)) {
    snprintf(err, err_size, "cancelled");
    errno = EINTR;
    return -1;
  }
  if(lstat(path, &st) != 0) {
    snprintf(err, err_size, "stat permissions target: %s", strerror(errno));
    return -1;
  }

  if(S_ISLNK(st.st_mode)) {
    return 0;
  }

  if(S_ISDIR(st.st_mode)) {
    DIR *d;
    if(chmod(path, 0777) != 0) {
      snprintf(err, err_size, "chmod: %s", strerror(errno));
      return -1;
    }
    if(changed) (*changed)++;
    if(progress_cb) progress_cb(progress_opaque, path);

    d = opendir(path);
    if(!d) {
      snprintf(err, err_size, "open permissions target: %s", strerror(errno));
      return -1;
    }
    int rc = 0;
    struct dirent *ent;
    while((ent = readdir(d))) {
      char child[1024];
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      if(strlen(path) + strlen(ent->d_name) + 2 >= sizeof(child)) {
        snprintf(err, err_size, "permissions path too long");
        rc = -1;
        break;
      }
      join_path(child, sizeof(child), path, ent->d_name);
      if(archive_chmod_777_walk(child, cancel_cb, cancel_opaque,
                                progress_cb, progress_opaque, changed,
                                err, err_size) != 0) {
        rc = -1;
        break;
      }
    }
    closedir(d);
    return rc;
  }

  if(S_ISREG(st.st_mode)) {
    if(chmod(path, 0777) != 0) {
      snprintf(err, err_size, "chmod: %s", strerror(errno));
      return -1;
    }
    if(changed) (*changed)++;
    if(progress_cb) progress_cb(progress_opaque, path);
  }

  return 0;
}


int
archive_chmod_777_recursive(const char *path, archive_cancel_cb_t cancel_cb,
                            void *cancel_opaque,
                            archive_progress_cb_t progress_cb,
                            void *progress_opaque, long *changed,
                            char *err, size_t err_size) {
  if(changed) *changed = 0;
  if(!path || !*path) {
    snprintf(err, err_size, "bad permissions target");
    errno = EINVAL;
    return -1;
  }
  return archive_chmod_777_walk(path, cancel_cb, cancel_opaque, progress_cb,
                                progress_opaque, changed, err, err_size);
}


static int
archive_job_cancel_cb(void *opaque) {
  (void)opaque;
  return job_cancelled();
}

static int
archive_archive_folder_name(const char *archive_name, char *out,
                            size_t out_size) {
  const char *base = path_basename(archive_name ? archive_name : "");
  size_t n = strlen(base);
  if(n > 12 && !strncasecmp(base + n - 12, ".part001.rar", 12)) {
    n -= 12;
  } else if(n > 4 &&
            (!strcasecmp(base + n - 4, ".zip") ||
             !strcasecmp(base + n - 4, ".rar"))) {
    n -= 4;
  }
  if(n == 0 || n >= out_size) {
    errno = n == 0 ? EINVAL : ENAMETOOLONG;
    return -1;
  }
  size_t pos = 0;
  for(size_t i = 0; i < n && pos + 1 < out_size; i++) {
    unsigned char ch = (unsigned char)base[i];
    if(isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
      out[pos++] = (char)ch;
    } else {
      out[pos++] = '_';
    }
  }
  while(pos > 0 && out[pos - 1] == '.') pos--;
  if(pos == 0) {
    snprintf(out, out_size, "archive");
  } else {
    out[pos] = 0;
  }
  if(!upload_segment_safe(out)) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}


int
archive_place_stage(const char *stage, const char *archive_name,
                    const char *dest, char *final_base,
                    size_t final_base_size, long *files,
                    char *err, size_t err_size) {
  char dest_folder[256];
  archive_backup_ctx_t backup;
  int backup_started = 0;
  int rc = -1;
  struct stat dst_st;
  struct stat stage_st;
  du_state_t du = {0};
  memset(&backup, 0, sizeof(backup));

  job_set_current("Placing extracted files");
  if(archive_archive_folder_name(archive_name, dest_folder,
                                 sizeof(dest_folder)) != 0) {
    snprintf(err, err_size, "archive folder name: %s", strerror(errno));
    goto done;
  }

  if(lstat(stage, &stage_st) != 0 || !S_ISDIR(stage_st.st_mode)) {
    snprintf(err, err_size, "archive stage: %s", strerror(errno));
    goto done;
  }
  du.root_dev = stage_st.st_dev;
  du_walk(stage, &du);
  if(du.files <= 0) {
    snprintf(err, err_size, "archive contained no files");
    goto done;
  }
  atomic_store(&g_job.done_files, 0);
  atomic_store(&g_job.total_files,
               du.files > (uint64_t)INT_MAX ? INT_MAX : (int)du.files);

  if(strlen(dest) + strlen(dest_folder) + 2 >= final_base_size) {
    snprintf(err, err_size, "archive destination path too long");
    return -1;
  }
  join_path(final_base, final_base_size, dest, dest_folder);
  job_set_target(final_base);

  if(lstat(final_base, &dst_st) == 0) {
    if(!S_ISDIR(dst_st.st_mode)) {
      snprintf(err, err_size, "archive destination exists and is not a folder");
      goto done;
    }
    if(archive_backup_begin(&backup, final_base, archive_name,
                            err, err_size) != 0) {
      goto done;
    }
    backup_started = 1;
  } else if(errno != ENOENT) {
    snprintf(err, err_size, "archive destination: %s", strerror(errno));
    goto done;
  }

  if(archive_merge_move_tree(stage, final_base, "", &backup,
                             err, err_size) != 0) {
    if(!err[0]) {
      snprintf(err, err_size, "place archive output: %s",
               job_cancelled() ? "cancelled" : strerror(errno));
    }
    goto done;
  }

  if(files) *files = (long)(du.files > (uint64_t)LONG_MAX
                            ? LONG_MAX : du.files);
  if(backup_started) {
    archive_backup_close(&backup, 1);
    backup_started = 0;
  }
  rc = 0;

done:
  if(backup_started) archive_backup_close(&backup, rc == 0);
  if(rc != 0 && !err[0]) {
    snprintf(err, err_size, "place archive output: %s",
             job_cancelled() ? "cancelled" : strerror(errno));
  }
  return rc;
}

static int
archive_title_id_safe(const char *title) {
  if(!title || !*title || strlen(title) >= 64) return 0;
  for(const unsigned char *p = (const unsigned char *)title; *p; p++) {
    if(!isalnum(*p) && *p != '_' && *p != '-') return 0;
  }
  return 1;
}


static int
archive_read_file_limited(const char *path, char **out, size_t max_size) {
  FILE *f = fopen(path, "rb");
  if(!f) return -1;
  if(fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return -1;
  }
  long end = ftell(f);
  if(end < 0 || (size_t)end > max_size) {
    fclose(f);
    errno = EFBIG;
    return -1;
  }
  if(fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return -1;
  }
  char *buf = malloc((size_t)end + 1);
  if(!buf) {
    fclose(f);
    errno = ENOMEM;
    return -1;
  }
  size_t n = fread(buf, 1, (size_t)end, f);
  int ferr = ferror(f);
  fclose(f);
  if(ferr) {
    free(buf);
    errno = EIO;
    return -1;
  }
  buf[n] = 0;
  *out = buf;
  return 0;
}


static int
archive_json_find_string(const char *json, const char *key,
                         char *out, size_t out_size) {
  char needle[80];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char *p = strstr(json, needle);
  if(!p) return 0;
  p += strlen(needle);
  while(*p && isspace((unsigned char)*p)) p++;
  if(*p != ':') return 0;
  p++;
  while(*p && isspace((unsigned char)*p)) p++;
  if(*p != '"') return 0;
  p++;
  size_t pos = 0;
  while(*p && *p != '"' && pos + 1 < out_size) {
    if(*p == '\\') return 0;
    out[pos++] = *p++;
  }
  if(*p != '"' || pos == 0) return 0;
  out[pos] = 0;
  return 1;
}


static int
archive_app_read_title_id(const char *app_path, char *title, size_t title_size) {
  char sce_sys[1024];
  char param[1024];
  char *json = NULL;
  int ok;
  if(strlen(app_path) + strlen("sce_sys/param.json") + 3 >= sizeof(param)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  join_path(sce_sys, sizeof(sce_sys), app_path, "sce_sys");
  join_path(param, sizeof(param), sce_sys, "param.json");
  if(archive_read_file_limited(param, &json, 128 * 1024) != 0) return -1;
  ok = archive_json_find_string(json, "titleId", title, title_size) ||
       archive_json_find_string(json, "title_id", title, title_size);
  free(json);
  if(!ok || !archive_title_id_safe(title)) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}


static int
archive_app_has_boot_file(const char *app_path) {
  char path[1024];
  struct stat st;
  if(strlen(app_path) + strlen("eboot.bin") + 2 < sizeof(path)) {
    join_path(path, sizeof(path), app_path, "eboot.bin");
    if(stat(path, &st) == 0 && S_ISREG(st.st_mode)) return 1;
  }
  if(strlen(app_path) + strlen("iboot.bin") + 2 < sizeof(path)) {
    join_path(path, sizeof(path), app_path, "iboot.bin");
    if(stat(path, &st) == 0 && S_ISREG(st.st_mode)) return 1;
  }
  return 0;
}


static int
archive_title_from_text(const char *text, char *out, size_t out_size) {
  if(!text || out_size < 10) return 0;
  for(const char *p = text; *p; p++) {
    if(strlen(p) < 9) break;
    if(toupper((unsigned char)p[0]) != 'P' ||
       toupper((unsigned char)p[1]) != 'P' ||
       toupper((unsigned char)p[2]) != 'S' ||
       toupper((unsigned char)p[3]) != 'A') {
      continue;
    }
    int ok = 1;
    for(int i = 4; i < 9; i++) {
      if(!isalnum((unsigned char)p[i])) {
        ok = 0;
        break;
      }
    }
    if(!ok) continue;
    if((p > text && isalnum((unsigned char)p[-1])) ||
       isalnum((unsigned char)p[9])) {
      continue;
    }
    for(int i = 0; i < 9; i++) {
      out[i] = (char)toupper((unsigned char)p[i]);
    }
    out[9] = 0;
    return archive_title_id_safe(out);
  }
  return 0;
}


static int
archive_path_same_or_child(const char *root, const char *path) {
  size_t n;
  if(!root || !path) return 0;
  n = strlen(root);
  while(n > 1 && root[n - 1] == '/') n--;
  return strncmp(root, path, n) == 0 && (path[n] == 0 || path[n] == '/');
}


static int
archive_path_depth(const char *path) {
  int depth = 0;
  int in_seg = 0;
  for(const char *p = path; p && *p; p++) {
    if(*p == '/') {
      in_seg = 0;
    } else if(!in_seg) {
      depth++;
      in_seg = 1;
    }
  }
  return depth;
}


static int
archive_title_app_path(const char *root, const char *title,
                       char *out, size_t out_size) {
  char expected[128];
  int n;
  if(snprintf(expected, sizeof(expected), "%s-app", title) >=
     (int)sizeof(expected)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  n = snprintf(out, out_size, "%s%s%s",
               root, root[1] ? "/" : "", expected);
  if(n < 0 || (size_t)n >= out_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}


static int
archive_app_dir_candidate(const char *path, char *title, size_t title_size) {
  struct stat st;
  if(lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
  if(!archive_app_has_boot_file(path)) return 0;
  return archive_app_read_title_id(path, title, title_size) == 0;
}


static int
archive_find_context_app_target(const char *scan_path, char *target,
                                size_t target_size, char *title,
                                size_t title_size) {
  char cursor[1024];
  char parent[1024];
  char base[256];
  char found_title[64];

  if(!scan_path || !*scan_path || !target || target_size == 0 ||
     !title || title_size == 0) {
    return 0;
  }
  snprintf(cursor, sizeof(cursor), "%s", scan_path);
  for(;;) {
    if(archive_app_dir_candidate(cursor, found_title, sizeof(found_title))) {
      snprintf(target, target_size, "%s", cursor);
      snprintf(title, title_size, "%s", found_title);
      return 1;
    }
    if(!strcmp(cursor, "/")) break;
    if(archive_parent_path(cursor, parent, sizeof(parent),
                           base, sizeof(base)) != 0) {
      break;
    }
    if(!strcmp(parent, cursor)) break;
    snprintf(cursor, sizeof(cursor), "%s", parent);
  }
  return 0;
}


static int
archive_homebrew_app_target_exists(const char *title) {
  char target[1024];
  struct stat st;
  if(!title || !*title) return 0;
  if(archive_title_app_path("/data/homebrew", title, target,
                            sizeof(target)) != 0) {
    return 0;
  }
  return lstat(target, &st) == 0 && S_ISDIR(st.st_mode);
}


static int
archive_app_patch_dir_candidate(const char *scan_path, const char *path,
                                char *title, size_t title_size) {
  struct stat st;
  char tmp[64];
  char context_target[1024];
  char context_title[64];
  if(lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
  if(!archive_app_has_boot_file(path)) return 0;
  if(archive_app_read_title_id(path, tmp, sizeof(tmp)) == 0) return 0;
  if(archive_find_context_app_target(scan_path, context_target,
                                     sizeof(context_target), context_title,
                                     sizeof(context_title)) &&
     strcmp(path, context_target) != 0 &&
     archive_path_same_or_child(context_target, path)) {
    snprintf(title, title_size, "%s", context_title);
    return 1;
  }
  if(!archive_title_from_text(path, title, title_size) &&
     !archive_title_from_text(scan_path, title, title_size)) {
    return 0;
  }
  return archive_homebrew_app_target_exists(title);
}


static int
archive_app_named_path(const char *path, const char *title) {
  char parent[1024];
  char base[256];
  char expected[128];
  if(!title || !*title) return 0;
  if(archive_parent_path(path, parent, sizeof(parent), base, sizeof(base)) != 0) {
    return 0;
  }
  if(snprintf(expected, sizeof(expected), "%s-app", title) >=
     (int)sizeof(expected)) {
    return 0;
  }
  return !strcasecmp(base, expected);
}


static int
archive_app_candidate_better(const char *scan_path, const char *current,
                             const char *current_title, int current_patch,
                             const char *next, const char *next_title,
                             int next_patch) {
  if(!current[0]) return 1;
  if(next_patch != current_patch) return !next_patch;
  int next_is_root = strcmp(scan_path, next) == 0;
  int current_is_root = strcmp(scan_path, current) == 0;
  if(next_is_root != current_is_root) return next_is_root;

  int next_named = archive_app_named_path(next, next_title);
  int current_named = archive_app_named_path(current, current_title);
  if(next_named != current_named) return next_named;

  int next_depth = rar_rel_depth(next + strlen(scan_path));
  int current_depth = rar_rel_depth(current + strlen(scan_path));
  if(next_depth != current_depth) return next_depth < current_depth;
  return strcmp(next, current) < 0;
}


static int
archive_app_find_candidate(const char *scan_path, const char *path,
                           archive_app_prepare_info_t *info,
                           char *err, size_t err_size) {
  char title[64];
  int patch_mode = 0;
  if(archive_app_dir_candidate(path, title, sizeof(title))) {
    patch_mode = 0;
  } else if(archive_app_patch_dir_candidate(scan_path, path,
                                            title, sizeof(title))) {
    patch_mode = 1;
  } else {
    title[0] = 0;
  }
  if(title[0]) {
    if(archive_app_candidate_better(scan_path, info->app_path,
                                    info->title_id, info->patch_mode,
                                    path, title, patch_mode)) {
      info->found = 1;
      info->patch_mode = patch_mode;
      snprintf(info->title_id, sizeof(info->title_id), "%s", title);
      snprintf(info->app_path, sizeof(info->app_path), "%s", path);
    }
  }

  DIR *d = opendir(path);
  if(!d) return 0;
  struct dirent *ent;
  while((ent = readdir(d))) {
    char child[1024];
    struct stat st;
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    if(!rar_segment_safe(ent->d_name)) {
      snprintf(err, err_size, "app scan found unsafe path");
      closedir(d);
      return -1;
    }
    if(strlen(path) + strlen(ent->d_name) + 2 >= sizeof(child)) {
      snprintf(err, err_size, "app scan path too long");
      closedir(d);
      return -1;
    }
    join_path(child, sizeof(child), path, ent->d_name);
    if(lstat(child, &st) != 0) {
      snprintf(err, err_size, "app scan stat: %s", strerror(errno));
      closedir(d);
      return -1;
    }
    if(S_ISLNK(st.st_mode)) continue;
    if(S_ISDIR(st.st_mode) &&
       archive_app_find_candidate(scan_path, child, info,
                                  err, err_size) != 0) {
      closedir(d);
      return -1;
    }
  }
  closedir(d);
  return 0;
}


static int
archive_tree_permissions_777(const char *path, int *ok,
                             char *err, size_t err_size) {
  struct stat st;
  if(lstat(path, &st) != 0) {
    snprintf(err, err_size, "stat permissions: %s", strerror(errno));
    return -1;
  }
  if(S_ISLNK(st.st_mode)) return 0;
  if((st.st_mode & 0777) != 0777) {
    *ok = 0;
    return 0;
  }
  if(!S_ISDIR(st.st_mode)) return 0;

  DIR *d = opendir(path);
  if(!d) {
    snprintf(err, err_size, "open permissions: %s", strerror(errno));
    return -1;
  }
  struct dirent *ent;
  int rc = 0;
  while(*ok && (ent = readdir(d))) {
    char child[1024];
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    if(strlen(path) + strlen(ent->d_name) + 2 >= sizeof(child)) {
      snprintf(err, err_size, "permissions path too long");
      rc = -1;
      break;
    }
    join_path(child, sizeof(child), path, ent->d_name);
    if(archive_tree_permissions_777(child, ok, err, err_size) != 0) {
      rc = -1;
      break;
    }
  }
  closedir(d);
  return rc;
}


static int
archive_app_resolve_prepared_path(const char *scan_path, const char *app_path,
                                  const char *title, int patch_mode,
                                  char *out, size_t out_size,
                                  int *target_exists) {
  char parent[1024], base[256], target[1024];
  char context_title[64];
  struct stat st;

  if(target_exists) *target_exists = 0;

  if(patch_mode &&
     archive_find_context_app_target(scan_path, target, sizeof(target),
                                     context_title, sizeof(context_title)) &&
     strcmp(app_path, target) != 0 &&
     archive_path_same_or_child(target, app_path)) {
    snprintf(out, out_size, "%s", target);
    if(target_exists) *target_exists = 1;
    return 0;
  }

  if(archive_title_app_path("/data/homebrew", title, target, sizeof(target)) == 0 &&
     strcmp(app_path, target) == 0) {
    snprintf(out, out_size, "%s", target);
    if(target_exists) *target_exists = 1;
    return 0;
  }

  if(archive_title_app_path("/data/homebrew", title, target, sizeof(target)) == 0 &&
     lstat(target, &st) == 0 && S_ISDIR(st.st_mode)) {
    snprintf(out, out_size, "%s", target);
    if(target_exists) *target_exists = 1;
    return 0;
  }

  if(archive_parent_path(scan_path, parent, sizeof(parent),
                         base, sizeof(base)) == 0 &&
     archive_title_app_path(parent, title, target, sizeof(target)) == 0 &&
     lstat(target, &st) == 0 && S_ISDIR(st.st_mode)) {
    snprintf(out, out_size, "%s", target);
    if(target_exists) *target_exists = 1;
    return 0;
  }

  if(lstat("/data/homebrew", &st) == 0 && S_ISDIR(st.st_mode)) {
    if(archive_title_app_path("/data/homebrew", title, out, out_size) != 0) {
      return -1;
    }
  } else if(!patch_mode && archive_app_named_path(app_path, title)) {
    snprintf(out, out_size, "%s", app_path);
    if(target_exists) *target_exists = 1;
    return 0;
  } else if(archive_parent_path(scan_path, parent, sizeof(parent),
                                base, sizeof(base)) == 0) {
    if(archive_title_app_path(parent, title, out, out_size) != 0) return -1;
  } else {
    errno = EINVAL;
    return -1;
  }

  if(target_exists && lstat(out, &st) == 0) *target_exists = 1;
  return 0;
}


static int
archive_prepare_delete_scan_safe(const archive_app_prepare_info_t *info) {
  if(!info || !info->scan_path[0] || !info->app_path[0] ||
     !info->prepared_path[0]) {
    return 0;
  }
  if(strcmp(info->scan_path, info->app_path) == 0 ||
     strcmp(info->scan_path, info->prepared_path) == 0) {
    return 0;
  }
  if(!archive_path_same_or_child(info->scan_path, info->app_path)) return 0;
  if(archive_path_same_or_child(info->scan_path, info->prepared_path)) return 0;
  return archive_path_depth(info->scan_path) >= 3;
}


static void
archive_prepare_set_delete_sample(archive_app_prepare_info_t *info,
                                  const char *path, int is_dir,
                                  int *sample_is_dir) {
  char rel[512];
  if(archive_rel_from_path(info->scan_path, path, rel, sizeof(rel)) != 0) {
    return;
  }
  if(!info->delete_extra_sample[0] ||
     (!is_dir && sample_is_dir && *sample_is_dir)) {
    snprintf(info->delete_extra_sample,
             sizeof(info->delete_extra_sample), "%s", rel);
    if(sample_is_dir) *sample_is_dir = is_dir;
  }
}


static int
archive_prepare_count_extras_walk(archive_app_prepare_info_t *info,
                                  const char *path, int *sample_is_dir,
                                  char *err, size_t err_size) {
  DIR *d = opendir(path);
  if(!d) {
    snprintf(err, err_size, "scan cleanup files: %s", strerror(errno));
    return -1;
  }

  struct dirent *ent;
  int rc = 0;
  while((ent = readdir(d))) {
    char child[1024];
    struct stat st;
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    if(strlen(path) + strlen(ent->d_name) + 2 >= sizeof(child)) {
      snprintf(err, err_size, "cleanup path too long");
      rc = -1;
      break;
    }
    join_path(child, sizeof(child), path, ent->d_name);
    if(lstat(child, &st) != 0) {
      snprintf(err, err_size, "scan cleanup file: %s", strerror(errno));
      rc = -1;
      break;
    }

    if(archive_path_same_or_child(info->app_path, child)) {
      continue;
    }
    if(S_ISDIR(st.st_mode) &&
       archive_path_same_or_child(child, info->app_path)) {
      if(archive_prepare_count_extras_walk(info, child, sample_is_dir,
                                           err, err_size) != 0) {
        rc = -1;
        break;
      }
      continue;
    }

    if(S_ISDIR(st.st_mode)) {
      info->delete_extra_dirs++;
      archive_prepare_set_delete_sample(info, child, 1, sample_is_dir);
      if(archive_prepare_count_extras_walk(info, child, sample_is_dir,
                                           err, err_size) != 0) {
        rc = -1;
        break;
      }
    } else {
      info->delete_extra_files++;
      archive_prepare_set_delete_sample(info, child, 0, sample_is_dir);
    }
  }
  closedir(d);
  return rc;
}


static int
archive_prepare_count_extras(archive_app_prepare_info_t *info,
                             char *err, size_t err_size) {
  int sample_is_dir = 0;
  if(!archive_prepare_delete_scan_safe(info)) return 0;
  return archive_prepare_count_extras_walk(info, info->scan_path,
                                           &sample_is_dir, err, err_size);
}


int
archive_app_prepare_probe(const char *path, archive_app_prepare_info_t *info,
                          char *err, size_t err_size) {
  char scan[1024];
  struct stat st;
  archive_app_prepare_info_t local;
  if(!info) info = &local;
  memset(info, 0, sizeof(*info));

  if(!path || !path_is_safe(path)) {
    snprintf(err, err_size, "bad path");
    errno = EINVAL;
    return -1;
  }
  snprintf(scan, sizeof(scan), "%s", path);
  size_t n = strlen(scan);
  while(n > 1 && scan[n - 1] == '/') scan[--n] = 0;
  if(lstat(scan, &st) != 0 || !S_ISDIR(st.st_mode)) {
    snprintf(err, err_size, "prepare target is not a folder");
    return -1;
  }
  snprintf(info->scan_path, sizeof(info->scan_path), "%s", scan);
  if(archive_app_find_candidate(scan, scan, info, err, err_size) != 0) {
    return -1;
  }
  if(!info->found) {
    snprintf(info->reason, sizeof(info->reason), "no PS5 app found");
    return 0;
  }

  if(archive_app_resolve_prepared_path(scan, info->app_path, info->title_id,
                                       info->patch_mode,
                                       info->prepared_path,
                                       sizeof(info->prepared_path),
                                       &info->target_exists) != 0) {
    snprintf(err, err_size, "prepared path too long");
    return -1;
  }
  info->path_ok = strcmp(info->app_path, info->prepared_path) == 0;
  info->merge_required = info->target_exists && !info->path_ok;
  info->permissions_ok = 1;
  if(!info->patch_mode) {
    const char *perm_path = info->target_exists ? info->prepared_path
                           : info->app_path;
    if(archive_tree_permissions_777(perm_path, &info->permissions_ok,
                                    err, err_size) != 0) {
      return -1;
    }
  }
  if(archive_prepare_count_extras(info, err, err_size) != 0) {
    return -1;
  }
  info->prepared = !info->patch_mode && !info->merge_required &&
                   info->path_ok && info->permissions_ok &&
                   info->delete_extra_files == 0 &&
                   info->delete_extra_dirs == 0;
  if(info->prepared) {
    snprintf(info->reason, sizeof(info->reason), "already prepared");
  } else if(info->patch_mode && info->target_exists) {
    snprintf(info->reason, sizeof(info->reason),
             "patch will overwrite matching files");
  } else if(info->patch_mode && info->merge_required &&
            (info->delete_extra_files || info->delete_extra_dirs)) {
    snprintf(info->reason, sizeof(info->reason),
             "patch will overwrite matching files and remove extra extracted files");
  } else if(info->patch_mode && info->merge_required) {
    snprintf(info->reason, sizeof(info->reason),
             "patch will overwrite matching files");
  } else if(info->patch_mode) {
    snprintf(info->reason, sizeof(info->reason),
             "needs app folder placement");
  } else if(info->merge_required &&
            (info->delete_extra_files || info->delete_extra_dirs)) {
    snprintf(info->reason, sizeof(info->reason),
             "needs merge into existing app and cleanup");
  } else if(info->merge_required) {
    snprintf(info->reason, sizeof(info->reason),
             "needs merge into existing app");
  } else if(!info->path_ok && !info->permissions_ok) {
    snprintf(info->reason, sizeof(info->reason),
             "needs app folder placement and permissions");
  } else if(!info->path_ok) {
    snprintf(info->reason, sizeof(info->reason),
             "needs app folder placement");
  } else {
    snprintf(info->reason, sizeof(info->reason), "needs permissions");
  }
  return 0;
}


int
archive_app_prepare_apply(const char *path, archive_app_prepare_info_t *info,
                          char *err, size_t err_size) {
  archive_app_prepare_info_t local;
  struct stat st;
  archive_backup_ctx_t backup;
  int backup_started = 0;
  int cleanup_extras = 0;
  if(!info) info = &local;
  memset(&backup, 0, sizeof(backup));
  if(archive_app_prepare_probe(path, info, err, err_size) != 0) return -1;
  if(!info->found) {
    snprintf(err, err_size, "no PS5 app found");
    errno = ENOENT;
    return -1;
  }

  job_set_target(info->prepared_path);
  cleanup_extras = archive_prepare_delete_scan_safe(info) &&
                   (info->patch_mode ||
                    info->delete_extra_files || info->delete_extra_dirs);
  if(info->patch_mode) {
    if(info->path_ok) {
      snprintf(err, err_size, "patch source is already the app target");
      errno = EINVAL;
      return -1;
    }
    if(lstat(info->prepared_path, &st) != 0) {
      snprintf(err, err_size, "patch target app does not exist");
      errno = ENOENT;
      return -1;
    }
    if(!S_ISDIR(st.st_mode)) {
      snprintf(err, err_size, "patch target is not a folder");
      errno = ENOTDIR;
      return -1;
    }
    if(archive_path_same_or_child(info->app_path, info->prepared_path)) {
      snprintf(err, err_size, "patch source and target overlap");
      errno = EINVAL;
      return -1;
    }

    job_set_current("Setting patch permissions");
    if(archive_chmod_777_recursive(info->app_path, archive_job_cancel_cb,
                                   NULL, NULL, NULL, NULL, err,
                                   err_size) != 0) {
      return -1;
    }

    job_set_current("Applying patch");
    if(archive_backup_begin(&backup, info->prepared_path,
                            path_basename(info->scan_path),
                            err, err_size) != 0) {
      return -1;
    }
    backup_started = 1;
    if(archive_merge_move_tree(info->app_path, info->prepared_path, "",
                               &backup, err, err_size) != 0) {
      if(!err[0]) {
        snprintf(err, err_size, "apply patch: %s",
                 job_cancelled() ? "cancelled" : strerror(errno));
      }
      archive_backup_close(&backup, 0);
      return -1;
    }
    archive_backup_close(&backup, 1);
    backup_started = 0;
    snprintf(info->app_path, sizeof(info->app_path), "%s",
             info->prepared_path);
    info->path_ok = 1;
    info->permissions_ok = 1;
    info->target_exists = 1;
    info->merge_required = 0;
  }
  if(!info->path_ok) {
    job_set_current("Placing app folder");
    if(lstat(info->prepared_path, &st) == 0) {
      if(!S_ISDIR(st.st_mode)) {
        snprintf(err, err_size, "prepared app target is not a folder");
        errno = ENOTDIR;
        return -1;
      }
      if(archive_backup_begin(&backup, info->prepared_path,
                              path_basename(info->scan_path),
                              err, err_size) != 0) {
        return -1;
      }
      backup_started = 1;
      if(archive_merge_move_tree(info->app_path, info->prepared_path, "",
                                 &backup, err, err_size) != 0) {
        if(!err[0]) {
          snprintf(err, err_size, "merge app folder: %s",
                   job_cancelled() ? "cancelled" : strerror(errno));
        }
        archive_backup_close(&backup, 0);
        return -1;
      }
      archive_backup_close(&backup, 1);
      backup_started = 0;
    }
    else {
      if(errno != ENOENT) {
        snprintf(err, err_size, "prepared path: %s", strerror(errno));
        return -1;
      }
      if(archive_mkdir_parent(info->prepared_path) != 0) {
        snprintf(err, err_size, "prepared parent: %s", strerror(errno));
        return -1;
      }
      if(rename(info->app_path, info->prepared_path) != 0) {
        snprintf(err, err_size, "place app folder: %s", strerror(errno));
        return -1;
      }
    }
    snprintf(info->app_path, sizeof(info->app_path), "%s", info->prepared_path);
    info->path_ok = 1;
  }

  if(!info->patch_mode) {
    job_set_current("Setting permissions");
    if(archive_chmod_777_recursive(info->prepared_path, archive_job_cancel_cb,
                                   NULL, NULL, NULL, NULL, err,
                                   err_size) != 0) {
      return -1;
    }
  }
  if(cleanup_extras) {
    job_set_current("Cleaning extracted files");
    if(rar_delete_tree(info->scan_path) != 0) {
      snprintf(err, err_size, "cleanup extracted files: %s",
               strerror(errno));
      return -1;
    }
  }
  if(backup_started) archive_backup_close(&backup, 0);
  if(info->patch_mode) {
    info->found = 1;
    info->prepared = 1;
    info->permissions_ok = 1;
    info->path_ok = 1;
    info->target_exists = 1;
    info->merge_required = 0;
    info->delete_extra_files = 0;
    info->delete_extra_dirs = 0;
    info->delete_extra_sample[0] = 0;
    snprintf(info->reason, sizeof(info->reason), "patch applied");
    return 0;
  }
  return archive_app_prepare_probe(info->prepared_path, info, err, err_size);
}


int
archive_ext_is_zip(const char *path) {
  const char *base = path_basename(path);
  size_t n = strlen(base);
  return n > 4 && !strcasecmp(base + n - 4, ".zip");
}


int
archive_ext_is_rar_first_volume(const char *path) {
  const char *base = path_basename(path);
  size_t n = strlen(base);
  if(n > 4 && !strcasecmp(base + n - 4, ".rar")) {
    if(n > 12 && !strncasecmp(base + n - 12, ".part", 5)) {
      return !strncasecmp(base + n - 12, ".part001.rar", 12);
    }
    return 1;
  }
  return 0;
}
