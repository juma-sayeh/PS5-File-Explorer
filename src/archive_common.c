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


static int
rar_app_folder_from_segment(const char *seg, char *out, size_t out_size) {
  if(strlen(seg) != 13) return 0;
  if(strncasecmp(seg, "PPSA", 4) != 0 || strcasecmp(seg + 9, "-app") != 0) {
    return 0;
  }
  for(int i = 4; i < 9; i++) {
    if(!isdigit((unsigned char)seg[i]) &&
       (toupper((unsigned char)seg[i]) < 'A' ||
        toupper((unsigned char)seg[i]) > 'Z')) {
      return 0;
    }
  }
  if(out_size < 14) return 0;
  snprintf(out, out_size, "PPSA%c%c%c%c%c-app",
           toupper((unsigned char)seg[4]), toupper((unsigned char)seg[5]),
           toupper((unsigned char)seg[6]), toupper((unsigned char)seg[7]),
           toupper((unsigned char)seg[8]));
  return 1;
}


static int
rar_rel_after_prefix_is_eboot(const char *rel, const char *prefix) {
  size_t n = strlen(prefix);
  if(strncmp(rel, prefix, n) != 0) return 0;
  if(rel[n] == '/') n++;
  else if(rel[n] != 0) return 0;
  return !strcasecmp(rel + n, "eboot.bin");
}


static int
rar_layout_better(const rar_layout_choice_t *l, const char *prefix,
                  int depth, int has_eboot) {
  if(!l->app_prefix[0]) return 1;
  if(has_eboot != l->app_has_eboot) return has_eboot;
  if(depth != l->app_depth) return depth < l->app_depth;
  return strcmp(prefix, l->app_prefix) < 0;
}


static void
rar_consider_file_layout(rar_layout_choice_t *layout, const char *rel) {
  char tmp[ZIP_NAME_MAX];
  char prefix[ZIP_NAME_MAX] = {0};
  size_t prefix_len = 0;
  char *seg;

  snprintf(tmp, sizeof(tmp), "%s", rel);
  seg = tmp;
  while(seg && *seg) {
    char *slash = strchr(seg, '/');
    if(slash) *slash = 0;
    if(*seg) {
      char app_folder[256];
      size_t seg_len = strlen(seg);
      if(prefix_len + (prefix_len ? 1 : 0) + seg_len + 1 < sizeof(prefix)) {
        if(prefix_len) prefix[prefix_len++] = '/';
        memcpy(prefix + prefix_len, seg, seg_len + 1);
        prefix_len += seg_len;
      }
      if(rar_app_folder_from_segment(seg, app_folder, sizeof(app_folder))) {
        int has_eboot = rar_rel_after_prefix_is_eboot(rel, prefix);
        int depth = rar_rel_depth(prefix);
        if(rar_layout_better(layout, prefix, depth, has_eboot) ||
           !strcmp(layout->app_prefix, prefix)) {
          snprintf(layout->app_prefix, sizeof(layout->app_prefix), "%s", prefix);
          snprintf(layout->dest_folder, sizeof(layout->dest_folder), "%s",
                   app_folder);
          if(has_eboot) layout->app_has_eboot = 1;
          layout->app_depth = depth;
        }
      }
    }
    if(!slash) break;
    seg = slash + 1;
  }

  const char *base = strrchr(rel, '/');
  base = base ? base + 1 : rel;
  if(!strcasecmp(base, "eboot.bin")) {
    char parent[ZIP_NAME_MAX] = {0};
    int depth;
    size_t parent_len = (size_t)(base - rel);
    if(parent_len > 0 && rel[parent_len - 1] == '/') parent_len--;
    if(parent_len >= sizeof(parent)) return;
    memcpy(parent, rel, parent_len);
    parent[parent_len] = 0;
    depth = rar_rel_depth(parent);
    if(!layout->eboot_parent[0] ||
       depth < layout->eboot_depth ||
       (depth == layout->eboot_depth &&
        strcmp(parent, layout->eboot_parent) < 0)) {
      snprintf(layout->eboot_parent, sizeof(layout->eboot_parent), "%s",
               parent);
      layout->eboot_depth = depth;
    }
  }
}


static int
rar_scan_stage(const char *root, const char *rel, rar_layout_choice_t *layout,
               char *err, size_t err_size) {
  char path[1024];
  DIR *d;
  struct dirent *ent;

  if(rel && rel[0]) {
    if(strlen(root) + strlen(rel) + 2 >= sizeof(path)) {
      snprintf(err, err_size, "rar path too long");
      return -1;
    }
    join_path(path, sizeof(path), root, rel);
  } else {
    snprintf(path, sizeof(path), "%s", root);
  }

  d = opendir(path);
  if(!d) {
    snprintf(err, err_size, "scan rar output: %s", strerror(errno));
    return -1;
  }

  while((ent = readdir(d))) {
    char child_rel[ZIP_NAME_MAX];
    char child_path[1024];
    struct stat st;

    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    if(!rar_segment_safe(ent->d_name) ||
       rar_join_rel(child_rel, sizeof(child_rel), rel ? rel : "",
                    ent->d_name) != 0) {
      snprintf(err, err_size, "rar contains an unsafe path");
      closedir(d);
      return -1;
    }
    if(strlen(root) + strlen(child_rel) + 2 >= sizeof(child_path)) {
      snprintf(err, err_size, "rar path too long");
      closedir(d);
      return -1;
    }
    join_path(child_path, sizeof(child_path), root, child_rel);
    if(lstat(child_path, &st) != 0) {
      snprintf(err, err_size, "stat rar output: %s", strerror(errno));
      closedir(d);
      return -1;
    }
    if(S_ISLNK(st.st_mode)) {
      snprintf(err, err_size, "rar links are not supported");
      closedir(d);
      return -1;
    }
    if(S_ISDIR(st.st_mode)) {
      if(rar_scan_stage(root, child_rel, layout, err, err_size) != 0) {
        closedir(d);
        return -1;
      }
    } else if(S_ISREG(st.st_mode)) {
      layout->files++;
      rar_consider_file_layout(layout, child_rel);
    }
  }

  closedir(d);
  return 0;
}


static int
rar_title_from_name(const char *name, char *out, size_t out_size) {
  for(const char *p = name ? name : ""; *p; p++) {
    if(strlen(p) < 9) break;
    if(strncasecmp(p, "PPSA", 4) != 0) continue;
    int ok = 1;
    for(int i = 4; i < 9; i++) {
      if(!isalnum((unsigned char)p[i])) {
        ok = 0;
        break;
      }
    }
    if(ok) {
      if(out_size < 10) return 0;
      snprintf(out, out_size, "PPSA%c%c%c%c%c",
               toupper((unsigned char)p[4]), toupper((unsigned char)p[5]),
               toupper((unsigned char)p[6]), toupper((unsigned char)p[7]),
               toupper((unsigned char)p[8]));
      return 1;
    }
  }
  return 0;
}


int
rar_choose_layout(const char *stage, const char *archive_name,
                  rar_layout_choice_t *layout, char *err, size_t err_size) {
  char title[16];
  memset(layout, 0, sizeof(*layout));
  layout->app_depth = INT_MAX;
  layout->eboot_depth = INT_MAX;

  if(rar_scan_stage(stage, "", layout, err, err_size) != 0) return -1;
  if(layout->files <= 0) {
    snprintf(err, err_size, "rar contained no files");
    return -1;
  }
  if(layout->app_prefix[0] && layout->app_has_eboot) return 0;

  if(layout->dest_folder[0]) {
    size_t n = strlen(layout->dest_folder);
    if(n > 4 && n - 4 < sizeof(title) &&
       !strcasecmp(layout->dest_folder + n - 4, "-app")) {
      memcpy(title, layout->dest_folder, n - 4);
      title[n - 4] = 0;
    } else {
      title[0] = 0;
    }
  } else if(!rar_title_from_name(archive_name, title, sizeof(title))) {
    title[0] = 0;
  }

  if(!title[0] && !rar_title_from_name(archive_name, title, sizeof(title))) {
    snprintf(err, err_size, "No PPSA title ID found in rar name or app folder");
    return -1;
  }
  if(!layout->eboot_parent[0] && layout->eboot_depth != 0) {
    snprintf(err, err_size, "No eboot.bin found to place inside %s-app",
             title);
    return -1;
  }
  snprintf(layout->dest_folder, sizeof(layout->dest_folder), "%s-app", title);
  snprintf(layout->app_prefix, sizeof(layout->app_prefix), "%s",
           layout->eboot_parent);
  return 0;
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


int
archive_place_stage(const char *stage, const char *archive_name,
                    const char *dest, char *final_base,
                    size_t final_base_size, long *files,
                    char *err, size_t err_size) {
  rar_layout_choice_t layout;
  char src_root[1024];
  archive_backup_ctx_t backup;
  int backup_started = 0;
  int rc = -1;
  struct stat dst_st;
  memset(&backup, 0, sizeof(backup));

  job_set_current("Placing extracted files");
  if(rar_choose_layout(stage, archive_name, &layout, err, err_size) != 0) {
    goto done;
  }

  atomic_store(&g_job.done_files, 0);
  atomic_store(&g_job.total_files,
               layout.files > INT_MAX ? INT_MAX : (int)layout.files);

  if(layout.app_prefix[0]) {
    if(strlen(stage) + strlen(layout.app_prefix) + 2 >= sizeof(src_root)) {
      snprintf(err, err_size, "archive path too long");
      return -1;
    }
    join_path(src_root, sizeof(src_root), stage, layout.app_prefix);
  } else {
    snprintf(src_root, sizeof(src_root), "%s", stage);
  }

  if(strlen(dest) + strlen(layout.dest_folder) + 2 >= final_base_size) {
    snprintf(err, err_size, "archive destination path too long");
    return -1;
  }
  join_path(final_base, final_base_size, dest, layout.dest_folder);
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

  if(archive_merge_move_tree(src_root, final_base, "", &backup,
                             err, err_size) != 0) {
    if(!err[0]) {
      snprintf(err, err_size, "place archive output: %s",
               job_cancelled() ? "cancelled" : strerror(errno));
    }
    goto done;
  }

  if(files) *files = layout.files;
  if(backup_started) {
    archive_backup_close(&backup, 1);
    backup_started = 0;
  }

  job_set_current("Setting permissions");
  if(archive_chmod_777_recursive(final_base, archive_job_cancel_cb, NULL,
                                 NULL, NULL, NULL, err, err_size) != 0) {
    goto done;
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
