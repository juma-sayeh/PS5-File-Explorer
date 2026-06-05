/*
 * File Explorer - file-manager copy/move/delete primitives and API.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "archive_common.h"
#include "pfs_compress.h"
#include "rar_transfer.h"
#include "transfer.h"
#include "transfer_internal.h"
#include "websrv.h"
#include "zip_archive.h"


#define COPY_BUF_SIZE   (1024 * 1024)
#define UPLOAD_BUF_SIZE (1024 * 1024)
#define UPLOAD_CHUNK_MAX (16 * 1024 * 1024)
#define OP_LOG_DIR "/data/FileExplorer/logs"
#define OP_LOG_READ_MAX (512 * 1024)
#define SHADOWMOUNT_AUTOTUNE_FILE "/data/shadowmount/autotune.ini"
#define ACTIVITY_MAX 128
#define ACTIVITY_LOG_LIMIT_DEFAULT 80
#define ACTIVITY_LOG_LIMIT_MAX 200
#define ACTIVITY_HEARTBEAT_TIMEOUT 20
#define ACTIVITY_RUNNING_START_TIMEOUT 300
#define ACTIVITY_NAME_MAX 256
#define ACTIVITY_ERROR_MAX 256


int
json_grow(json_buf_t *b, size_t add) {
  if(b->len + add + 1 <= b->cap) return 0;
  size_t next = b->cap ? b->cap : 1024;
  while(next < b->len + add + 1) next *= 2;
  char *p = realloc(b->data, next);
  if(!p) return -1;
  b->data = p;
  b->cap = next;
  return 0;
}


int
json_append(json_buf_t *b, const char *s) {
  size_t n = strlen(s);
  if(json_grow(b, n) != 0) return -1;
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = 0;
  return 0;
}


int
json_appendf(json_buf_t *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list cp;
  va_copy(cp, ap);
  int n = vsnprintf(NULL, 0, fmt, cp);
  va_end(cp);
  if(n < 0) {
    va_end(ap);
    return -1;
  }
  if(json_grow(b, (size_t)n) != 0) {
    va_end(ap);
    return -1;
  }
  vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
  return 0;
}


int
json_string(json_buf_t *b, const char *s) {
  if(json_append(b, "\"") != 0) return -1;
  for(const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
    switch(*p) {
    case '\\': if(json_append(b, "\\\\") != 0) return -1; break;
    case '"':  if(json_append(b, "\\\"") != 0) return -1; break;
    case '\b': if(json_append(b, "\\b") != 0) return -1; break;
    case '\f': if(json_append(b, "\\f") != 0) return -1; break;
    case '\n': if(json_append(b, "\\n") != 0) return -1; break;
    case '\r': if(json_append(b, "\\r") != 0) return -1; break;
    case '\t': if(json_append(b, "\\t") != 0) return -1; break;
    default:
      if(*p < 0x20) {
        if(json_appendf(b, "\\u%04x", *p) != 0) return -1;
      } else {
        if(json_grow(b, 1) != 0) return -1;
        b->data[b->len++] = (char)*p;
        b->data[b->len] = 0;
      }
      break;
    }
  }
  return json_append(b, "\"");
}


int
serve_owned(const http_request_t *req, int status, char *data, size_t size) {
  int rc = websrv_send(req->fd, status, "application/json", data, size);
  free(data);
  return rc;
}


int
serve_error(const http_request_t *req, int status, const char *msg) {
  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":false,\"error\":") != 0 ||
     json_string(&b, msg ? msg : "error") != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, status, b.data, b.len);
}


int
path_is_safe(const char *p) {
  if(!p || !*p) return 0;
  if(p[0] != '/') return 0;
  if(strstr(p, "..")) return 0;
  return 1;
}


static int
path_has_dir_prefix(const char *path, const char *prefix) {
  size_t n;
  if(!path || !prefix) return 0;
  n = strlen(prefix);
  return strncmp(path, prefix, n) == 0 &&
         (path[n] == 0 || path[n] == '/');
}


static int
app_convert_path_forbidden(const char *path) {
  return path_has_dir_prefix(path, "/mnt/shadowmnt") ||
         path_has_dir_prefix(path, "/system_ex");
}


int
mkdirs(const char *path) {
  char buf[1024];
  size_t n = strlen(path);
  if(n >= sizeof(buf)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(buf, path, n + 1);
  for(size_t i = 1; i <= n; i++) {
    if(buf[i] == '/' || buf[i] == 0) {
      char saved = buf[i];
      buf[i] = 0;
      if(mkdir(buf, 0777) != 0 && errno != EEXIST) return -1;
      buf[i] = saved;
    }
  }
  return 0;
}


static int
log_name_safe(const char *name) {
  if(!name || !*name || strlen(name) > 180) return 0;
  if(strstr(name, "..")) return 0;
  for(const unsigned char *p = (const unsigned char *)name; *p; p++) {
    if(!isalnum(*p) && *p != '-' && *p != '_' && *p != '.') return 0;
  }
  return 1;
}


static int
token_safe(const char *value) {
  if(!value || !*value || strlen(value) >= ACTIVITY_CLIENT_MAX) return 0;
  for(const unsigned char *p = (const unsigned char *)value; *p; p++) {
    if(!isalnum(*p) && *p != '-' && *p != '_' && *p != '.') return 0;
  }
  return 1;
}


static int
kind_safe(const char *value) {
  if(!value || !*value || strlen(value) >= ACTIVITY_KIND_MAX) return 0;
  if(strcmp(value, "upload") && strcmp(value, "zip") && strcmp(value, "rar")) {
    return 0;
  }
  return 1;
}


static void
log_slug(char *out, size_t out_size, const char *src) {
  size_t pos = 0;
  if(out_size == 0) return;
  for(const unsigned char *p = (const unsigned char *)(src ? src : "op");
      *p && pos + 1 < out_size; p++) {
    if(isalnum(*p)) out[pos++] = (char)tolower(*p);
    else if((*p == '-' || *p == '_') && pos > 0 && out[pos - 1] != '-') {
      out[pos++] = '-';
    }
  }
  while(pos > 0 && out[pos - 1] == '-') pos--;
  if(pos == 0) {
    const char fallback[] = "op";
    size_t n = sizeof(fallback) - 1;
    if(n >= out_size) n = out_size - 1;
    memcpy(out, fallback, n);
    pos = n;
  }
  out[pos] = 0;
}


static int
operation_log_dir_ready(void) {
  mkdir("/data/FileExplorer", 0777);
  if(mkdir(OP_LOG_DIR, 0777) != 0 && errno != EEXIST) return -1;
  return 0;
}


static atomic_int g_operation_log_counter;


const char *path_basename(const char *path);


static int
operation_log_make_path(char *path, size_t path_size, const char *verb,
                        time_t at) {
  char slug[48];
  int seq;

  if(!path || path_size == 0) return -1;
  path[0] = 0;
  if(operation_log_dir_ready() != 0) return -1;
  log_slug(slug, sizeof(slug), verb);
  if(!at) at = time(NULL);
  seq = atomic_fetch_add(&g_operation_log_counter, 1);

  int n = snprintf(path, path_size, "%s/%ld-%ld-%d-%s.log",
                   OP_LOG_DIR, (long)at, (long)getpid(), seq, slug);
  if(n < 0 || (size_t)n >= path_size) {
    path[0] = 0;
    return -1;
  }
  return 0;
}


int
operation_error_is_cancelled(const char *err) {
  if(!err || !*err) return 0;
  if(strstr(err, "password required") || strstr(err, "bad password")) return 0;
  return strstr(err, "cancelled") != NULL;
}


static const char *
operation_result_name(int rc, const char *err) {
  if(rc == 0) return "success";
  return operation_error_is_cancelled(err) ? "cancelled" : "failed";
}


static void
operation_log_write_at_extra(const char *path, const char *result, const char *verb,
                             const char *target, const char *err,
                             time_t started_at, time_t ended_at,
                             long total_bytes, long copied_bytes,
                             int total_files, int done_files,
                             int failed_files, const char *current,
                             const char *extra) {
  if(!path || !*path) return;
  if(!started_at) started_at = ended_at ? ended_at : time(NULL);
  if(!ended_at) ended_at = time(NULL);

  FILE *f = fopen(path, "w");
  if(!f) return;
  fprintf(f, "File Explorer operation log\n");
  fprintf(f, "Result: %s\n", result && *result ? result : "unknown");
  fprintf(f, "Operation: %s\n", verb && *verb ? verb : "operation");
  fprintf(f, "Target: %s\n", target && *target ? target : "-");
  fprintf(f, "Started: %ld\n", (long)started_at);
  fprintf(f, "Ended: %ld\n", (long)ended_at);
  fprintf(f, "DurationSeconds: %ld\n",
          ended_at >= started_at ? (long)(ended_at - started_at) : 0L);
  fprintf(f, "Bytes: %ld / %ld\n", copied_bytes, total_bytes);
  fprintf(f, "Items: %d / %d\n", done_files, total_files);
  fprintf(f, "FailedItems: %d\n", failed_files);
  fprintf(f, "Error: %s\n",
          !strcmp(result ? result : "", "success") ? "none" :
          (err && *err ? err : "failed"));
  fprintf(f, "Current: %s\n", current && *current ? current : "-");
  if(extra && *extra) {
    fputs(extra, f);
    size_t n = strlen(extra);
    if(n == 0 || extra[n - 1] != '\n') fputc('\n', f);
  }
  fclose(f);
}


static void
operation_log_write_at(const char *path, const char *result, const char *verb,
                       const char *target, const char *err,
                       time_t started_at, time_t ended_at, long total_bytes,
                       long copied_bytes, int total_files, int done_files,
                       int failed_files, const char *current) {
  operation_log_write_at_extra(path, result, verb, target, err, started_at,
                               ended_at, total_bytes, copied_bytes,
                               total_files, done_files, failed_files, current,
                               NULL);
}


static void
operation_log_write(const char *verb, const char *target, int rc,
                    const char *err, time_t started_at, time_t ended_at,
                    long total_bytes, long copied_bytes, int total_files,
                    int done_files, int failed_files, const char *current) {
  char path[1024];
  if(operation_log_make_path(path, sizeof(path), verb,
                             ended_at ? ended_at : time(NULL)) != 0) {
    return;
  }
  operation_log_write_at(path, operation_result_name(rc, err), verb, target,
                         err, started_at, ended_at, total_bytes, copied_bytes,
                         total_files, done_files, failed_files, current);
}


static void
operation_log_write_named(char *log_name, size_t log_name_size,
                          const char *verb, const char *target, int rc,
                          const char *err, time_t started_at, time_t ended_at,
                          long total_bytes, long copied_bytes, int total_files,
                          int done_files, int failed_files,
                          const char *current) {
  char path[1024];
  if(log_name && log_name_size > 0) log_name[0] = 0;
  if(operation_log_make_path(path, sizeof(path), verb,
                             ended_at ? ended_at : time(NULL)) != 0) {
    return;
  }
  operation_log_write_at(path, operation_result_name(rc, err), verb, target,
                         err, started_at, ended_at, total_bytes, copied_bytes,
                         total_files, done_files, failed_files, current);
  if(log_name && log_name_size > 0) {
    snprintf(log_name, log_name_size, "%s", path_basename(path));
  }
}


static void
operation_log_write_simple(const char *verb, const char *target, int rc,
                           const char *err, long total_bytes,
                           long copied_bytes) {
  time_t now = time(NULL);
  operation_log_write(verb, target, rc, err, now, now, total_bytes,
                      copied_bytes, 1, rc == 0 ? 1 : 0, rc == 0 ? 0 : 1,
                      target);
}


static int
read_log_field(const char *path, const char *prefix, char *out,
               size_t out_size) {
  FILE *f = fopen(path, "r");
  if(!f) return -1;
  char line[1024];
  size_t prefix_len = strlen(prefix);
  if(out_size > 0) out[0] = 0;
  while(fgets(line, sizeof(line), f)) {
    if(!strncmp(line, prefix, prefix_len)) {
      char *value = line + prefix_len;
      size_t n = strlen(value);
      while(n > 0 && (value[n - 1] == '\n' || value[n - 1] == '\r')) {
        value[--n] = 0;
      }
      snprintf(out, out_size, "%s", value);
      fclose(f);
      return 0;
    }
  }
  fclose(f);
  return -1;
}


typedef struct log_summary {
  long started_at;
  long ended_at;
  long copied_bytes;
  long total_bytes;
  int done_items;
  int total_items;
  int failed_items;
} log_summary_t;


typedef struct activity_log_candidate {
  char name[256];
  struct stat st;
} activity_log_candidate_t;


static long
parse_log_long_value(const char *value, long fallback) {
  if(!value || !*value) return fallback;
  errno = 0;
  char *end = NULL;
  long parsed = strtol(value, &end, 10);
  if(end == value || errno == ERANGE) return fallback;
  return parsed;
}


static int
clamp_log_int(long value) {
  if(value <= 0) return 0;
  if(value > INT_MAX) return INT_MAX;
  return (int)value;
}


static void
parse_log_pair_value(const char *value, long *left, long *right) {
  if(!value || !left || !right) return;
  errno = 0;
  char *end = NULL;
  long parsed_left = strtol(value, &end, 10);
  if(end == value || errno == ERANGE) return;
  while(*end && isspace((unsigned char)*end)) end++;
  if(*end != '/') return;
  end++;
  while(*end && isspace((unsigned char)*end)) end++;
  errno = 0;
  char *end_right = NULL;
  long parsed_right = strtol(end, &end_right, 10);
  if(end_right == end || errno == ERANGE) return;
  *left = parsed_left < 0 ? 0 : parsed_left;
  *right = parsed_right < 0 ? 0 : parsed_right;
}


static void
read_log_summary(const char *path, log_summary_t *summary) {
  if(!summary) return;
  memset(summary, 0, sizeof(*summary));
  char value[96];
  if(read_log_field(path, "Started: ", value, sizeof(value)) == 0) {
    summary->started_at = parse_log_long_value(value, 0);
  }
  if(read_log_field(path, "Ended: ", value, sizeof(value)) == 0) {
    summary->ended_at = parse_log_long_value(value, 0);
  }
  if(read_log_field(path, "Bytes: ", value, sizeof(value)) == 0) {
    parse_log_pair_value(value, &summary->copied_bytes, &summary->total_bytes);
  }
  if(read_log_field(path, "Items: ", value, sizeof(value)) == 0) {
    long done = 0, total = 0;
    parse_log_pair_value(value, &done, &total);
    summary->done_items = clamp_log_int(done);
    summary->total_items = clamp_log_int(total);
  }
  if(read_log_field(path, "FailedItems: ", value, sizeof(value)) == 0) {
    summary->failed_items = clamp_log_int(parse_log_long_value(value, 0));
  }
}


static int
query_arg_truthy(const http_request_t *req, const char *name, int fallback) {
  char value[32];
  if(!websrv_get_query_arg(req, name, value, sizeof(value))) return fallback;
  if(!strcasecmp(value, "0") || !strcasecmp(value, "false") ||
     !strcasecmp(value, "no") || !strcasecmp(value, "off")) {
    return 0;
  }
  return 1;
}


static int
parse_int_query_arg(const http_request_t *req, const char *name, int fallback,
                    int min_value, int max_value) {
  char value[32];
  if(!websrv_get_query_arg(req, name, value, sizeof(value))) return fallback;
  errno = 0;
  char *end = NULL;
  long parsed = strtol(value, &end, 10);
  if(end == value || errno == ERANGE) return fallback;
  if(parsed < min_value) return min_value;
  if(parsed > max_value) return max_value;
  return (int)parsed;
}


static int
activity_log_candidate_newer(const activity_log_candidate_t *a,
                             const activity_log_candidate_t *b) {
  if(a->st.st_mtime != b->st.st_mtime) return a->st.st_mtime > b->st.st_mtime;
  return strcmp(a->name, b->name) > 0;
}


static int
activity_log_candidate_cmp_desc(const void *ap, const void *bp) {
  const activity_log_candidate_t *a = (const activity_log_candidate_t *)ap;
  const activity_log_candidate_t *b = (const activity_log_candidate_t *)bp;
  if(activity_log_candidate_newer(a, b)) return -1;
  if(activity_log_candidate_newer(b, a)) return 1;
  return 0;
}


static int
activity_log_candidate_oldest_index(const activity_log_candidate_t *items,
                                    int count) {
  int oldest = 0;
  for(int i = 1; i < count; i++) {
    if(activity_log_candidate_newer(&items[oldest], &items[i])) {
      oldest = i;
    }
  }
  return oldest;
}


void
join_path(char *out, size_t out_sz, const char *dir, const char *name) {
  size_t n = strlen(dir);
  snprintf(out, out_sz, "%s%s%s", dir, (n > 1 && dir[n - 1] != '/') ? "/" : "",
           name);
}


const char *
path_basename(const char *path) {
  const char *base = strrchr(path ? path : "", '/');
  return base && base[1] ? base + 1 : path;
}


static char *
trim_ascii_inplace(char *s) {
  if(!s) return s;
  while(*s && isspace((unsigned char)*s)) s++;
  char *end = s + strlen(s);
  while(end > s && isspace((unsigned char)end[-1])) *--end = 0;
  return s;
}


static int
shadowmount_image_filename_safe(const char *name) {
  if(!name || !*name || strlen(name) >= 512) return 0;
  for(const unsigned char *p = (const unsigned char *)name; *p; p++) {
    if(*p < 0x20 || *p == ':' || *p == '/' || *p == '\\') return 0;
  }
  return 1;
}


static int
shadowmount_line_matches_image_sector(const char *line, const char *filename) {
  char local[1024];
  char *eq, *key, *value, *sep, *rule_name;
  if(!line || !filename) return 0;
  snprintf(local, sizeof(local), "%s", line);
  key = trim_ascii_inplace(local);
  if(!*key || *key == '#' || *key == ';') return 0;
  eq = strchr(key, '=');
  if(!eq) return 0;
  *eq = 0;
  value = trim_ascii_inplace(eq + 1);
  key = trim_ascii_inplace(key);
  if(strcasecmp(key, "image_sector")) return 0;
  sep = strrchr(value, ':');
  if(!sep) return 0;
  *sep = 0;
  rule_name = trim_ascii_inplace(value);
  return strcasecmp(rule_name, filename) == 0;
}


static int
shadowmount_remove_image_sector(const char *image_path, char *configured_name,
                                size_t configured_name_size, int *removed,
                                char *err, size_t err_size) {
  const char *base = path_basename(image_path);
  char temp_path[1024];
  FILE *in = NULL;
  FILE *out = NULL;
  int found = 0;
  int rc = -1;

  if(configured_name && configured_name_size > 0) configured_name[0] = 0;
  if(removed) *removed = 0;
  if(!shadowmount_image_filename_safe(base)) {
    snprintf(err, err_size, "bad ShadowMount image filename");
    errno = EINVAL;
    return -1;
  }
  if(configured_name && configured_name_size > 0) {
    snprintf(configured_name, configured_name_size, "%s", base);
  }
  if(snprintf(temp_path, sizeof(temp_path), "%s.tmp",
              SHADOWMOUNT_AUTOTUNE_FILE) >= (int)sizeof(temp_path)) {
    snprintf(err, err_size, "ShadowMount autotune path too long");
    errno = ENAMETOOLONG;
    return -1;
  }

  in = fopen(SHADOWMOUNT_AUTOTUNE_FILE, "r");
  if(!in) {
    if(errno == ENOENT) return 0;
    snprintf(err, err_size, "open ShadowMount autotune: %s", strerror(errno));
    return -1;
  }
  out = fopen(temp_path, "w");
  if(!out) {
    snprintf(err, err_size, "open ShadowMount autotune temp: %s", strerror(errno));
    fclose(in);
    return -1;
  }

  char line[1024];
  while(fgets(line, sizeof(line), in)) {
    if(shadowmount_line_matches_image_sector(line, base)) {
      found = 1;
      continue;
    }
    if(fputs(line, out) == EOF) {
      snprintf(err, err_size, "write ShadowMount autotune: %s",
               strerror(errno));
      goto done;
    }
  }

  if(fclose(out) != 0) {
    out = NULL;
    snprintf(err, err_size, "close ShadowMount autotune: %s", strerror(errno));
    goto done;
  }
  out = NULL;
  if(rename(temp_path, SHADOWMOUNT_AUTOTUNE_FILE) != 0) {
    snprintf(err, err_size, "replace ShadowMount autotune: %s",
             strerror(errno));
    goto done;
  }
  if(removed) *removed = found;
  rc = 0;

done:
  if(in) fclose(in);
  if(out) fclose(out);
  if(rc != 0) unlink(temp_path);
  return rc;
}


static int
path_is_same_or_child(const char *base, const char *path) {
  size_t n = strlen(base);
  while(n > 1 && base[n - 1] == '/') n--;
  return strncmp(base, path, n) == 0 && (path[n] == 0 || path[n] == '/');
}


struct job_state g_job = {
  .lock = PTHREAD_MUTEX_INITIALIZER,
};

void
job_set_current(const char *path) {
  pthread_mutex_lock(&g_job.lock);
  snprintf(g_job.current, sizeof(g_job.current), "%s", path ? path : "");
  pthread_mutex_unlock(&g_job.lock);
}


void
job_set_target(const char *path) {
  char verb[16], current[512], target[1024], log_path[1024];
  time_t started_at;
  long total_bytes, copied_bytes;
  int total_files, done_files, failed_files, busy;

  pthread_mutex_lock(&g_job.lock);
  snprintf(g_job.target, sizeof(g_job.target), "%s", path ? path : "");
  snprintf(verb, sizeof(verb), "%s", g_job.verb);
  snprintf(current, sizeof(current), "%s", g_job.current);
  snprintf(target, sizeof(target), "%s", g_job.target);
  snprintf(log_path, sizeof(log_path), "%s", g_job.log_path);
  started_at = g_job.started_at;
  pthread_mutex_unlock(&g_job.lock);

  busy = atomic_load(&g_job.busy);
  if(!busy || !log_path[0]) return;

  total_bytes = atomic_load(&g_job.total_bytes);
  copied_bytes = atomic_load(&g_job.copied_bytes);
  total_files = atomic_load(&g_job.total_files);
  done_files = atomic_load(&g_job.done_files);
  failed_files = atomic_load(&g_job.failed_files);
  operation_log_write_at(log_path, "running", verb, target,
                         "operation still running", started_at, time(NULL),
                         total_bytes, copied_bytes, total_files, done_files,
                         failed_files, current);
}


int
job_cancelled(void) {
  return atomic_load(&g_job.cancel);
}


static int activity_enqueue_handler(const http_request_t *req);
static int activity_poll_handler(const http_request_t *req);
static int activity_progress_handler(const http_request_t *req);
static int activity_complete_handler(const http_request_t *req);
static int activity_cancel_handler(const http_request_t *req);
static int activity_clear_handler(const http_request_t *req);
static int logs_list_handler(const http_request_t *req);
static int logs_read_handler(const http_request_t *req);
static int logs_clear_handler(const http_request_t *req);
static int upload_complete_handler(const http_request_t *req);


int
job_begin(const char *verb) {
  int expected = 0;
  char log_path[1024] = {0};
  time_t started_at = time(NULL);

  if(!atomic_compare_exchange_strong(&g_job.busy, &expected, 1)) return 0;
  operation_log_make_path(log_path, sizeof(log_path), verb, started_at);
  pthread_mutex_lock(&g_job.lock);
  atomic_store(&g_job.cancel, 0);
  atomic_store(&g_job.total_bytes, 0);
  atomic_store(&g_job.copied_bytes, 0);
  atomic_store(&g_job.compressed_output_bytes, 0);
  atomic_store(&g_job.raw_blocks, 0);
  atomic_store(&g_job.compressed_blocks, 0);
  atomic_store(&g_job.skipped_zlib_blocks, 0);
  atomic_store(&g_job.total_blocks, 0);
  atomic_store(&g_job.writer_wait_us, 0);
  atomic_store(&g_job.worker_wait_us, 0);
  atomic_store(&g_job.total_files, 0);
  atomic_store(&g_job.done_files, 0);
  atomic_store(&g_job.failed_files, 0);
  g_job.current[0] = 0;
  g_job.target[0] = 0;
  snprintf(g_job.log_path, sizeof(g_job.log_path), "%s", log_path);
  g_job.error[0] = 0;
  snprintf(g_job.verb, sizeof(g_job.verb), "%s", verb);
  g_job.started_at = started_at;
  g_job.ended_at = 0;
  pthread_mutex_unlock(&g_job.lock);
  operation_log_write_at(log_path, "running", verb, "-", "operation still running",
                         started_at, started_at, 0, 0, 0, 0, 0, "-");
  return 1;
}


static void
job_end_with_extra(int rc, const char *err, const char *extra) {
  char verb[16], current[512], target[1024], final_err[256], log_path[1024];
  time_t started_at, ended_at;
  long total_bytes = atomic_load(&g_job.total_bytes);
  long copied_bytes = atomic_load(&g_job.copied_bytes);
  int total_files = atomic_load(&g_job.total_files);
  int done_files = atomic_load(&g_job.done_files);
  int failed_files = atomic_load(&g_job.failed_files);

  pthread_mutex_lock(&g_job.lock);
  g_job.ended_at = time(NULL);
  if(rc != 0 && err && !g_job.error[0]) {
    snprintf(g_job.error, sizeof(g_job.error), "%s", err);
  }
  snprintf(verb, sizeof(verb), "%s", g_job.verb);
  snprintf(current, sizeof(current), "%s", g_job.current);
  snprintf(target, sizeof(target), "%s", g_job.target[0] ? g_job.target : g_job.current);
  snprintf(log_path, sizeof(log_path), "%s", g_job.log_path);
  snprintf(final_err, sizeof(final_err), "%s", g_job.error);
  started_at = g_job.started_at;
  ended_at = g_job.ended_at;
  pthread_mutex_unlock(&g_job.lock);
  if(log_path[0]) {
    operation_log_write_at_extra(log_path, operation_result_name(rc, final_err),
                                 verb, target, final_err, started_at,
                                 ended_at, total_bytes, copied_bytes,
                                 total_files, done_files, failed_files,
                                 current, extra);
  } else {
    char path[1024];
    if(operation_log_make_path(path, sizeof(path), verb, ended_at) == 0) {
      operation_log_write_at_extra(path, operation_result_name(rc, final_err),
                                   verb, target, final_err, started_at,
                                   ended_at, total_bytes, copied_bytes,
                                   total_files, done_files, failed_files,
                                   current, extra);
    } else {
      operation_log_write(verb, target, rc, final_err, started_at, ended_at,
                          total_bytes, copied_bytes, total_files, done_files,
                          failed_files, current);
    }
  }
  atomic_store(&g_job.busy, 0);
}


void
job_end(int rc, const char *err) {
  job_end_with_extra(rc, err, NULL);
}


void
job_log_name(char *out, size_t out_size) {
  char path[1024];
  if(out && out_size > 0) out[0] = 0;
  pthread_mutex_lock(&g_job.lock);
  snprintf(path, sizeof(path), "%s", g_job.log_path);
  pthread_mutex_unlock(&g_job.lock);
  if(out && out_size > 0 && path[0]) {
    snprintf(out, out_size, "%s", path_basename(path));
  }
}


typedef enum activity_status {
  ACT_STATUS_EMPTY = 0,
  ACT_STATUS_QUEUED,
  ACT_STATUS_RUNNING,
  ACT_STATUS_SUCCESS,
  ACT_STATUS_FAILED,
  ACT_STATUS_CANCELLED,
  ACT_STATUS_ABANDONED,
} activity_status_t;


typedef struct activity_item {
  int               used;
  uint64_t          seq;
  activity_status_t status;
  char              queue_id[ACTIVITY_ID_MAX];
  char              client_id[ACTIVITY_CLIENT_MAX];
  char              local_id[ACTIVITY_CLIENT_MAX];
  char              kind[ACTIVITY_KIND_MAX];
  char              display_name[ACTIVITY_NAME_MAX];
  char              dest_path[1024];
  char              lease_token[ACTIVITY_ID_MAX];
  char              error[ACTIVITY_ERROR_MAX];
  char              log_name[256];
  long              total_bytes;
  long              copied_bytes;
  int               total_items;
  int               done_items;
  int               failed_items;
  time_t            created_at;
  time_t            updated_at;
  time_t            started_at;
  time_t            ended_at;
  time_t            heartbeat_at;
} activity_item_t;


typedef struct activity_queue {
  pthread_mutex_t lock;
  uint64_t        next_id;
  uint64_t        next_lease;
  activity_item_t items[ACTIVITY_MAX];
} activity_queue_t;


static activity_queue_t g_activity = {
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .next_id = 1,
  .next_lease = 1,
};


static const char *
activity_status_name(activity_status_t status) {
  switch(status) {
  case ACT_STATUS_QUEUED: return "queued";
  case ACT_STATUS_RUNNING: return "running";
  case ACT_STATUS_SUCCESS: return "success";
  case ACT_STATUS_FAILED: return "failed";
  case ACT_STATUS_CANCELLED: return "cancelled";
  case ACT_STATUS_ABANDONED: return "abandoned";
  default: return "unknown";
  }
}


static int
activity_terminal(activity_status_t status) {
  return status == ACT_STATUS_SUCCESS || status == ACT_STATUS_FAILED ||
         status == ACT_STATUS_CANCELLED || status == ACT_STATUS_ABANDONED;
}


static activity_item_t *
activity_find_locked(const char *queue_id) {
  if(!queue_id || !*queue_id) return NULL;
  for(int i = 0; i < ACTIVITY_MAX; i++) {
    if(g_activity.items[i].used &&
       !strcmp(g_activity.items[i].queue_id, queue_id)) {
      return &g_activity.items[i];
    }
  }
  return NULL;
}


static int
activity_running_locked(void) {
  for(int i = 0; i < ACTIVITY_MAX; i++) {
    if(g_activity.items[i].used &&
       g_activity.items[i].status == ACT_STATUS_RUNNING) {
      return 1;
    }
  }
  return 0;
}


static void
activity_make_id(char *out, size_t out_size, const char *prefix,
                 uint64_t value) {
  snprintf(out, out_size, "%s-%ld-%llu", prefix, (long)getpid(),
           (unsigned long long)value);
}


static int
activity_owned_contains(const char *owned, const char *queue_id) {
  if(!owned || !*owned || !queue_id || !*queue_id) return 0;
  size_t qn = strlen(queue_id);
  const char *p = owned;
  while(*p) {
    while(*p == ',') p++;
    const char *end = strchr(p, ',');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    if(n == qn && !strncmp(p, queue_id, n)) return 1;
    if(!end) break;
    p = end + 1;
  }
  return 0;
}


static void
activity_finish_locked(activity_item_t *item, activity_status_t status,
                       const char *err, const char *target,
                       long copied_bytes, int done_items,
                       int failed_items, const char *log_name) {
  time_t now = time(NULL);
  if(!item) return;
  item->status = status;
  item->updated_at = now;
  item->ended_at = now;
  item->heartbeat_at = now;
  item->lease_token[0] = 0;
  if(target && *target) snprintf(item->dest_path, sizeof(item->dest_path), "%s", target);
  if(copied_bytes >= 0) item->copied_bytes = copied_bytes;
  if(done_items >= 0) item->done_items = done_items;
  if(failed_items >= 0) item->failed_items = failed_items;
  if(err && *err) snprintf(item->error, sizeof(item->error), "%s", err);
  else item->error[0] = 0;
  if(log_name && *log_name) snprintf(item->log_name, sizeof(item->log_name), "%s", log_name);
}


static int
activity_append_json(json_buf_t *b, const activity_item_t *item,
                     int position, int owned) {
  if(json_append(b, "{\"source\":\"queue\",\"queueId\":") != 0 ||
     json_string(b, item->queue_id) != 0 ||
     json_append(b, ",\"clientId\":") != 0 ||
     json_string(b, item->client_id) != 0 ||
     json_append(b, ",\"localId\":") != 0 ||
     json_string(b, item->local_id) != 0 ||
     json_append(b, ",\"kind\":") != 0 ||
     json_string(b, item->kind) != 0 ||
     json_append(b, ",\"operation\":") != 0 ||
     json_string(b, item->kind) != 0 ||
     json_append(b, ",\"displayName\":") != 0 ||
     json_string(b, item->display_name) != 0 ||
     json_append(b, ",\"target\":") != 0 ||
     json_string(b, item->dest_path) != 0 ||
     json_append(b, ",\"status\":") != 0 ||
     json_string(b, activity_status_name(item->status)) != 0 ||
     json_append(b, ",\"result\":") != 0 ||
     json_string(b, activity_status_name(item->status)) != 0 ||
     json_appendf(b,
       ",\"position\":%d,\"totalBytes\":%ld,\"copiedBytes\":%ld,"
       "\"totalItems\":%d,\"doneItems\":%d,\"failedItems\":%d,"
       "\"createdAt\":%ld,\"updatedAt\":%ld,\"startedAt\":%ld,"
       "\"endedAt\":%ld,\"mtime\":%ld,\"owned\":%s",
       position, item->total_bytes, item->copied_bytes,
       item->total_items, item->done_items, item->failed_items,
       (long)item->created_at, (long)item->updated_at,
       (long)item->started_at, (long)item->ended_at,
       (long)(item->updated_at ? item->updated_at : item->created_at),
       owned ? "true" : "false") != 0 ||
     json_append(b, ",\"error\":") != 0 ||
     json_string(b, item->error) != 0 ||
     json_append(b, ",\"logName\":") != 0 ||
     json_string(b, item->log_name) != 0 ||
     json_append(b, "}") != 0) {
    return -1;
  }
  return 0;
}


void
activity_finish_queue(const char *queue_id, int rc, const char *err,
                      const char *target, long copied_bytes,
                      int done_items, int failed_items,
                      const char *log_name) {
  if(!queue_id || !*queue_id) return;
  pthread_mutex_lock(&g_activity.lock);
  activity_item_t *item = activity_find_locked(queue_id);
  if(item) {
    activity_status_t status = rc == 0 ? ACT_STATUS_SUCCESS :
      (operation_error_is_cancelled(err) ? ACT_STATUS_CANCELLED :
       ACT_STATUS_FAILED);
    activity_finish_locked(item, status,
                           err, target, copied_bytes, done_items, failed_items,
                           log_name);
  }
  pthread_mutex_unlock(&g_activity.lock);
}


void
activity_defer_queue_success(const char *queue_id, const char *target,
                             long copied_bytes, int done_items,
                             const char *log_name) {
  if(!queue_id || !*queue_id) return;
  pthread_mutex_lock(&g_activity.lock);
  activity_item_t *item = activity_find_locked(queue_id);
  if(item && item->status == ACT_STATUS_RUNNING) {
    time_t now = time(NULL);
    if(target && *target) {
      snprintf(item->dest_path, sizeof(item->dest_path), "%s", target);
    }
    if(copied_bytes >= 0) item->copied_bytes = copied_bytes;
    if(done_items >= 0) item->done_items = done_items;
    item->failed_items = 0;
    item->error[0] = 0;
    if(log_name && *log_name) {
      snprintf(item->log_name, sizeof(item->log_name), "%s", log_name);
    }
    item->updated_at = item->heartbeat_at = now;
  }
  pthread_mutex_unlock(&g_activity.lock);
}


static int
activity_terminal_ok_response(const http_request_t *req) {
  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"terminal\":true}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
activity_request_is_terminal(const http_request_t *req) {
  char queue_id[ACTIVITY_ID_MAX];
  int terminal = 0;
  if(!websrv_get_query_arg(req, "queueId", queue_id, sizeof(queue_id)) ||
     !token_safe(queue_id)) {
    return 0;
  }
  pthread_mutex_lock(&g_activity.lock);
  activity_item_t *item = activity_find_locked(queue_id);
  terminal = item && activity_terminal(item->status);
  pthread_mutex_unlock(&g_activity.lock);
  return terminal;
}


int
activity_validate_lease(const http_request_t *req, const char *kind,
                        activity_request_ctx_t *ctx, char *err,
                        size_t err_size) {
  char queue_id[ACTIVITY_ID_MAX], lease[ACTIVITY_ID_MAX];
  if(ctx) memset(ctx, 0, sizeof(*ctx));
  if(!websrv_get_query_arg(req, "queueId", queue_id, sizeof(queue_id)) ||
     !queue_id[0]) {
    return 0;
  }
  if(!token_safe(queue_id) ||
     !websrv_get_query_arg(req, "leaseToken", lease, sizeof(lease)) ||
     !token_safe(lease)) {
    snprintf(err, err_size, "bad activity lease");
    return -1;
  }

  pthread_mutex_lock(&g_activity.lock);
  activity_item_t *item = activity_find_locked(queue_id);
  if(!item || item->status != ACT_STATUS_RUNNING ||
     strcmp(item->lease_token, lease) ||
     (kind && *kind && strcmp(item->kind, kind))) {
    pthread_mutex_unlock(&g_activity.lock);
    snprintf(err, err_size, "activity lease is not current");
    return -1;
  }
  item->heartbeat_at = time(NULL);
  item->updated_at = item->heartbeat_at;
  if(ctx) {
    ctx->queued = 1;
    ctx->seq = item->seq;
    snprintf(ctx->queue_id, sizeof(ctx->queue_id), "%s", item->queue_id);
    snprintf(ctx->lease_token, sizeof(ctx->lease_token), "%s", item->lease_token);
    snprintf(ctx->kind, sizeof(ctx->kind), "%s", item->kind);
  }
  pthread_mutex_unlock(&g_activity.lock);
  return 0;
}


typedef struct bg_file_op {
  char   verb[16];
  char   src[1024];
  char   dst[1024];
  char   target[1024];
  char   current[512];
  char   error[256];
  char   extra[2048];
  char   log_path[1024];
  int    is_move;
  long   total_bytes;
  long   copied_bytes;
  int    total_files;
  int    done_files;
  int    failed_files;
  time_t started_at;
  time_t last_log_at;
} bg_file_op_t;


static void
bg_file_op_current(bg_file_op_t *op, const char *path) {
  if(op && path && *path) snprintf(op->current, sizeof(op->current), "%s", path);
}


static void
bg_file_op_error(bg_file_op_t *op, const char *fmt, ...) {
  if(!op || op->error[0]) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(op->error, sizeof(op->error), fmt, ap);
  va_end(ap);
}


static const char *
bg_file_op_target(const bg_file_op_t *op) {
  if(!op) return "-";
  if(op->target[0]) return op->target;
  if(op->dst[0]) return op->dst;
  if(op->src[0]) return op->src;
  return "-";
}


static void
bg_file_op_write_progress(bg_file_op_t *op, int force) {
  time_t now = time(NULL);
  if(!op) return;
  if(!force && op->last_log_at > 0 && now - op->last_log_at < 2) return;
  op->last_log_at = now;
  operation_log_write_at(op->log_path, "running", op->verb,
                         bg_file_op_target(op), "operation still running",
                         op->started_at, now, op->total_bytes,
                         op->copied_bytes, op->total_files, op->done_files,
                         op->failed_files,
                         op->current[0] ? op->current : bg_file_op_target(op));
}


static void
bg_file_op_write_running(bg_file_op_t *op) {
  bg_file_op_write_progress(op, 1);
}


static void
bg_file_op_finish(bg_file_op_t *op, int rc) {
  time_t ended_at = time(NULL);
  if(!op) return;
  if(rc != 0 && op->failed_files <= 0) op->failed_files = 1;
  operation_log_write_at_extra(op->log_path, rc == 0 ? "success" : "failed",
                               op->verb, bg_file_op_target(op),
                               rc == 0 ? NULL :
                                 (op->error[0] ? op->error : "operation failed"),
                               op->started_at, ended_at, op->total_bytes,
                               op->copied_bytes, op->total_files,
                               op->done_files, op->failed_files,
                               op->current[0] ? op->current :
                                 bg_file_op_target(op),
                               op->extra);
}


static void
size_walker(const char *path, long *items, long *bytes, int count_dirs) {

  struct stat st;
  if(lstat(path, &st) != 0) return;
  if(S_ISDIR(st.st_mode)) {
    if(count_dirs) (*items)++;
    if(count_dirs && st.st_size > 0) *bytes += (long)st.st_size;
    DIR *d = opendir(path);
    if(!d) return;
    struct dirent *ent;
    while((ent = readdir(d))) {
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      char child[1024];
      if(strlen(path) + strlen(ent->d_name) + 2 >= sizeof(child)) continue;
      join_path(child, sizeof(child), path, ent->d_name);
      size_walker(child, items, bytes, count_dirs);
    }
    closedir(d);
  } else if(S_ISREG(st.st_mode)) {
    (*items)++;
    *bytes += (long)st.st_size;
  } else if(count_dirs) {
    (*items)++;
    if(st.st_size > 0) *bytes += (long)st.st_size;
  }
}


typedef struct sha256_ctx {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t  data[64];
  size_t   datalen;
} sha256_ctx_t;


static uint32_t
sha256_rotr(uint32_t value, uint32_t bits) {
  return (value >> bits) | (value << (32 - bits));
}


static uint32_t
sha256_load_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}


static void
sha256_store_be32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}


static const uint32_t sha256_k[64] = {
  0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
  0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
  0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
  0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
  0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
  0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
  0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
  0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
  0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
  0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
  0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
  0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
  0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
  0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
  0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
  0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};


static void
sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64]) {
  uint32_t m[64];
  for(int i = 0; i < 16; i++) {
    m[i] = sha256_load_be32(data + (i * 4));
  }
  for(int i = 16; i < 64; i++) {
    uint32_t s0 = sha256_rotr(m[i - 15], 7) ^
                  sha256_rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
    uint32_t s1 = sha256_rotr(m[i - 2], 17) ^
                  sha256_rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
    m[i] = m[i - 16] + s0 + m[i - 7] + s1;
  }

  uint32_t a = ctx->state[0];
  uint32_t b = ctx->state[1];
  uint32_t c = ctx->state[2];
  uint32_t d = ctx->state[3];
  uint32_t e = ctx->state[4];
  uint32_t f = ctx->state[5];
  uint32_t g = ctx->state[6];
  uint32_t h = ctx->state[7];

  for(int i = 0; i < 64; i++) {
    uint32_t s1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^
                  sha256_rotr(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t temp1 = h + s1 + ch + sha256_k[i] + m[i];
    uint32_t s0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^
                  sha256_rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}


static void
sha256_init(sha256_ctx_t *ctx) {
  ctx->datalen = 0;
  ctx->bitlen = 0;
  ctx->state[0] = 0x6a09e667U;
  ctx->state[1] = 0xbb67ae85U;
  ctx->state[2] = 0x3c6ef372U;
  ctx->state[3] = 0xa54ff53aU;
  ctx->state[4] = 0x510e527fU;
  ctx->state[5] = 0x9b05688cU;
  ctx->state[6] = 0x1f83d9abU;
  ctx->state[7] = 0x5be0cd19U;
}


static void
sha256_update(sha256_ctx_t *ctx, const void *data, size_t len) {
  const uint8_t *bytes = data;
  for(size_t i = 0; i < len; i++) {
    ctx->data[ctx->datalen++] = bytes[i];
    if(ctx->datalen == sizeof(ctx->data)) {
      sha256_transform(ctx, ctx->data);
      ctx->bitlen += 512;
      ctx->datalen = 0;
    }
  }
}


static void
sha256_final(sha256_ctx_t *ctx, uint8_t hash[32]) {
  size_t i = ctx->datalen;
  ctx->data[i++] = 0x80;
  if(i > 56) {
    while(i < sizeof(ctx->data)) ctx->data[i++] = 0;
    sha256_transform(ctx, ctx->data);
    i = 0;
  }
  while(i < 56) ctx->data[i++] = 0;

  ctx->bitlen += (uint64_t)ctx->datalen * 8;
  for(int j = 0; j < 8; j++) {
    ctx->data[63 - j] = (uint8_t)(ctx->bitlen >> (j * 8));
  }
  sha256_transform(ctx, ctx->data);

  for(int j = 0; j < 8; j++) {
    sha256_store_be32(hash + (j * 4), ctx->state[j]);
  }
}


static void
sha256_hex(const uint8_t hash[32], char out[65]) {
  static const char hex[] = "0123456789abcdef";
  for(int i = 0; i < 32; i++) {
    out[i * 2] = hex[hash[i] >> 4];
    out[i * 2 + 1] = hex[hash[i] & 0xf];
  }
  out[64] = 0;
}


static void
checksum_update_cstr(sha256_ctx_t *ctx, const char *s) {
  static const uint8_t zero = 0;
  if(s && *s) sha256_update(ctx, s, strlen(s));
  sha256_update(ctx, &zero, 1);
}


static void
checksum_update_u64_le(sha256_ctx_t *ctx, uint64_t value) {
  uint8_t data[8];
  for(int i = 0; i < 8; i++) {
    data[i] = (uint8_t)(value >> (i * 8));
  }
  sha256_update(ctx, data, sizeof(data));
}


typedef struct checksum_entry {
  char name[256];
} checksum_entry_t;


static int
checksum_ascii_tolower(int ch) {
  return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
}


static int
checksum_name_cmp(const void *a, const void *b) {
  const checksum_entry_t *ea = a;
  const checksum_entry_t *eb = b;
  const unsigned char *pa = (const unsigned char *)ea->name;
  const unsigned char *pb = (const unsigned char *)eb->name;
  while(*pa || *pb) {
    int ca = checksum_ascii_tolower(*pa);
    int cb = checksum_ascii_tolower(*pb);
    if(ca != cb) return ca - cb;
    if(*pa) pa++;
    if(*pb) pb++;
  }
  return strcmp(ea->name, eb->name);
}


static int
checksum_push_entry(checksum_entry_t **entries, size_t *count, size_t *cap,
                    const char *name, bg_file_op_t *op) {
  if(strlen(name) >= sizeof((*entries)[0].name)) {
    bg_file_op_error(op, "name too long: %s", name);
    return -1;
  }
  if(*count == *cap) {
    size_t next = *cap ? *cap * 2 : 64;
    checksum_entry_t *p = realloc(*entries, next * sizeof(*p));
    if(!p) {
      bg_file_op_error(op, "out of memory");
      return -1;
    }
    *entries = p;
    *cap = next;
  }
  snprintf((*entries)[*count].name, sizeof((*entries)[*count].name), "%s", name);
  (*count)++;
  return 0;
}


static int
checksum_join_rel(char *out, size_t out_size, const char *parent,
                  const char *name) {
  int n;
  if(parent && *parent) {
    n = snprintf(out, out_size, "%s/%s", parent, name);
  } else {
    n = snprintf(out, out_size, "%s", name);
  }
  return n >= 0 && (size_t)n < out_size ? 0 : -1;
}


static int
checksum_hash_file(bg_file_op_t *op, sha256_ctx_t *app_ctx, const char *path,
                   const char *rel, const struct stat *st) {
  int fd = open(path, O_RDONLY);
  if(fd < 0) {
    bg_file_op_error(op, "open(%s): %s", path, strerror(errno));
    return -1;
  }

  uint8_t *buf = malloc(COPY_BUF_SIZE);
  if(!buf) {
    close(fd);
    bg_file_op_error(op, "out of memory");
    return -1;
  }

  sha256_ctx_t file_ctx;
  sha256_init(&file_ctx);
  uint64_t file_bytes = 0;
  bg_file_op_current(op, rel);

  for(;;) {
    ssize_t n = read(fd, buf, COPY_BUF_SIZE);
    if(n < 0) {
      if(errno == EINTR) continue;
      bg_file_op_error(op, "read(%s): %s", path, strerror(errno));
      free(buf);
      close(fd);
      return -1;
    }
    if(n == 0) break;
    sha256_update(&file_ctx, buf, (size_t)n);
    file_bytes += (uint64_t)n;
    if(op) op->copied_bytes += (long)n;
    bg_file_op_write_progress(op, 0);
  }

  free(buf);
  if(close(fd) != 0) {
    bg_file_op_error(op, "close(%s): %s", path, strerror(errno));
    return -1;
  }

  if(st && S_ISREG(st->st_mode) && file_bytes != (uint64_t)st->st_size) {
    bg_file_op_error(op, "file changed while hashing: %s", path);
    return -1;
  }

  uint8_t file_hash[32];
  sha256_final(&file_ctx, file_hash);

  checksum_update_cstr(app_ctx, "F");
  checksum_update_cstr(app_ctx, rel);
  checksum_update_u64_le(app_ctx, file_bytes);
  sha256_update(app_ctx, file_hash, sizeof(file_hash));

  if(op) op->done_files++;
  bg_file_op_write_progress(op, 1);
  return 0;
}


static int
checksum_walk_dir(bg_file_op_t *op, sha256_ctx_t *app_ctx, const char *path,
                  const char *rel_parent) {
  DIR *d = opendir(path);
  if(!d) {
    bg_file_op_error(op, "opendir(%s): %s", path, strerror(errno));
    return -1;
  }

  checksum_entry_t *entries = NULL;
  size_t count = 0, cap = 0;
  struct dirent *ent;
  int rc = 0;
  while((ent = readdir(d))) {
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    if(checksum_push_entry(&entries, &count, &cap, ent->d_name, op) != 0) {
      rc = -1;
      break;
    }
  }
  closedir(d);
  if(rc != 0) {
    free(entries);
    return -1;
  }

  qsort(entries, count, sizeof(entries[0]), checksum_name_cmp);
  for(size_t i = 0; i < count; i++) {
    char child[1024];
    char rel[1024];
    if(strlen(path) + strlen(entries[i].name) + 2 >= sizeof(child) ||
       checksum_join_rel(rel, sizeof(rel), rel_parent, entries[i].name) != 0) {
      bg_file_op_error(op, "path too long: %s/%s", path, entries[i].name);
      rc = -1;
      break;
    }
    join_path(child, sizeof(child), path, entries[i].name);

    struct stat st;
    if(lstat(child, &st) != 0) {
      bg_file_op_error(op, "stat(%s): %s", child, strerror(errno));
      rc = -1;
      break;
    }

    if(S_ISDIR(st.st_mode)) {
      checksum_update_cstr(app_ctx, "D");
      checksum_update_cstr(app_ctx, rel);
      bg_file_op_current(op, rel);
      bg_file_op_write_progress(op, 0);
      if(checksum_walk_dir(op, app_ctx, child, rel) != 0) {
        rc = -1;
        break;
      }
    } else if(S_ISREG(st.st_mode)) {
      if(checksum_hash_file(op, app_ctx, child, rel, &st) != 0) {
        rc = -1;
        break;
      }
    } else {
      bg_file_op_error(op, "unsupported entry type: %s", child);
      rc = -1;
      break;
    }
  }

  free(entries);
  return rc;
}


static void *
checksum_worker(void *arg) {
  bg_file_op_t *a = arg;
  long files = 0, bytes = 0;
  struct stat st;
  int rc = -1;

  if(lstat(a->src, &st) != 0) {
    bg_file_op_error(a, "source not found");
    goto done;
  }
  if(!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
    bg_file_op_error(a, "checksum needs a folder or file");
    goto done;
  }

  bg_file_op_current(a, "Scanning checksum target");
  size_walker(a->src, &files, &bytes, 0);
  a->total_files = files > INT_MAX ? INT_MAX : (int)files;
  a->total_bytes = bytes;
  bg_file_op_current(a, "Hashing");
  bg_file_op_write_progress(a, 1);

  sha256_ctx_t app_ctx;
  sha256_init(&app_ctx);
  checksum_update_cstr(&app_ctx, "BFpilot app checksum v1");

  if(S_ISDIR(st.st_mode)) {
    rc = checksum_walk_dir(a, &app_ctx, a->src, "");
  } else {
    rc = checksum_hash_file(a, &app_ctx, a->src, path_basename(a->src), &st);
  }
  if(rc != 0) goto done;

  uint8_t digest[32];
  char hex[65];
  sha256_final(&app_ctx, digest);
  sha256_hex(digest, hex);
  snprintf(a->extra, sizeof(a->extra),
           "ChecksumAlgorithm: SHA-256\n"
           "ChecksumScope: BFpilot app-manifest-v1\n"
           "Checksum: %s\n"
           "ChecksumFiles: %d\n"
           "ChecksumBytes: %ld\n",
           hex, a->done_files, a->copied_bytes);
  bg_file_op_current(a, "Checksum complete");
  rc = 0;

done:
  bg_file_op_finish(a, rc);
  free(a);
  return NULL;
}


static int
copy_file(bg_file_op_t *op, const char *src, const char *dst) {
  int in = open(src, O_RDONLY);
  if(in < 0) {
    bg_file_op_error(op, "open(%s): %s", src, strerror(errno));
    return -1;
  }
  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  if(out < 0) {
    bg_file_op_error(op, "open(%s): %s", dst, strerror(errno));
    close(in);
    return -1;
  }

  void *buf = malloc(COPY_BUF_SIZE);
  if(!buf) {
    bg_file_op_error(op, "out of memory");
    close(in);
    close(out);
    unlink(dst);
    return -1;
  }

  int rc = 0;
  bg_file_op_current(op, src);
  for(;;) {
    ssize_t r = read(in, buf, COPY_BUF_SIZE);
    if(r < 0) {
      if(errno == EINTR) continue;
      bg_file_op_error(op, "read(%s): %s", src, strerror(errno));
      rc = -1;
      break;
    }
    if(r == 0) break;
    char *p = buf;
    ssize_t left = r;
    while(left > 0) {
      ssize_t w = write(out, p, (size_t)left);
      if(w < 0) {
        if(errno == EINTR) continue;
        bg_file_op_error(op, "write(%s): %s", dst, strerror(errno));
        rc = -1;
        break;
      }
      if(w == 0) {
        errno = EIO;
        rc = -1;
        break;
      }
      p += w;
      left -= w;
      if(op) op->copied_bytes += (long)w;
    }
    if(rc != 0) break;
  }

  free(buf);
  close(in);
  close(out);
  if(rc != 0) unlink(dst);
  return rc;
}


static int
copy_recursive(bg_file_op_t *op, const char *src, const char *dst) {
  struct stat st;
  if(lstat(src, &st) != 0) return -1;

  if(S_ISDIR(st.st_mode)) {
    if(mkdir(dst, 0777) != 0 && errno != EEXIST) return -1;
    DIR *d = opendir(src);
    if(!d) return -1;
    int rc = 0;
    struct dirent *ent;
    while((ent = readdir(d))) {
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      char s2[1024], d2[1024];
      join_path(s2, sizeof(s2), src, ent->d_name);
      join_path(d2, sizeof(d2), dst, ent->d_name);
      if(copy_recursive(op, s2, d2) != 0) {
        rc = -1;
      }
    }
    closedir(d);
    return rc;
  }

  if(S_ISREG(st.st_mode)) {
    int rc = copy_file(op, src, dst);
    if(rc == 0 && op) op->done_files++;
    return rc;
  }

  return 0;
}


static int
delete_recursive(bg_file_op_t *op, const char *path, int count_progress) {
  struct stat st;
  if(lstat(path, &st) != 0) return -1;

  if(S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if(!d) return -1;
    int rc = 0;
    struct dirent *ent;
    while((ent = readdir(d))) {
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      char child[1024];
      join_path(child, sizeof(child), path, ent->d_name);
      if(delete_recursive(op, child, count_progress) != 0) {
        rc = -1;
        if(op) op->failed_files++;
      }
    }
    closedir(d);
    if(rc == 0) {
      if(count_progress) bg_file_op_current(op, path);
      if(rmdir(path) != 0) return -1;
      if(count_progress) {
        if(op && st.st_size > 0) op->copied_bytes += (long)st.st_size;
        if(op) op->done_files++;
      }
    }
    return rc;
  }

  if(count_progress) bg_file_op_current(op, path);
  if(unlink(path) != 0) return -1;
  if(count_progress) {
    if(op && st.st_size > 0) op->copied_bytes += (long)st.st_size;
    if(op) op->done_files++;
  }
  return 0;
}


static void *
copy_worker(void *arg) {
  bg_file_op_t *a = arg;
  long files = 0, bytes = 0;
  struct stat src_st, dst_st;
  char final_dst[1024];
  int rc = -1;

  if(lstat(a->src, &src_st) != 0) {
    bg_file_op_error(a, "source not found");
    goto done;
  }

  bg_file_op_current(a, "Scanning source");
  size_walker(a->src, &files, &bytes, 0);
  a->total_files = files > INT_MAX ? INT_MAX : (int)files;
  a->total_bytes = bytes;

  if(stat(a->dst, &dst_st) == 0) {
    if(!S_ISDIR(dst_st.st_mode)) {
      bg_file_op_error(a, "target exists and is not a folder");
      goto done;
    }
  } else {
    if(errno != ENOENT) {
      bg_file_op_error(a, "target: %s", strerror(errno));
      goto done;
    }
    if(mkdirs(a->dst) != 0) {
      bg_file_op_error(a, "mkdir target: %s", strerror(errno));
      goto done;
    }
  }

  const char *base = path_basename(a->src);
  if(strlen(a->dst) + strlen(base) + 2 >= sizeof(final_dst)) {
    bg_file_op_error(a, "target path too long");
    goto done;
  }
  join_path(final_dst, sizeof(final_dst), a->dst, base);
  snprintf(a->target, sizeof(a->target), "%s", final_dst);

  if(!strcmp(a->src, final_dst)) {
    bg_file_op_error(a, "source and destination are the same");
    goto done;
  }
  if(S_ISDIR(src_st.st_mode) &&
     path_is_same_or_child(a->src, final_dst)) {
    bg_file_op_error(a, "refusing to place a folder inside itself");
    goto done;
  }

  if(a->is_move) {
    bg_file_op_current(a, a->src);
    if(rename(a->src, final_dst) == 0) {
      a->done_files = files > INT_MAX ? INT_MAX : (int)(files > 0 ? files : 1);
      a->copied_bytes = bytes;
      rc = 0;
      goto done;
    }
    if(errno != EXDEV) {
      bg_file_op_error(a, "rename: %s", strerror(errno));
      goto done;
    }
  }

  rc = copy_recursive(a, a->src, final_dst);
  if(rc != 0) {
    bg_file_op_error(a, "copy: %s", strerror(errno));
    goto done;
  }
  if(a->is_move && delete_recursive(a, a->src, 0) != 0) {
    bg_file_op_error(a, "post-move cleanup: %s", strerror(errno));
    rc = -1;
    goto done;
  }

  rc = 0;

done:
  bg_file_op_finish(a, rc);
  free(a);
  return NULL;
}


static void *
delete_worker(void *arg) {
  bg_file_op_t *a = arg;
  long files = 0, bytes = 0;
  int rc = -1;

  bg_file_op_current(a, "Scanning delete target");
  size_walker(a->src, &files, &bytes, 1);
  a->total_files = files > INT_MAX ? INT_MAX : (int)files;
  a->total_bytes = bytes;
  bg_file_op_current(a, "Deleting");

  if(delete_recursive(a, a->src, 1) != 0) {
    bg_file_op_error(a, "delete: %s", strerror(errno));
    goto done;
  }

  rc = 0;

done:
  bg_file_op_finish(a, rc);
  free(a);
  return NULL;
}


static void
chmod_progress_cb(void *opaque, const char *path) {
  bg_file_op_t *op = opaque;
  if(!op) return;
  bg_file_op_current(op, path);
  op->done_files++;
}


static void *
chmod_worker(void *arg) {
  bg_file_op_t *a = arg;
  long items = 0, bytes = 0;
  long changed = 0;
  int rc = -1;

  bg_file_op_current(a, "Scanning permissions target");
  size_walker(a->src, &items, &bytes, 1);
  a->total_files = items > INT_MAX ? INT_MAX : (int)items;
  a->total_bytes = 0;
  bg_file_op_current(a, "Setting permissions");

  if(archive_chmod_777_recursive(a->src, NULL, NULL, chmod_progress_cb, a,
                                 &changed, a->error, sizeof(a->error)) != 0) {
    if(!a->error[0]) bg_file_op_error(a, "chmod: %s", strerror(errno));
    goto done;
  }

  a->done_files = changed > INT_MAX ? INT_MAX : (int)changed;
  rc = 0;

done:
  bg_file_op_finish(a, rc);
  free(a);
  return NULL;
}

typedef struct app_compress_op {
  char path[1024];
  int  overwrite;
  int  workers;
  int  format;
  int  delete_policy;
} app_compress_op_t;

typedef struct app_decompress_op {
  char path[1024];
  int  overwrite;
  int  workers;
  int  delete_policy;
} app_decompress_op_t;

typedef struct app_prepare_op {
  char path[1024];
} app_prepare_op_t;

static const char *
app_compress_format_name(int format) {
  return format == PFS_COMPRESS_FORMAT_EXFAT ? "exfat" : "pfs";
}

static const char *
app_delete_policy_name(int policy) {
  if(policy == PFS_DELETE_AFTER) return "after";
  if(policy == PFS_DELETE_STREAM) return "stream";
  return "keep";
}

static const char *
app_nested_type_name(int nested_type) {
  if(nested_type == PFS_NESTED_PFS) return "pfs";
  if(nested_type == PFS_NESTED_EXFAT) return "exfat";
  return "unknown";
}


static void *
app_compress_worker(void *arg) {
  app_compress_op_t *a = arg;
  pfs_app_info_t info;
  char err[256] = {0};
  char extra[1024] = {0};
  snprintf(extra, sizeof(extra),
           "Format: %s\n"
           "DeletePolicy: %s\n",
           app_compress_format_name(a->format),
           app_delete_policy_name(a->delete_policy));
  if(a->delete_policy == PFS_DELETE_STREAM) {
    snprintf(extra, sizeof(extra),
             "Format: %s\n"
             "DeletePolicy: %s\n"
             "ConvertMode: destructive-stream\n"
             "ConvertWarning: no rollback\n",
             app_compress_format_name(a->format),
             app_delete_policy_name(a->delete_policy));
  } else if(a->delete_policy == PFS_DELETE_AFTER) {
    size_t used = strlen(extra);
    snprintf(extra + used, sizeof(extra) - used,
             "ConvertMode: delete-after-success\n");
  }
  int rc = pfs_compress_app_to_ffpfsc_opts(a->path, a->overwrite, a->workers,
                                           a->format, a->delete_policy,
                                           &info, err, sizeof(err));
  if(rc == 0) {
    job_set_target(info.output_path);
    size_t used = strlen(extra);
    snprintf(extra + used, sizeof(extra) - used,
             "Output: %s\n"
             "NestedImage: %s\n",
             info.output_path, info.nested_name);
  }
  job_end_with_extra(rc, rc == 0 ? NULL : (err[0] ? err : "compress failed"),
                     extra[0] ? extra : NULL);
  free(a);
  return NULL;
}


static void *
app_decompress_worker(void *arg) {
  app_decompress_op_t *a = arg;
  pfs_decompress_info_t info;
  char err[256] = {0};
  char extra[1024] = {0};
  snprintf(extra, sizeof(extra),
           "DeletePolicy: %s\n",
           app_delete_policy_name(a->delete_policy));
  if(a->delete_policy == PFS_DELETE_STREAM) {
    size_t used = strlen(extra);
    snprintf(extra + used, sizeof(extra) - used,
             "ConvertMode: destructive-stream\n"
             "ConvertWarning: no rollback\n");
  } else if(a->delete_policy == PFS_DELETE_AFTER) {
    size_t used = strlen(extra);
    snprintf(extra + used, sizeof(extra) - used,
             "ConvertMode: delete-after-success\n");
  }
  int rc = pfs_decompress_ffpfsc_to_app_opts(a->path, a->overwrite, a->workers,
                                             a->delete_policy, &info,
                                             err, sizeof(err));
  if(rc == 0) {
    size_t used = strlen(extra);
    snprintf(extra + used, sizeof(extra) - used,
             "Output: %s\n"
             "NestedImage: %s\n"
             "NestedType: %s\n",
             info.output_path, info.nested_name,
             app_nested_type_name(info.nested_type));
  }
  if(rc == 0 && a->delete_policy != PFS_DELETE_KEEP) {
    char sm_name[512] = {0};
    char sm_err[256] = {0};
    int removed = 0;
    if(shadowmount_remove_image_sector(info.source_path, sm_name,
          sizeof(sm_name), &removed, sm_err, sizeof(sm_err)) != 0) {
      snprintf(err, sizeof(err), "decompressed, but ShadowMountPlus cleanup failed: %s",
               sm_err[0] ? sm_err : strerror(errno));
      rc = -1;
    } else {
      size_t used = strlen(extra);
      snprintf(extra + used, sizeof(extra) - used,
               "ShadowMountAutotune: %s\n"
               "ShadowMountImage: %s\n"
               "ShadowMountAction: removed\n"
               "ShadowMountRemoved: %s\n",
               SHADOWMOUNT_AUTOTUNE_FILE, sm_name,
               removed ? "yes" : "no");
      job_set_current(removed ? "ShadowMountPlus entry removed" :
                                "ShadowMountPlus entry already clean");
    }
    if(rc == 0 && info.nested_type == PFS_NESTED_EXFAT &&
       info.nested_name[0]) {
      char nested_sm_name[512] = {0};
      int nested_removed = 0;
      if(shadowmount_remove_image_sector(info.nested_name, nested_sm_name,
            sizeof(nested_sm_name), &nested_removed, sm_err,
            sizeof(sm_err)) != 0) {
        snprintf(err, sizeof(err), "decompressed, but nested exFAT ShadowMountPlus cleanup failed: %s",
                 sm_err[0] ? sm_err : strerror(errno));
        rc = -1;
      } else {
        size_t used = strlen(extra);
        snprintf(extra + used, sizeof(extra) - used,
                 "ShadowMountNestedImage: %s\n"
                 "ShadowMountNestedAction: removed\n"
                 "ShadowMountNestedRemoved: %s\n",
                 nested_sm_name, nested_removed ? "yes" : "no");
      }
    }
  }
  job_end_with_extra(rc, rc == 0 ? NULL : (err[0] ? err : "decompress failed"),
                     extra[0] ? extra : NULL);
  free(a);
  return NULL;
}


static void *
app_prepare_worker(void *arg) {
  app_prepare_op_t *a = arg;
  archive_app_prepare_info_t info;
  char err[256] = {0};
  long items = 0;
  long bytes = 0;

  if(archive_app_prepare_probe(a->path, &info, err, sizeof(err)) == 0 &&
     info.found) {
    size_walker(info.app_path, &items, &bytes, 1);
    atomic_store(&g_job.total_files, items > INT_MAX ? INT_MAX : (int)items);
    job_set_target(info.prepared_path);
  }

  int rc = archive_app_prepare_apply(a->path, &info, err, sizeof(err));
  if(rc == 0) {
    atomic_store(&g_job.done_files, atomic_load(&g_job.total_files));
  }
  job_end(rc, rc == 0 ? NULL : (err[0] ? err : "prepare failed"));
  free(a);
  return NULL;
}


static int
append_app_prepare_info(json_buf_t *b, const archive_app_prepare_info_t *info) {
  if(json_appendf(b,
       "\"found\":%s,\"prepared\":%s,"
       "\"permissionsOk\":%s,\"pathOk\":%s,"
       "\"patchMode\":%s,\"targetExists\":%s,"
       "\"mergeRequired\":%s,"
       "\"deleteExtraFiles\":%d,\"deleteExtraDirs\":%d",
       info->found ? "true" : "false",
       info->prepared ? "true" : "false",
       info->permissions_ok ? "true" : "false",
       info->path_ok ? "true" : "false",
       info->patch_mode ? "true" : "false",
       info->target_exists ? "true" : "false",
       info->merge_required ? "true" : "false",
       info->delete_extra_files,
       info->delete_extra_dirs) != 0 ||
     json_append(b, ",\"titleId\":") != 0 ||
     json_string(b, info->title_id) != 0 ||
     json_append(b, ",\"scanPath\":") != 0 ||
     json_string(b, info->scan_path) != 0 ||
     json_append(b, ",\"appPath\":") != 0 ||
     json_string(b, info->app_path) != 0 ||
     json_append(b, ",\"preparedPath\":") != 0 ||
     json_string(b, info->prepared_path) != 0 ||
     json_append(b, ",\"deleteExtraSample\":") != 0 ||
     json_string(b, info->delete_extra_sample) != 0 ||
     json_append(b, ",\"reason\":") != 0 ||
     json_string(b, info->reason) != 0) {
    return -1;
  }
  return 0;
}


static int
app_prepare_probe_handler(const http_request_t *req) {
  char path[1024];
  char err[256] = {0};
  archive_app_prepare_info_t info;

  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    return serve_error(req, 400, "bad path");
  }
  if(archive_app_prepare_probe(path, &info, err, sizeof(err)) != 0) {
    return serve_error(req, 400, err[0] ? err : "prepare probe failed");
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,") != 0 ||
     append_app_prepare_info(&b, &info) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
app_prepare_handler(const http_request_t *req) {
  char path[1024];
  char inline_arg[16] = {0};
  char err[256] = {0};
  char log_name[256] = {0};
  archive_app_prepare_info_t info;
  int inline_apply = 0;

  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    operation_log_write_simple("prepare", "-", -1, "bad path", 0, 0);
    return serve_error(req, 400, "bad path");
  }
  if(archive_app_prepare_probe(path, &info, err, sizeof(err)) != 0) {
    operation_log_write_simple("prepare", path, -1,
                               err[0] ? err : "prepare probe failed", 0, 0);
    return serve_error(req, 400, err[0] ? err : "prepare probe failed");
  }
  if(!info.found) {
    operation_log_write_simple("prepare", path, -1, "no PS5 app found", 0, 0);
    return serve_error(req, 400, "no PS5 app found");
  }
  if(websrv_get_query_arg(req, "inline", inline_arg, sizeof(inline_arg)) &&
     strcmp(inline_arg, "0") != 0) {
    inline_apply = 1;
  }
  if(info.prepared) {
    json_buf_t b = {0};
    if(json_append(&b, "{\"ok\":true,\"background\":false,\"verb\":\"prepare\",") != 0 ||
       append_app_prepare_info(&b, &info) != 0 ||
       json_append(&b, "}") != 0) {
      free(b.data);
      return -1;
    }
    return serve_owned(req, 200, b.data, b.len);
  }
  if(inline_apply) {
    int rc = archive_app_prepare_apply(path, &info, err, sizeof(err));
    if(rc != 0) {
      return serve_error(req, 500, err[0] ? err : "prepare failed");
    }
    json_buf_t b = {0};
    if(json_append(&b, "{\"ok\":true,\"background\":false,\"verb\":\"prepare\",") != 0 ||
       append_app_prepare_info(&b, &info) != 0 ||
       json_append(&b, "}") != 0) {
      free(b.data);
      return -1;
    }
    return serve_owned(req, 200, b.data, b.len);
  }
  const char *job_verb = info.patch_mode ? "patch" : "prepare";
  if(!job_begin(job_verb)) {
    return serve_error(req, 409, "job already running");
  }
  job_set_target(info.prepared_path);
  job_log_name(log_name, sizeof(log_name));

  app_prepare_op_t *a = calloc(1, sizeof(*a));
  if(!a) {
    job_end(-1, "alloc");
    return serve_error(req, 500, "alloc");
  }
  snprintf(a->path, sizeof(a->path), "%s", info.scan_path);

  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int trc = pthread_create(&t, &at, app_prepare_worker, a);
  pthread_attr_destroy(&at);
  if(trc != 0) {
    free(a);
    job_end(-1, "could not start job");
    return serve_error(req, 500, "could not start job");
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"background\":true,\"verb\":") != 0 ||
     json_string(&b, job_verb) != 0 ||
     json_append(&b, ",") != 0 ||
     append_app_prepare_info(&b, &info) != 0 ||
     json_append(&b, ",\"logName\":") != 0 ||
     json_string(&b, log_name) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
list_request(const http_request_t *req) {
  char path[1024];
  char fast[32];
  int fast_mode = websrv_get_query_arg(req, "fast", fast, sizeof(fast)) &&
                  strcmp(fast, "0") != 0;
  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    return serve_error(req, 400, "missing or unsafe path");
  }

  DIR *d = opendir(path);
  if(!d) return serve_error(req, 404, strerror(errno));

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, path) != 0 ||
     json_appendf(&b, ",\"fast\":%s,\"entries\":[",
                  fast_mode ? "true" : "false") != 0) {
    closedir(d);
    free(b.data);
    return -1;
  }

  int first = 1;
  struct dirent *ent;
  while((ent = readdir(d))) {
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    if(!strcmp(ent->d_name, ".bfpilot-backups")) continue;
    char full[1024];
    join_path(full, sizeof(full), path, ent->d_name);
    struct stat st;
    if(lstat(full, &st) != 0) continue;
    if(!first && json_append(&b, ",") != 0) break;
    first = 0;
    if(json_append(&b, "{\"name\":") != 0 ||
       json_string(&b, ent->d_name) != 0 ||
       json_appendf(&b, ",\"dir\":%s",
                    S_ISDIR(st.st_mode) ? "true" : "false") != 0) break;
    if(!fast_mode &&
       json_appendf(&b, ",\"size\":%ld,\"mtime\":%ld",
                    (long)st.st_size, (long)st.st_mtime) != 0) break;
    if(S_ISDIR(st.st_mode)) {
      int backups = archive_backup_count(full);
      if(backups > 0 &&
         json_appendf(&b, ",\"backups\":%d", backups) != 0) break;
      pfs_app_info_t app_info;
      char probe_err[128] = {0};
      if(pfs_app_probe(full, &app_info, probe_err, sizeof(probe_err)) == 0) {
        if(json_append(&b, ",\"compressible\":true,\"titleId\":") != 0 ||
           json_string(&b, app_info.title_id) != 0 ||
           json_append(&b, ",\"compressOutput\":") != 0 ||
           json_string(&b, app_info.output_path) != 0 ||
           json_appendf(&b, ",\"compressOutputExists\":%s",
                        app_info.output_exists ? "true" : "false") != 0) {
          break;
        }
      }
    } else if(S_ISREG(st.st_mode)) {
      pfs_decompress_info_t dec_info;
      char probe_err[128] = {0};
      if(pfs_decompress_probe(full, &dec_info, probe_err, sizeof(probe_err)) == 0) {
        if(json_append(&b, ",\"decompressible\":true,\"decompressOutput\":") != 0 ||
           json_string(&b, dec_info.output_path) != 0 ||
           json_appendf(&b, ",\"decompressOutputExists\":%s",
                        dec_info.output_exists ? "true" : "false") != 0) {
          break;
        }
      }
    }
    if(json_append(&b, "}") != 0) break;
  }
  closedir(d);
  if(json_append(&b, "]}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
stat_request(const http_request_t *req) {
  char path[1024];
  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    return serve_error(req, 400, "missing or unsafe path");
  }
  struct stat st;
  if(lstat(path, &st) != 0) return serve_error(req, 404, strerror(errno));

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, path) != 0 ||
     json_appendf(&b, ",\"dir\":%s,\"size\":%ld,\"mtime\":%ld}",
                  S_ISDIR(st.st_mode) ? "true" : "false",
                  (long)st.st_size, (long)st.st_mtime) != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


void
du_walk(const char *path, du_state_t *du) {
  struct stat st;
  if(lstat(path, &st) != 0) return;

  du->entries++;
  if(st.st_size > 0) du->bytes += (unsigned long long)st.st_size;

  if(!S_ISDIR(st.st_mode)) {
    du->files++;
    return;
  }

  du->dirs++;
  if(st.st_dev != du->root_dev) return;

  DIR *dir = opendir(path);
  if(!dir) return;

  struct dirent *ent;
  while((ent = readdir(dir))) {
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

    char child[1024];
    if(strlen(path) + strlen(ent->d_name) + 2 >= sizeof(child)) {
      continue;
    }
    join_path(child, sizeof(child), path, ent->d_name);
    du_walk(child, du);
  }

  closedir(dir);
}


static int
du_request(const http_request_t *req) {
  char path[1024];
  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    return serve_error(req, 400, "missing or unsafe path");
  }

  struct stat st;
  if(lstat(path, &st) != 0) return serve_error(req, 404, strerror(errno));

  du_state_t du = {0};
  du.root_dev = st.st_dev;
  du_walk(path, &du);

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, path) != 0 ||
     json_appendf(&b,
                  ",\"size\":%llu,\"entries\":%llu,\"files\":%llu,"
                  "\"dirs\":%llu,\"truncated\":false}",
                  (unsigned long long)du.bytes,
                  (unsigned long long)du.entries,
                  (unsigned long long)du.files,
                  (unsigned long long)du.dirs) != 0) {
    free(b.data);
    return -1;
  }

  return serve_owned(req, 200, b.data, b.len);
}


static int
append_fs_place(json_buf_t *b, int *first, const char *path,
                const char *kind) {
  struct stat st;
  if(stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;

  char probe[128];
  int n = snprintf(probe, sizeof(probe), "%s/.bfpilot_probe", path);
  int writable = 0;
  if(n > 0 && (size_t)n < sizeof(probe)) {
    int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd >= 0) {
      writable = 1;
      close(fd);
      unlink(probe);
    }
  }

  if(!*first && json_append(b, ",") != 0) return -1;
  *first = 0;

  if(json_append(b, "{\"path\":") != 0 ||
     json_string(b, path) != 0 ||
     json_append(b, ",\"kind\":") != 0 ||
     json_string(b, kind ? kind : "path") != 0 ||
     json_appendf(b, ",\"writable\":%s}", writable ? "true" : "false") != 0) {
    return -1;
  }
  return 0;
}


static int
usb_request(const http_request_t *req) {
  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"mounts\":[") != 0) return -1;
  int first = 1;

  if(append_fs_place(&b, &first, "/data/homebrew", "homebrew") != 0) {
    free(b.data);
    return -1;
  }
  for(int i = 0; i < 8; i++) {
    char path[24];
    snprintf(path, sizeof(path), "/mnt/usb%d", i);
    if(append_fs_place(&b, &first, path, "usb") != 0) {
      free(b.data);
      return -1;
    }
    snprintf(path, sizeof(path), "/mnt/usb%d/homebrew", i);
    if(append_fs_place(&b, &first, path, "homebrew") != 0) {
      free(b.data);
      return -1;
    }
  }
  for(int i = 0; i < 8; i++) {
    char path[24];
    snprintf(path, sizeof(path), "/mnt/ext%d", i);
    if(append_fs_place(&b, &first, path, "ext") != 0) {
      free(b.data);
      return -1;
    }
    snprintf(path, sizeof(path), "/mnt/ext%d/homebrew", i);
    if(append_fs_place(&b, &first, path, "homebrew") != 0) {
      free(b.data);
      return -1;
    }
  }
  if(json_append(&b, "]}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
spawn_copy_or_move(const http_request_t *req, int is_move) {
  char src[1024], dst[1024];
  char log_name[256] = {0};
  if(!websrv_get_query_arg(req, "src", src, sizeof(src)) ||
     !websrv_get_query_arg(req, "dst", dst, sizeof(dst)) ||
     !path_is_safe(src) || !path_is_safe(dst)) {
    operation_log_write_simple(is_move ? "move" : "copy", "-", -1,
                               "bad src/dst", 0, 0);
    return serve_error(req, 400, "bad src/dst");
  }
  if(!strcmp(src, dst)) {
    operation_log_write_simple(is_move ? "move" : "copy", src, -1,
                               "src == dst", 0, 0);
    return serve_error(req, 400, "src == dst");
  }

  bg_file_op_t *a = calloc(1, sizeof(*a));
  if(!a) {
    return serve_error(req, 500, "alloc");
  }
  snprintf(a->verb, sizeof(a->verb), "%s", is_move ? "move" : "copy");
  snprintf(a->src, sizeof(a->src), "%s", src);
  snprintf(a->dst, sizeof(a->dst), "%s", dst);
  snprintf(a->target, sizeof(a->target), "%s", dst);
  a->is_move = is_move;
  a->started_at = time(NULL);
  operation_log_make_path(a->log_path, sizeof(a->log_path), a->verb,
                          a->started_at);
  if(a->log_path[0]) snprintf(log_name, sizeof(log_name), "%s",
                              path_basename(a->log_path));
  bg_file_op_write_running(a);

  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int trc = pthread_create(&t, &at, copy_worker, a);
  pthread_attr_destroy(&at);
  if(trc != 0) {
    snprintf(a->error, sizeof(a->error), "could not start job");
    bg_file_op_finish(a, -1);
    free(a);
    return serve_error(req, 500, "could not start job");
  }

  json_buf_t b = {0};
  if(json_appendf(&b, "{\"ok\":true,\"background\":true,\"verb\":\"%s\",\"src\":",
                  is_move ? "move" : "copy") != 0 ||
     json_string(&b, src) != 0 ||
     json_append(&b, ",\"dst\":") != 0 ||
     json_string(&b, dst) != 0 ||
     json_append(&b, ",\"logName\":") != 0 ||
     json_string(&b, log_name) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
delete_handler(const http_request_t *req) {
  char path[1024], recursive[32];
  char log_name[256] = {0};
  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    operation_log_write_simple("delete", "-", -1, "bad path", 0, 0);
    return serve_error(req, 400, "bad path");
  }
  if(!strcmp(path, "/") || !strcmp(path, "/data") ||
     !strcmp(path, "/system_data") || !strcmp(path, "/user")) {
    operation_log_write_simple("delete", path, -1,
                               "refusing to delete root path", 0, 0);
    return serve_error(req, 403, "refusing to delete root path");
  }

  int has_recursive = websrv_get_query_arg(req, "recursive", recursive,
                                           sizeof(recursive));
  struct stat st;
  if(lstat(path, &st) != 0) {
    operation_log_write_simple("delete", path, -1, strerror(errno), 0, 0);
    return serve_error(req, 404, strerror(errno));
  }
  if(S_ISDIR(st.st_mode) && (!has_recursive || !strcmp(recursive, "0"))) {
    operation_log_write_simple("delete", path, -1,
                               "directory delete needs recursive=1", 0, 0);
    return serve_error(req, 400, "directory delete needs recursive=1");
  }

  bg_file_op_t *a = calloc(1, sizeof(*a));
  if(!a) {
    return serve_error(req, 500, "alloc");
  }
  snprintf(a->verb, sizeof(a->verb), "%s", "delete");
  snprintf(a->src, sizeof(a->src), "%s", path);
  snprintf(a->target, sizeof(a->target), "%s", path);
  a->started_at = time(NULL);
  operation_log_make_path(a->log_path, sizeof(a->log_path), a->verb,
                          a->started_at);
  if(a->log_path[0]) snprintf(log_name, sizeof(log_name), "%s",
                              path_basename(a->log_path));
  bg_file_op_write_running(a);

  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int trc = pthread_create(&t, &at, delete_worker, a);
  pthread_attr_destroy(&at);
  if(trc != 0) {
    snprintf(a->error, sizeof(a->error), "could not start job");
    bg_file_op_finish(a, -1);
    free(a);
    return serve_error(req, 500, "could not start job");
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"background\":true,\"verb\":\"delete\",\"path\":") != 0 ||
     json_string(&b, path) != 0 ||
     json_append(&b, ",\"logName\":") != 0 ||
     json_string(&b, log_name) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
chmod777_handler(const http_request_t *req) {
  char path[1024];
  char log_name[256] = {0};
  struct stat st;

  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    operation_log_write_simple("chmod", "-", -1, "bad path", 0, 0);
    return serve_error(req, 400, "bad path");
  }
  if(!strcmp(path, "/") || !strcmp(path, "/data") ||
     !strcmp(path, "/system_data") || !strcmp(path, "/user")) {
    operation_log_write_simple("chmod", path, -1,
                               "refusing to chmod root path", 0, 0);
    return serve_error(req, 403, "refusing to chmod root path");
  }
  if(lstat(path, &st) != 0) {
    operation_log_write_simple("chmod", path, -1, strerror(errno), 0, 0);
    return serve_error(req, 404, strerror(errno));
  }
  if(!S_ISDIR(st.st_mode)) {
    operation_log_write_simple("chmod", path, -1,
                               "recursive chmod needs a folder", 0, 0);
    return serve_error(req, 400, "recursive chmod needs a folder");
  }

  bg_file_op_t *a = calloc(1, sizeof(*a));
  if(!a) {
    return serve_error(req, 500, "alloc");
  }
  snprintf(a->verb, sizeof(a->verb), "%s", "chmod");
  snprintf(a->src, sizeof(a->src), "%s", path);
  snprintf(a->target, sizeof(a->target), "%s", path);
  a->started_at = time(NULL);
  operation_log_make_path(a->log_path, sizeof(a->log_path), a->verb,
                          a->started_at);
  if(a->log_path[0]) snprintf(log_name, sizeof(log_name), "%s",
                              path_basename(a->log_path));
  bg_file_op_write_running(a);

  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int trc = pthread_create(&t, &at, chmod_worker, a);
  pthread_attr_destroy(&at);
  if(trc != 0) {
    snprintf(a->error, sizeof(a->error), "could not start job");
    bg_file_op_finish(a, -1);
    free(a);
    return serve_error(req, 500, "could not start job");
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"background\":true,\"verb\":\"chmod\",\"path\":") != 0 ||
     json_string(&b, path) != 0 ||
     json_append(&b, ",\"mode\":\"777\",\"recursive\":true,\"logName\":") != 0 ||
     json_string(&b, log_name) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
app_checksum_handler(const http_request_t *req) {
  char path[1024];
  char log_name[256] = {0};
  struct stat st;

  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    operation_log_write_simple("checksum", "-", -1, "bad path", 0, 0);
    return serve_error(req, 400, "bad path");
  }
  if(!strcmp(path, "/")) {
    operation_log_write_simple("checksum", path, -1,
                               "refusing to checksum root path", 0, 0);
    return serve_error(req, 403, "refusing to checksum root path");
  }
  if(lstat(path, &st) != 0) {
    operation_log_write_simple("checksum", path, -1, strerror(errno), 0, 0);
    return serve_error(req, 404, strerror(errno));
  }
  if(!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
    operation_log_write_simple("checksum", path, -1,
                               "checksum needs a folder or file", 0, 0);
    return serve_error(req, 400, "checksum needs a folder or file");
  }

  bg_file_op_t *a = calloc(1, sizeof(*a));
  if(!a) {
    return serve_error(req, 500, "alloc");
  }
  snprintf(a->verb, sizeof(a->verb), "%s", "checksum");
  snprintf(a->src, sizeof(a->src), "%s", path);
  snprintf(a->target, sizeof(a->target), "%s", path);
  a->started_at = time(NULL);
  operation_log_make_path(a->log_path, sizeof(a->log_path), a->verb,
                          a->started_at);
  if(a->log_path[0]) snprintf(log_name, sizeof(log_name), "%s",
                              path_basename(a->log_path));
  bg_file_op_write_running(a);

  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int trc = pthread_create(&t, &at, checksum_worker, a);
  pthread_attr_destroy(&at);
  if(trc != 0) {
    snprintf(a->error, sizeof(a->error), "could not start job");
    bg_file_op_finish(a, -1);
    free(a);
    return serve_error(req, 500, "could not start job");
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"background\":true,\"verb\":\"checksum\",\"path\":") != 0 ||
     json_string(&b, path) != 0 ||
     json_append(&b, ",\"logName\":") != 0 ||
     json_string(&b, log_name) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
app_compress_handler(const http_request_t *req) {
  char path[1024], overwrite_arg[32], convert_arg[32];
  char format_arg[32], delete_arg[32];
  char err[256] = {0};
  pfs_app_info_t info;
  int overwrite = 0;
  int convert = 0;
  int format = PFS_COMPRESS_FORMAT_PFS;
  int delete_policy = PFS_DELETE_KEEP;
  int workers = PFS_COMPRESS_DEFAULT_WORKERS;

  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    operation_log_write_simple("compress", "-", -1, "bad path", 0, 0);
    return serve_error(req, 400, "bad path");
  }
  if(websrv_get_query_arg(req, "overwrite", overwrite_arg,
                          sizeof(overwrite_arg)) &&
     strcmp(overwrite_arg, "0") != 0) {
    overwrite = 1;
  }
  if(websrv_get_query_arg(req, "convert", convert_arg,
                          sizeof(convert_arg)) &&
     strcmp(convert_arg, "0") != 0) {
    convert = 1;
  }
  if(websrv_get_query_arg(req, "format", format_arg, sizeof(format_arg))) {
    if(!strcasecmp(format_arg, "pfs")) {
      format = PFS_COMPRESS_FORMAT_PFS;
    } else if(!strcasecmp(format_arg, "exfat")) {
      format = PFS_COMPRESS_FORMAT_EXFAT;
    } else {
      operation_log_write_simple("compress", path, -1,
                                 "bad compression format", 0, 0);
      return serve_error(req, 400, "bad compression format");
    }
  }
  if(websrv_get_query_arg(req, "delete", delete_arg, sizeof(delete_arg))) {
    if(!strcasecmp(delete_arg, "keep")) {
      delete_policy = PFS_DELETE_KEEP;
    } else if(!strcasecmp(delete_arg, "after")) {
      delete_policy = PFS_DELETE_AFTER;
    } else if(!strcasecmp(delete_arg, "stream")) {
      delete_policy = PFS_DELETE_STREAM;
    } else {
      operation_log_write_simple("compress", path, -1,
                                 "bad delete policy", 0, 0);
      return serve_error(req, 400, "bad delete policy");
    }
  }
  if(convert) {
    format = PFS_COMPRESS_FORMAT_PFS;
    delete_policy = PFS_DELETE_STREAM;
  }
  if(pfs_app_probe(path, &info, err, sizeof(err)) != 0) {
    operation_log_write_simple("compress", path, -1,
                               err[0] ? err : "not compressible", 0, 0);
    return serve_error(req, 400, err[0] ? err : "not compressible");
  }
  if(delete_policy != PFS_DELETE_KEEP &&
     app_convert_path_forbidden(info.source_path)) {
    operation_log_write_simple("compress", info.source_path, -1,
                               "convert is not allowed for mounted paths", 0, 0);
    return serve_error(req, 403, "convert is not allowed for mounted paths");
  }
  if(info.output_exists && !overwrite) {
    operation_log_write_simple("compress", info.output_path, -1,
                               "output exists", 0, 0);
    return serve_error(req, 409, "output exists");
  }
  if(!job_begin("compress")) {
    return serve_error(req, 409, "job already running");
  }

  app_compress_op_t *a = calloc(1, sizeof(*a));
  if(!a) {
    job_end(-1, "alloc");
    return serve_error(req, 500, "alloc");
  }
  snprintf(a->path, sizeof(a->path), "%s", info.source_path);
  a->overwrite = overwrite;
  a->workers = workers;
  a->format = format;
  a->delete_policy = delete_policy;

  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int trc = pthread_create(&t, &at, app_compress_worker, a);
  pthread_attr_destroy(&at);
  if(trc != 0) {
    free(a);
    job_end(-1, "could not start job");
    return serve_error(req, 500, "could not start job");
  }

  json_buf_t b = {0};
  char nested_name[256];
  if(format == PFS_COMPRESS_FORMAT_EXFAT) {
    snprintf(nested_name, sizeof(nested_name), "%s.exfat", info.title_id);
  } else {
    snprintf(nested_name, sizeof(nested_name), "pfs_image.dat");
  }
  if(json_append(&b, "{\"ok\":true,\"background\":true,\"verb\":\"compress\",\"path\":") != 0 ||
     json_string(&b, info.source_path) != 0 ||
     json_append(&b, ",\"titleId\":") != 0 ||
	     json_string(&b, info.title_id) != 0 ||
	     json_append(&b, ",\"output\":") != 0 ||
	     json_string(&b, info.output_path) != 0 ||
	     json_append(&b, ",\"format\":") != 0 ||
	     json_string(&b, app_compress_format_name(format)) != 0 ||
	     json_append(&b, ",\"deletePolicy\":") != 0 ||
	     json_string(&b, app_delete_policy_name(delete_policy)) != 0 ||
	     json_append(&b, ",\"nestedName\":") != 0 ||
	     json_string(&b, nested_name) != 0 ||
	     json_appendf(&b, ",\"convert\":%s", convert ? "true" : "false") != 0 ||
	     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
app_decompress_handler(const http_request_t *req) {
  char path[1024], overwrite_arg[32], convert_arg[32], delete_arg[32];
  char err[256] = {0};
  pfs_decompress_info_t info;
  int overwrite = 0;
  int convert = 0;
  int delete_policy = PFS_DELETE_KEEP;
  int workers = PFS_COMPRESS_DEFAULT_WORKERS;

  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    operation_log_write_simple("decompress", "-", -1, "bad path", 0, 0);
    return serve_error(req, 400, "bad path");
  }
  if(websrv_get_query_arg(req, "overwrite", overwrite_arg,
                          sizeof(overwrite_arg)) &&
     strcmp(overwrite_arg, "0") != 0) {
    overwrite = 1;
  }
  if(websrv_get_query_arg(req, "convert", convert_arg,
                          sizeof(convert_arg)) &&
     strcmp(convert_arg, "0") != 0) {
    convert = 1;
  }
  if(websrv_get_query_arg(req, "delete", delete_arg, sizeof(delete_arg))) {
    if(!strcasecmp(delete_arg, "keep")) {
      delete_policy = PFS_DELETE_KEEP;
    } else if(!strcasecmp(delete_arg, "after")) {
      delete_policy = PFS_DELETE_AFTER;
    } else if(!strcasecmp(delete_arg, "stream")) {
      delete_policy = PFS_DELETE_STREAM;
    } else {
      operation_log_write_simple("decompress", path, -1,
                                 "bad delete policy", 0, 0);
      return serve_error(req, 400, "bad delete policy");
    }
  }
  if(convert) delete_policy = PFS_DELETE_STREAM;
  if(pfs_decompress_detect_nested(path, &info, err, sizeof(err)) != 0) {
    operation_log_write_simple("decompress", path, -1,
                               err[0] ? err : "not decompressible", 0, 0);
    return serve_error(req, 400, err[0] ? err : "not decompressible");
  }
  if(delete_policy != PFS_DELETE_KEEP &&
     app_convert_path_forbidden(info.source_path)) {
    operation_log_write_simple("decompress", info.source_path, -1,
                               "convert is not allowed for mounted paths", 0, 0);
    return serve_error(req, 403, "convert is not allowed for mounted paths");
  }
  if(info.output_exists && !overwrite) {
    operation_log_write_simple("decompress", info.output_path, -1,
                               "output exists", 0, 0);
    return serve_error(req, 409, "output exists");
  }
  if(!job_begin("decompress")) {
    return serve_error(req, 409, "job already running");
  }

  app_decompress_op_t *a = calloc(1, sizeof(*a));
  if(!a) {
    job_end(-1, "alloc");
    return serve_error(req, 500, "alloc");
  }
  snprintf(a->path, sizeof(a->path), "%s", info.source_path);
  a->overwrite = overwrite;
  a->workers = workers;
  a->delete_policy = delete_policy;

  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int trc = pthread_create(&t, &at, app_decompress_worker, a);
  pthread_attr_destroy(&at);
  if(trc != 0) {
    free(a);
    job_end(-1, "could not start job");
    return serve_error(req, 500, "could not start job");
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"background\":true,\"verb\":\"decompress\",\"path\":") != 0 ||
	     json_string(&b, info.source_path) != 0 ||
	     json_append(&b, ",\"output\":") != 0 ||
	     json_string(&b, info.output_path) != 0 ||
	     json_append(&b, ",\"deletePolicy\":") != 0 ||
	     json_string(&b, app_delete_policy_name(delete_policy)) != 0 ||
	     json_append(&b, ",\"nestedType\":") != 0 ||
	     json_string(&b, app_nested_type_name(info.nested_type)) != 0 ||
	     json_append(&b, ",\"nestedName\":") != 0 ||
	     json_string(&b, info.nested_name) != 0 ||
	     json_appendf(&b, ",\"convert\":%s", convert ? "true" : "false") != 0 ||
	     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
mkdir_handler(const http_request_t *req) {
  char path[1024];
  if(!websrv_get_query_arg(req, "path", path, sizeof(path)) ||
     !path_is_safe(path)) {
    operation_log_write_simple("mkdir", "-", -1, "bad path", 0, 0);
    return serve_error(req, 400, "bad path");
  }
  if(mkdirs(path) != 0) {
    operation_log_write_simple("mkdir", path, -1, strerror(errno), 0, 0);
    return serve_error(req, 500, strerror(errno));
  }
  operation_log_write_simple("mkdir", path, 0, NULL, 0, 0);

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, path) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
rename_handler(const http_request_t *req) {
  char src[1024], dst[1024];
  if(!websrv_get_query_arg(req, "src", src, sizeof(src)) ||
     !websrv_get_query_arg(req, "dst", dst, sizeof(dst)) ||
     !path_is_safe(src) || !path_is_safe(dst)) {
    operation_log_write_simple("rename", "-", -1, "bad src/dst", 0, 0);
    return serve_error(req, 400, "bad src/dst");
  }
  if(rename(src, dst) != 0) {
    operation_log_write_simple("rename", dst, -1, strerror(errno), 0, 0);
    return serve_error(req, 500, strerror(errno));
  }
  operation_log_write_simple("rename", dst, 0, NULL, 0, 0);

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"src\":") != 0 ||
     json_string(&b, src) != 0 ||
     json_append(&b, ",\"dst\":") != 0 ||
     json_string(&b, dst) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
status_handler(const http_request_t *req) {
  int busy = atomic_load(&g_job.busy);
  long tb = atomic_load(&g_job.total_bytes);
  long cb = atomic_load(&g_job.copied_bytes);
  int tf = atomic_load(&g_job.total_files);
  int df = atomic_load(&g_job.done_files);
  int ff = atomic_load(&g_job.failed_files);
  int cancel = busy && atomic_load(&g_job.cancel);
  char verb[16], current[512], err[256];
  time_t started_at;
  time_t ended_at;

  pthread_mutex_lock(&g_job.lock);
  snprintf(verb, sizeof(verb), "%s", g_job.verb);
  snprintf(current, sizeof(current), "%s", g_job.current);
  snprintf(err, sizeof(err), "%s", g_job.error);
  started_at = g_job.started_at;
  ended_at = g_job.ended_at;
  pthread_mutex_unlock(&g_job.lock);

  time_t now = time(NULL);
  time_t ref_at = busy ? now : (ended_at ? ended_at : now);
  long elapsed = started_at > 0 && ref_at > started_at
	                     ? (long)(ref_at - started_at)
                     : 0;
  long speed = elapsed > 0 && cb > 0 ? cb / elapsed : 0;
  long eta = busy && speed > 0 && tb > cb
                 ? (tb - cb + speed - 1) / speed
                 : 0;

  json_buf_t b = {0};
  if(json_appendf(&b,
      "{\"ok\":true,\"busy\":%s,\"cancelling\":%s,\"verb\":",
      busy ? "true" : "false", cancel ? "true" : "false") != 0 ||
     json_string(&b, verb) != 0 ||
     json_append(&b, ",\"current\":") != 0 ||
     json_string(&b, current) != 0 ||
	     json_append(&b, ",\"error\":") != 0 ||
     json_string(&b, err) != 0 ||
     json_appendf(&b,
      ",\"totalBytes\":%ld,\"copiedBytes\":%ld,"
      "\"totalFiles\":%d,\"doneFiles\":%d,\"failedFiles\":%d,"
      "\"startedAt\":%ld,\"endedAt\":%ld,"
      "\"elapsedSeconds\":%ld,\"speedBytesPerSec\":%ld,"
      "\"etaSeconds\":%ld}",
      tb, cb, tf, df, ff, (long)started_at, (long)ended_at, elapsed, speed,
      eta) != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
cancel_handler(const http_request_t *req) {
  if(atomic_load(&g_job.busy)) {
    atomic_store(&g_job.cancel, 1);
    archive_upload_cancel_active();
  }
  return status_handler(req);
}


int
transfer_request(const http_request_t *req, const char *url) {
  if(!strcmp(url, "/api/fs/list")) return list_request(req);
  if(!strcmp(url, "/api/fs/stat")) return stat_request(req);
  if(!strcmp(url, "/api/fs/du")) return du_request(req);
  if(!strcmp(url, "/api/fs/usb")) return usb_request(req);
  if(!strcmp(url, "/api/fs/copy")) return spawn_copy_or_move(req, 0);
  if(!strcmp(url, "/api/fs/move")) return spawn_copy_or_move(req, 1);
  if(!strcmp(url, "/api/fs/delete")) return delete_handler(req);
  if(!strcmp(url, "/api/fs/chmod777")) return chmod777_handler(req);
  if(!strcmp(url, "/api/fs/app-prepare/probe")) return app_prepare_probe_handler(req);
  if(!strcmp(url, "/api/fs/app-prepare")) return app_prepare_handler(req);
  if(!strcmp(url, "/api/fs/app-checksum")) return app_checksum_handler(req);
  if(!strcmp(url, "/api/fs/app-compress")) return app_compress_handler(req);
  if(!strcmp(url, "/api/fs/app-decompress")) return app_decompress_handler(req);
  if(!strcmp(url, "/api/fs/mkdir")) return mkdir_handler(req);
  if(!strcmp(url, "/api/fs/rename")) return rename_handler(req);
  if(!strcmp(url, "/api/fs/job/status")) return status_handler(req);
  if(!strcmp(url, "/api/fs/job/cancel")) return cancel_handler(req);
  if(!strcmp(url, "/api/fs/activity/enqueue")) return activity_enqueue_handler(req);
  if(!strcmp(url, "/api/fs/activity/poll")) return activity_poll_handler(req);
  if(!strcmp(url, "/api/fs/activity/progress")) return activity_progress_handler(req);
  if(!strcmp(url, "/api/fs/activity/complete")) return activity_complete_handler(req);
  if(!strcmp(url, "/api/fs/activity/cancel")) return activity_cancel_handler(req);
  if(!strcmp(url, "/api/fs/activity/clear")) return activity_clear_handler(req);
  if(!strcmp(url, "/api/fs/logs/list")) return logs_list_handler(req);
  if(!strcmp(url, "/api/fs/logs/read")) return logs_read_handler(req);
  if(!strcmp(url, "/api/fs/logs/clear")) return logs_clear_handler(req);
  if(!strcmp(url, "/api/fs/backups/list")) return archive_backups_list_handler(req);
  if(!strcmp(url, "/api/fs/backups/restore")) return archive_backup_restore_handler(req);
  if(!strcmp(url, "/api/fs/backups/remove")) return archive_backup_remove_handler(req);
  if(!strcmp(url, "/api/fs/extract")) return extract_archive_handler(req);
  if(!strcmp(url, "/api/fs/upload-complete")) return upload_complete_handler(req);
  return serve_error(req, 404, "no such endpoint");
}


int
upload_segment_safe(const char *seg) {
  if(!seg || !*seg) return 0;
  if(!strcmp(seg, ".") || !strcmp(seg, "..")) return 0;
  for(const char *p = seg; *p; p++) {
    if(*p == '/' || *p == '\\') return 0;
  }
  return 1;
}


int
write_all_fd(int fd, const void *data, size_t size) {
  const char *p = data;
  while(size > 0) {
    ssize_t n = write(fd, p, size);
    if(n < 0) {
      if(errno == EINTR) continue;
      return -1;
    }
    if(n == 0) return -1;
    p += n;
    size -= (size_t)n;
  }
  return 0;
}


static int
pwrite_all_fd(int fd, const void *data, size_t size, off_t offset) {
  const char *p = data;
  while(size > 0) {
    ssize_t n = pwrite(fd, p, size, offset);
    if(n < 0) {
      if(errno == EINTR) continue;
      return -1;
    }
    if(n == 0) return -1;
    p += n;
    size -= (size_t)n;
    offset += n;
  }
  return 0;
}


void
drain_body(int fd, size_t already_read, size_t content_size) {
  char buf[8192];
  size_t remaining = content_size > already_read ? content_size - already_read : 0;
  while(remaining > 0) {
    size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
    ssize_t n = recv(fd, buf, want, 0);
    if(n < 0) {
      if(errno == EINTR) continue;
      break;
    }
    if(n == 0) break;
    remaining -= (size_t)n;
  }
}


int
parse_upload_size_arg(const http_request_t *req, const char *name,
                      uint64_t *out) {
  char value[64];
  char *end = NULL;

  if(!websrv_get_query_arg(req, name, value, sizeof(value)) || !value[0]) {
    return 0;
  }
  errno = 0;
  unsigned long long parsed = strtoull(value, &end, 10);
  if(errno != 0 || !end || *end) return 0;
  *out = (uint64_t)parsed;
  return 1;
}


static int
build_upload_target(const http_request_t *req, char *final_path,
                    size_t final_path_size, char *err, size_t err_size,
                    int *status) {
  char dest[1024], fname[256], relpath[768];
  char dir[1024];
  int has_relpath = 0;

  if(status) *status = 400;
  if(err_size > 0) err[0] = 0;

  if(!websrv_get_query_arg(req, "path", dest, sizeof(dest)) ||
     !path_is_safe(dest) ||
     !websrv_get_query_arg(req, "filename", fname, sizeof(fname)) ||
     !upload_segment_safe(fname)) {
    snprintf(err, err_size, "bad upload target");
    return -1;
  }

  has_relpath = websrv_get_query_arg(req, "relpath", relpath, sizeof(relpath));
  snprintf(dir, sizeof(dir), "%s", dest);
  size_t dlen = strlen(dir);
  while(dlen > 1 && dir[dlen - 1] == '/') dir[--dlen] = 0;

  if(has_relpath && relpath[0]) {
    char tmp[768];
    snprintf(tmp, sizeof(tmp), "%s", relpath);
    char *seg = tmp;
    while(seg && *seg) {
      char *slash = strchr(seg, '/');
      if(slash) *slash = 0;
      if(*seg) {
        if(!upload_segment_safe(seg)) {
          snprintf(err, err_size, "bad relative path");
          return -1;
        }
        size_t used = strlen(dir);
        if(used + strlen(seg) + 2 >= sizeof(dir)) {
          snprintf(err, err_size, "upload path too long");
          return -1;
        }
        snprintf(dir + used, sizeof(dir) - used, "/%s", seg);
      }
      if(!slash) break;
      seg = slash + 1;
    }
  }

  if(mkdirs(dir) != 0) {
    if(status) *status = 500;
    snprintf(err, err_size, "%s", strerror(errno));
    return -1;
  }

  size_t dir_len = strlen(dir);
  size_t fname_len = strlen(fname);
  int needs_slash = dir_len > 1 && dir[dir_len - 1] != '/';
  if(dir_len + (needs_slash ? 1 : 0) + fname_len + 1 > final_path_size) {
    snprintf(err, err_size, "upload path too long");
    return -1;
  }

  join_path(final_path, final_path_size, dir, fname);
  return 0;
}


static int
build_queue_temp_path(const char *final_path, const char *queue_id,
                      char *tmp_path, size_t tmp_path_size) {
  const char *base = path_basename(final_path);
  size_t dir_len = base && base > final_path ? (size_t)(base - final_path) : 0;
  char safe_id[ACTIVITY_ID_MAX];
  if(!final_path || !queue_id || !token_safe(queue_id) ||
     !base || !*base || dir_len == 0) {
    errno = EINVAL;
    return -1;
  }
  snprintf(safe_id, sizeof(safe_id), "%s", queue_id);
  if(dir_len + strlen(".bfpilot-part--") + strlen(safe_id) + strlen(base) + 1 >=
     tmp_path_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(tmp_path, final_path, dir_len);
  tmp_path[dir_len] = 0;
  snprintf(tmp_path + dir_len, tmp_path_size - dir_len,
           ".bfpilot-part-%s-%s", safe_id, base);
  return 0;
}


static int
activity_append_log_json(json_buf_t *b, const char *name, const struct stat *st,
                         const char *result, const char *operation,
                         const char *target, const char *error,
                         const log_summary_t *summary) {
  char id[320];
  json_buf_t *jb = b;
  long mtime = st ? (long)st->st_mtime : (long)time(NULL);
  long started_at = summary && summary->started_at > 0 ?
                    summary->started_at : mtime;
  long ended_at = summary && summary->ended_at > 0 ?
                  summary->ended_at : mtime;
  long updated_at = ended_at > 0 ? ended_at : mtime;
  long total_bytes = summary ? summary->total_bytes : 0;
  long copied_bytes = summary ? summary->copied_bytes : 0;
  int total_items = summary ? summary->total_items : 0;
  int done_items = summary ? summary->done_items : 0;
  int failed_items = summary ? summary->failed_items : 0;
  if(result && (!strcmp(result, "running") || !strcmp(result, "queued"))) {
    updated_at = mtime;
  }
  const char *display_name = operation;
  if(target && target[0] && strcmp(target, "-")) {
    const char *base = path_basename(target);
    if(base && *base) display_name = base;
  }
  snprintf(id, sizeof(id), "log:%s", name ? name : "");
  if(json_append(jb, "{\"source\":\"log\",\"queueId\":") != 0 ||
     json_string(jb, id) != 0 ||
     json_append(jb, ",\"clientId\":\"\",\"localId\":\"\",\"kind\":") != 0 ||
     json_string(jb, operation) != 0 ||
     json_append(jb, ",\"operation\":") != 0 ||
     json_string(jb, operation) != 0 ||
     json_append(jb, ",\"displayName\":") != 0 ||
     json_string(jb, display_name) != 0 ||
     json_append(jb, ",\"target\":") != 0 ||
     json_string(jb, target) != 0 ||
     json_append(jb, ",\"status\":") != 0 ||
     json_string(jb, result) != 0 ||
     json_append(jb, ",\"result\":") != 0 ||
     json_string(jb, result) != 0 ||
     json_appendf(jb,
       ",\"position\":0,\"totalBytes\":%ld,\"copiedBytes\":%ld,"
       "\"totalItems\":%d,\"doneItems\":%d,\"failedItems\":%d,"
       "\"createdAt\":%ld,\"updatedAt\":%ld,\"startedAt\":%ld,"
       "\"endedAt\":%ld,\"mtime\":%ld,\"owned\":false",
       total_bytes, copied_bytes, total_items, done_items, failed_items,
       started_at, updated_at, started_at, ended_at, mtime) != 0 ||
     json_append(jb, ",\"error\":") != 0 ||
     json_string(jb, error) != 0 ||
     json_append(jb, ",\"logName\":") != 0 ||
     json_string(jb, name) != 0 ||
     json_append(jb, "}") != 0) {
    return -1;
  }
  return 0;
}


static int
activity_queue_has_log_locked(const char *name) {
  if(!name || !*name) return 0;
  for(int i = 0; i < ACTIVITY_MAX; i++) {
    if(g_activity.items[i].used && g_activity.items[i].log_name[0] &&
       !strcmp(g_activity.items[i].log_name, name)) {
      return 1;
    }
  }
  return 0;
}


static int
activity_append_logs(json_buf_t *b, int *first, int limit) {
  if(limit <= 0) return 0;
  if(limit > ACTIVITY_LOG_LIMIT_MAX) limit = ACTIVITY_LOG_LIMIT_MAX;
  if(operation_log_dir_ready() != 0) return 0;
  DIR *d = opendir(OP_LOG_DIR);
  if(!d) return 0;
  activity_log_candidate_t *candidates =
    calloc((size_t)limit, sizeof(*candidates));
  if(!candidates) {
    closedir(d);
    return 0;
  }

  struct dirent *ent;
  int count = 0;
  while((ent = readdir(d))) {
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    if(!log_name_safe(ent->d_name)) continue;

    char path[1024];
    if(snprintf(path, sizeof(path), "%s/%s", OP_LOG_DIR, ent->d_name) >=
       (int)sizeof(path)) {
      continue;
    }
    struct stat st;
    if(lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

    pthread_mutex_lock(&g_activity.lock);
    int skip = activity_queue_has_log_locked(ent->d_name);
    pthread_mutex_unlock(&g_activity.lock);
    if(skip) continue;

    activity_log_candidate_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    if(snprintf(candidate.name, sizeof(candidate.name), "%s", ent->d_name) >=
       (int)sizeof(candidate.name)) {
      continue;
    }
    candidate.st = st;
    if(count < limit) {
      candidates[count++] = candidate;
    } else {
      int oldest = activity_log_candidate_oldest_index(candidates, count);
      if(activity_log_candidate_newer(&candidate, &candidates[oldest])) {
        candidates[oldest] = candidate;
      }
    }
  }
  closedir(d);

  qsort(candidates, (size_t)count, sizeof(candidates[0]),
        activity_log_candidate_cmp_desc);

  for(int i = 0; i < count; i++) {
    char path[1024];
    if(snprintf(path, sizeof(path), "%s/%s", OP_LOG_DIR,
                candidates[i].name) >= (int)sizeof(path)) {
      continue;
    }
    char result[32], operation[32], target[256], error[256];
    if(read_log_field(path, "Result: ", result, sizeof(result)) != 0) {
      snprintf(result, sizeof(result), "unknown");
    }
    if(read_log_field(path, "Operation: ", operation, sizeof(operation)) != 0) {
      snprintf(operation, sizeof(operation), "operation");
    }
    if(read_log_field(path, "Target: ", target, sizeof(target)) != 0) {
      snprintf(target, sizeof(target), "-");
    }
    if(read_log_field(path, "Error: ", error, sizeof(error)) != 0) {
      snprintf(error, sizeof(error), "");
    }
    log_summary_t summary;
    read_log_summary(path, &summary);

    if(!*first && json_append(b, ",") != 0) break;
    *first = 0;
    if(activity_append_log_json(b, candidates[i].name, &candidates[i].st,
                                result, operation, target, error,
                                &summary) != 0) {
      break;
    }
  }
  free(candidates);
  return 0;
}


static int
activity_enqueue_handler(const http_request_t *req) {
  char client_id[ACTIVITY_CLIENT_MAX], local_id[ACTIVITY_CLIENT_MAX];
  char kind[ACTIVITY_KIND_MAX], name[ACTIVITY_NAME_MAX], dest[1024];
  uint64_t total_bytes = 0, total_items = 0;

  if(!websrv_get_query_arg(req, "clientId", client_id, sizeof(client_id)) ||
     !token_safe(client_id) ||
     !websrv_get_query_arg(req, "localId", local_id, sizeof(local_id)) ||
     !token_safe(local_id) ||
     !websrv_get_query_arg(req, "kind", kind, sizeof(kind)) ||
     !kind_safe(kind) ||
     !websrv_get_query_arg(req, "name", name, sizeof(name)) ||
     !name[0] ||
     !websrv_get_query_arg(req, "dest", dest, sizeof(dest)) ||
     !path_is_safe(dest)) {
    return serve_error(req, 400, "bad activity enqueue");
  }
  parse_upload_size_arg(req, "bytes", &total_bytes);
  parse_upload_size_arg(req, "items", &total_items);

  pthread_mutex_lock(&g_activity.lock);
  activity_item_t *slot = NULL;
  for(int i = 0; i < ACTIVITY_MAX; i++) {
    if(!g_activity.items[i].used) {
      slot = &g_activity.items[i];
      break;
    }
  }
  if(!slot) {
    for(int i = 0; i < ACTIVITY_MAX; i++) {
      if(g_activity.items[i].used &&
         activity_terminal(g_activity.items[i].status)) {
        slot = &g_activity.items[i];
        break;
      }
    }
  }
  if(!slot) {
    pthread_mutex_unlock(&g_activity.lock);
    return serve_error(req, 409, "activity queue is full");
  }

  memset(slot, 0, sizeof(*slot));
  slot->used = 1;
  slot->seq = g_activity.next_id++;
  slot->status = ACT_STATUS_QUEUED;
  activity_make_id(slot->queue_id, sizeof(slot->queue_id), "q", slot->seq);
  snprintf(slot->client_id, sizeof(slot->client_id), "%s", client_id);
  snprintf(slot->local_id, sizeof(slot->local_id), "%s", local_id);
  snprintf(slot->kind, sizeof(slot->kind), "%s", kind);
  snprintf(slot->display_name, sizeof(slot->display_name), "%s", name);
  snprintf(slot->dest_path, sizeof(slot->dest_path), "%s", dest);
  slot->total_bytes = zip_job_long(total_bytes);
  slot->total_items = total_items > (uint64_t)INT_MAX ? INT_MAX : (int)total_items;
  slot->created_at = slot->updated_at = slot->heartbeat_at = time(NULL);
  char queue_id[ACTIVITY_ID_MAX];
  snprintf(queue_id, sizeof(queue_id), "%s", slot->queue_id);
  pthread_mutex_unlock(&g_activity.lock);

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"queueId\":") != 0 ||
     json_string(&b, queue_id) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
activity_poll_handler(const http_request_t *req) {
  char client_id[ACTIVITY_CLIENT_MAX], owned[4096];
  char start_queue[ACTIVITY_ID_MAX] = {0};
  char start_lease[ACTIVITY_ID_MAX] = {0};
  int cancel_active = 0;
  time_t now = time(NULL);

  if(!websrv_get_query_arg(req, "clientId", client_id, sizeof(client_id)) ||
     !token_safe(client_id)) {
    return serve_error(req, 400, "bad activity client");
  }
  if(!websrv_get_query_arg(req, "owned", owned, sizeof(owned))) owned[0] = 0;
  int include_logs = query_arg_truthy(req, "logs", 1);
  int log_limit = parse_int_query_arg(req, "limit",
                                      ACTIVITY_LOG_LIMIT_DEFAULT, 0,
                                      ACTIVITY_LOG_LIMIT_MAX);

  pthread_mutex_lock(&g_activity.lock);
  for(int i = 0; i < ACTIVITY_MAX; i++) {
    activity_item_t *item = &g_activity.items[i];
    if(!item->used || activity_terminal(item->status)) continue;
    if(!strcmp(item->client_id, client_id)) {
      if(activity_owned_contains(owned, item->queue_id)) {
        item->heartbeat_at = now;
        item->updated_at = now;
      } else {
        int was_running = item->status == ACT_STATUS_RUNNING;
        activity_finish_locked(item, ACT_STATUS_ABANDONED,
                               "owning tab no longer has the local file",
                               NULL, -1, -1, -1, NULL);
        if(was_running) cancel_active = 1;
        continue;
      }
	    }
	    if(item->heartbeat_at > 0 &&
	       now - item->heartbeat_at > ACTIVITY_HEARTBEAT_TIMEOUT) {
	      int was_running = item->status == ACT_STATUS_RUNNING;
	      if(was_running) {
	        int job_busy = atomic_load(&g_job.busy);
	        if(job_busy || (item->started_at > 0 &&
	           now - item->started_at <= ACTIVITY_RUNNING_START_TIMEOUT)) {
	          continue;
	        }
	      }
	      activity_finish_locked(item, ACT_STATUS_ABANDONED,
	                             "owning tab stopped heartbeating",
	                             NULL, -1, -1, -1, NULL);
      if(was_running) cancel_active = 1;
    }
  }

  for(int i = 0; i < ACTIVITY_MAX; i++) {
    activity_item_t *item = &g_activity.items[i];
    if(item->used && item->status == ACT_STATUS_RUNNING &&
       !strcmp(item->client_id, client_id) && item->lease_token[0]) {
      snprintf(start_queue, sizeof(start_queue), "%s", item->queue_id);
      snprintf(start_lease, sizeof(start_lease), "%s", item->lease_token);
      break;
    }
  }

  if(!start_queue[0] && !activity_running_locked() && !atomic_load(&g_job.busy)) {
    activity_item_t *next = NULL;
    for(int i = 0; i < ACTIVITY_MAX; i++) {
      activity_item_t *item = &g_activity.items[i];
      if(!item->used || item->status != ACT_STATUS_QUEUED) continue;
      if(!next || item->seq < next->seq) next = item;
    }
    if(next && !strcmp(next->client_id, client_id)) {
      next->status = ACT_STATUS_RUNNING;
      next->started_at = next->updated_at = next->heartbeat_at = now;
      activity_make_id(next->lease_token, sizeof(next->lease_token),
                       "lease", g_activity.next_lease++);
      snprintf(start_queue, sizeof(start_queue), "%s", next->queue_id);
      snprintf(start_lease, sizeof(start_lease), "%s", next->lease_token);
    }
  }

  json_buf_t b = {0};
  if(json_appendf(&b, "{\"ok\":true,\"now\":%ld,\"start\":",
                  (long)now) != 0) {
    pthread_mutex_unlock(&g_activity.lock);
    free(b.data);
    return -1;
  }
  if(start_queue[0]) {
    if(json_append(&b, "{\"queueId\":") != 0 ||
       json_string(&b, start_queue) != 0 ||
       json_append(&b, ",\"leaseToken\":") != 0 ||
       json_string(&b, start_lease) != 0 ||
       json_append(&b, "}") != 0) {
      pthread_mutex_unlock(&g_activity.lock);
      free(b.data);
      return -1;
    }
  } else if(json_append(&b, "null") != 0) {
    pthread_mutex_unlock(&g_activity.lock);
    free(b.data);
    return -1;
  }
  if(json_append(&b, ",\"items\":[") != 0) {
    pthread_mutex_unlock(&g_activity.lock);
    free(b.data);
    return -1;
  }
  int first = 1;
  for(int pass = 0; pass < 2; pass++) {
    for(int i = 0; i < ACTIVITY_MAX; i++) {
      activity_item_t *item = &g_activity.items[i];
      if(!item->used) continue;
      if(pass == 0 && activity_terminal(item->status)) continue;
      if(pass == 1 && !activity_terminal(item->status)) continue;
      int position = 0;
      if(item->status == ACT_STATUS_QUEUED) {
        position = 1;
        for(int j = 0; j < ACTIVITY_MAX; j++) {
          activity_item_t *other = &g_activity.items[j];
          if(other->used && other->status == ACT_STATUS_QUEUED &&
             other->seq < item->seq) {
            position++;
          }
        }
      }
      if(!first && json_append(&b, ",") != 0) {
        pthread_mutex_unlock(&g_activity.lock);
        free(b.data);
        return -1;
      }
      first = 0;
      if(activity_append_json(&b, item, position,
                              !strcmp(item->client_id, client_id)) != 0) {
        pthread_mutex_unlock(&g_activity.lock);
        free(b.data);
        return -1;
      }
    }
  }
  pthread_mutex_unlock(&g_activity.lock);

  if(include_logs) activity_append_logs(&b, &first, log_limit);
  if(json_append(&b, "]}") != 0) {
    free(b.data);
    return -1;
  }
  if(cancel_active) {
    atomic_store(&g_job.cancel, 1);
    archive_upload_cancel_active();
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
activity_progress_handler(const http_request_t *req) {
  activity_request_ctx_t ctx;
  char err[128] = {0}, value[ACTIVITY_ERROR_MAX];
  uint64_t copied = 0, total = 0, done = 0, failed = 0;
  if(activity_validate_lease(req, NULL, &ctx, err, sizeof(err)) != 0 ||
     !ctx.queued) {
    if(activity_request_is_terminal(req)) {
      return activity_terminal_ok_response(req);
    }
    return serve_error(req, 409, err[0] ? err : "activity lease is required");
  }
  parse_upload_size_arg(req, "copied", &copied);
  parse_upload_size_arg(req, "total", &total);
  parse_upload_size_arg(req, "done", &done);
  parse_upload_size_arg(req, "failed", &failed);
  if(!websrv_get_query_arg(req, "error", value, sizeof(value))) value[0] = 0;

  pthread_mutex_lock(&g_activity.lock);
  activity_item_t *item = activity_find_locked(ctx.queue_id);
  if(item && item->status == ACT_STATUS_RUNNING) {
    if(total > 0) item->total_bytes = zip_job_long(total);
    item->copied_bytes = zip_job_long(copied);
    item->done_items = done > (uint64_t)INT_MAX ? INT_MAX : (int)done;
    item->failed_items = failed > (uint64_t)INT_MAX ? INT_MAX : (int)failed;
    if(value[0]) snprintf(item->error, sizeof(item->error), "%s", value);
    item->updated_at = item->heartbeat_at = time(NULL);
  }
  pthread_mutex_unlock(&g_activity.lock);

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
activity_complete_handler(const http_request_t *req) {
  activity_request_ctx_t ctx;
  char err[ACTIVITY_ERROR_MAX] = {0}, status[32], log_name[256] = {0};
  char existing_log[256] = {0}, log_path[1024] = {0};
  char current[512] = {0}, extra[1024] = {0};
  uint64_t copied = 0, total = 0, done = 0, failed = 0;
  int rc = -1;

  if(activity_validate_lease(req, NULL, &ctx, err, sizeof(err)) != 0 ||
     !ctx.queued) {
    if(activity_request_is_terminal(req)) {
      return activity_terminal_ok_response(req);
    }
    return serve_error(req, 409, err[0] ? err : "activity lease is required");
  }
  if(!websrv_get_query_arg(req, "status", status, sizeof(status))) {
    snprintf(status, sizeof(status), "failed");
  }
  if(!websrv_get_query_arg(req, "error", err, sizeof(err))) err[0] = 0;
  websrv_get_query_arg(req, "logName", existing_log, sizeof(existing_log));
  websrv_get_query_arg(req, "current", current, sizeof(current));
  websrv_get_query_arg(req, "extra", extra, sizeof(extra));
  parse_upload_size_arg(req, "copied", &copied);
  parse_upload_size_arg(req, "total", &total);
  parse_upload_size_arg(req, "done", &done);
  parse_upload_size_arg(req, "failed", &failed);
  rc = !strcmp(status, "success") ? 0 : -1;

  char target[1024] = {0}, kind[ACTIVITY_KIND_MAX] = {0}, name[ACTIVITY_NAME_MAX] = {0};
  pthread_mutex_lock(&g_activity.lock);
  activity_item_t *item = activity_find_locked(ctx.queue_id);
  if(item) {
    snprintf(target, sizeof(target), "%s", item->dest_path);
    snprintf(kind, sizeof(kind), "%s", item->kind);
    snprintf(name, sizeof(name), "%s", item->display_name);
  }
  pthread_mutex_unlock(&g_activity.lock);

  int done_items = done > (uint64_t)INT_MAX ? INT_MAX : (int)done;
  int failed_items = failed > (uint64_t)INT_MAX ? INT_MAX : (int)failed;
  int total_items = done_items + failed_items;
  if(total_items <= 0) total_items = rc == 0 ? 1 : 0;

  if(existing_log[0] && log_name_safe(existing_log) &&
     snprintf(log_path, sizeof(log_path), "%s/%s", OP_LOG_DIR,
              existing_log) < (int)sizeof(log_path)) {
    struct stat st;
    if(lstat(log_path, &st) == 0 && S_ISREG(st.st_mode)) {
      log_summary_t summary;
      read_log_summary(log_path, &summary);
      if(total == 0 && summary.total_bytes > 0) total = (uint64_t)summary.total_bytes;
      if(copied == 0 && summary.copied_bytes > 0) copied = (uint64_t)summary.copied_bytes;
      if(done_items == 0 && summary.done_items > 0) done_items = summary.done_items;
      if(total_items <= 1 && summary.total_items > 0) total_items = summary.total_items;
      if(failed_items == 0 && summary.failed_items > 0) failed_items = summary.failed_items;
      operation_log_write_at_extra(log_path, operation_result_name(rc, err),
                                   kind[0] ? kind : "upload",
                                   target[0] ? target : name,
                                   err[0] ? err : NULL,
                                   summary.started_at > 0 ? summary.started_at : time(NULL),
                                   time(NULL),
                                   zip_job_long(total), zip_job_long(copied),
                                   total_items, done_items, failed_items,
                                   current[0] ? current : (target[0] ? target : name),
                                   extra);
      snprintf(log_name, sizeof(log_name), "%s", existing_log);
    }
  }
  if(!log_name[0]) {
    operation_log_write_named(log_name, sizeof(log_name), kind[0] ? kind : "upload",
                              target[0] ? target : name, rc,
                              err[0] ? err : NULL, time(NULL), time(NULL),
                              zip_job_long(total), zip_job_long(copied),
                              total_items, done_items, failed_items,
                              current[0] ? current : (target[0] ? target : name));
  }
  if(!strcmp(status, "cancelled")) {
    pthread_mutex_lock(&g_activity.lock);
    activity_item_t *item = activity_find_locked(ctx.queue_id);
    if(item) {
      activity_finish_locked(item, ACT_STATUS_CANCELLED,
                             err[0] ? err : "cancelled", target,
                             zip_job_long(copied), done_items, failed_items,
                             log_name);
    }
    pthread_mutex_unlock(&g_activity.lock);
  } else {
    activity_finish_queue(ctx.queue_id, rc, err, target, zip_job_long(copied),
                          done_items, failed_items, log_name);
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
activity_cancel_handler(const http_request_t *req) {
  char queue_id[ACTIVITY_ID_MAX];
  int cancel_active = 0;
  if(!websrv_get_query_arg(req, "queueId", queue_id, sizeof(queue_id)) ||
     !token_safe(queue_id)) {
    return serve_error(req, 400, "bad activity id");
  }

  pthread_mutex_lock(&g_activity.lock);
  activity_item_t *item = activity_find_locked(queue_id);
  if(!item) {
    pthread_mutex_unlock(&g_activity.lock);
    return serve_error(req, 404, "activity item not found");
  }
  if(item->status == ACT_STATUS_QUEUED) {
    activity_finish_locked(item, ACT_STATUS_CANCELLED, "cancelled", NULL,
                           -1, -1, -1, NULL);
  } else if(item->status == ACT_STATUS_RUNNING) {
    activity_finish_locked(item, ACT_STATUS_CANCELLED, "cancelled", NULL,
                           -1, -1, -1, NULL);
    cancel_active = 1;
  }
  pthread_mutex_unlock(&g_activity.lock);

  if(cancel_active) {
    atomic_store(&g_job.cancel, 1);
    archive_upload_cancel_active();
  }
  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static void
activity_clear_terminal_items(void) {
  pthread_mutex_lock(&g_activity.lock);
  for(int i = 0; i < ACTIVITY_MAX; i++) {
    if(g_activity.items[i].used && activity_terminal(g_activity.items[i].status)) {
      memset(&g_activity.items[i], 0, sizeof(g_activity.items[i]));
    }
  }
  pthread_mutex_unlock(&g_activity.lock);
}


static int
activity_clear_handler(const http_request_t *req) {
  activity_clear_terminal_items();
  return logs_clear_handler(req);
}


static int
logs_list_handler(const http_request_t *req) {
  if(operation_log_dir_ready() != 0) {
    return serve_error(req, 500, strerror(errno));
  }

  DIR *d = opendir(OP_LOG_DIR);
  if(!d) return serve_error(req, 500, strerror(errno));

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"logs\":[") != 0) {
    closedir(d);
    free(b.data);
    return -1;
  }

  int first = 1;
  struct dirent *ent;
  while((ent = readdir(d))) {
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    if(!log_name_safe(ent->d_name)) continue;

    char path[1024];
    if(snprintf(path, sizeof(path), "%s/%s", OP_LOG_DIR, ent->d_name) >=
       (int)sizeof(path)) {
      continue;
    }
    struct stat st;
    if(lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

    char result[32], operation[32], target[256], error[256];
    if(read_log_field(path, "Result: ", result, sizeof(result)) != 0) {
      snprintf(result, sizeof(result), "unknown");
    }
    if(read_log_field(path, "Operation: ", operation, sizeof(operation)) != 0) {
      snprintf(operation, sizeof(operation), "operation");
    }
    if(read_log_field(path, "Target: ", target, sizeof(target)) != 0) {
      snprintf(target, sizeof(target), "-");
    }
    if(read_log_field(path, "Error: ", error, sizeof(error)) != 0) {
      snprintf(error, sizeof(error), "");
    }
    log_summary_t summary;
    read_log_summary(path, &summary);
    long started_at = summary.started_at > 0 ? summary.started_at :
                      (long)st.st_mtime;
    long ended_at = summary.ended_at > 0 ? summary.ended_at :
                    (long)st.st_mtime;

    if(!first && json_append(&b, ",") != 0) break;
    first = 0;
    if(json_append(&b, "{\"name\":") != 0 ||
       json_string(&b, ent->d_name) != 0 ||
       json_appendf(&b, ",\"size\":%ld,\"mtime\":%ld,\"result\":",
                    (long)st.st_size, (long)st.st_mtime) != 0 ||
       json_string(&b, result) != 0 ||
       json_append(&b, ",\"operation\":") != 0 ||
       json_string(&b, operation) != 0 ||
       json_append(&b, ",\"target\":") != 0 ||
       json_string(&b, target) != 0 ||
       json_appendf(&b,
        ",\"startedAt\":%ld,\"endedAt\":%ld,"
        "\"totalBytes\":%ld,\"copiedBytes\":%ld,"
        "\"totalItems\":%d,\"doneItems\":%d,\"failedItems\":%d",
        started_at, ended_at, summary.total_bytes, summary.copied_bytes,
        summary.total_items, summary.done_items, summary.failed_items) != 0 ||
       json_append(&b, ",\"error\":") != 0 ||
       json_string(&b, error) != 0 ||
       json_append(&b, "}") != 0) {
      break;
    }
  }
  closedir(d);

  if(json_append(&b, "]}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
logs_read_handler(const http_request_t *req) {
  char name[256];
  if(!websrv_get_query_arg(req, "name", name, sizeof(name)) ||
     !log_name_safe(name)) {
    return serve_error(req, 400, "bad log name");
  }

  char path[1024];
  if(snprintf(path, sizeof(path), "%s/%s", OP_LOG_DIR, name) >=
     (int)sizeof(path)) {
    return serve_error(req, 400, "log path too long");
  }

  FILE *f = fopen(path, "r");
  if(!f) return serve_error(req, 404, strerror(errno));
  char *content = malloc(OP_LOG_READ_MAX + 1);
  if(!content) {
    fclose(f);
    return serve_error(req, 500, "out of memory");
  }
  size_t n = fread(content, 1, OP_LOG_READ_MAX, f);
  int truncated = !feof(f);
  fclose(f);
  content[n] = 0;

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"name\":") != 0 ||
     json_string(&b, name) != 0 ||
     json_append(&b, ",\"content\":") != 0 ||
     json_string(&b, content) != 0 ||
     json_appendf(&b, ",\"truncated\":%s}", truncated ? "true" : "false") != 0) {
    free(content);
    free(b.data);
    return -1;
  }
  free(content);
  return serve_owned(req, 200, b.data, b.len);
}


static int
logs_clear_handler(const http_request_t *req) {
  int removed = 0;
  if(operation_log_dir_ready() == 0) {
    DIR *d = opendir(OP_LOG_DIR);
    if(d) {
      struct dirent *ent;
      while((ent = readdir(d))) {
        if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if(!log_name_safe(ent->d_name)) continue;
        char path[1024];
        if(snprintf(path, sizeof(path), "%s/%s", OP_LOG_DIR, ent->d_name) >=
           (int)sizeof(path)) {
          continue;
        }
        if(unlink(path) == 0) removed++;
      }
      closedir(d);
    }
  }
  if(unlink(RAR_LOG_PATH) == 0) removed++;

  json_buf_t b = {0};
  if(json_appendf(&b, "{\"ok\":true,\"removed\":%d}", removed) != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
upload_complete_handler(const http_request_t *req) {
  char final_path[1024];
  char check_path[1024];
  char err[200] = {0};
  int status = 400;
  uint64_t total = 0;
  activity_request_ctx_t activity = {0};

  if(build_upload_target(req, final_path, sizeof(final_path), err, sizeof(err),
                         &status) != 0) {
    operation_log_write_simple("upload", "-", -1,
                               err[0] ? err : "bad upload target", 0, 0);
    return serve_error(req, status, err[0] ? err : "bad upload target");
  }
  snprintf(check_path, sizeof(check_path), "%s", final_path);
  if(activity_validate_lease(req, "upload", &activity, err, sizeof(err)) != 0) {
    return serve_error(req, 409, err[0] ? err : "bad activity lease");
  }
  if(activity.queued &&
     build_queue_temp_path(final_path, activity.queue_id,
                           check_path, sizeof(check_path)) != 0) {
    snprintf(err, sizeof(err), "temp path: %s", strerror(errno));
    return serve_error(req, 500, err);
  }
  parse_upload_size_arg(req, "total", &total);

  struct stat st;
  if(lstat(check_path, &st) != 0 || !S_ISREG(st.st_mode)) {
    snprintf(err, sizeof(err), "%s", strerror(errno));
    if(!activity.queued) {
      operation_log_write_simple("upload", final_path, -1, err,
                                 (long)total, 0);
    }
    return serve_error(req, 404, err);
  }
  if(total > 0 && (uint64_t)st.st_size != total) {
    snprintf(err, sizeof(err), "upload size mismatch");
    if(activity.queued) {
      unlink(check_path);
    } else {
      operation_log_write_simple("upload", final_path, -1, err,
                                 (long)total, (long)st.st_size);
    }
    return serve_error(req, 500, err);
  }

  if(activity.queued) {
    if(rename(check_path, final_path) != 0) {
      snprintf(err, sizeof(err), "rename: %s", strerror(errno));
      unlink(check_path);
      return serve_error(req, 500, err);
    }
  } else {
    operation_log_write_simple("upload", final_path, 0, NULL,
                               total > 0 ? (long)total : (long)st.st_size,
                               (long)st.st_size);
  }
  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, final_path) != 0 ||
     json_appendf(&b, ",\"size\":%ld}", (long)st.st_size) != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


int
transfer_upload_request(const http_request_t *req, const char *initial_data,
                        size_t initial_size, size_t content_size) {
  char final_path[1024];
  char write_path[1024];
  char err[200] = {0};
  int status = 400;
  activity_request_ctx_t activity = {0};

  if(build_upload_target(req, final_path, sizeof(final_path), err, sizeof(err),
                         &status) != 0) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, status, err[0] ? err : "bad upload target");
  }
  snprintf(write_path, sizeof(write_path), "%s", final_path);
  if(activity_validate_lease(req, "upload", &activity, err, sizeof(err)) != 0) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 409, err[0] ? err : "bad activity lease");
  }
  if(activity.queued &&
     build_queue_temp_path(final_path, activity.queue_id,
                           write_path, sizeof(write_path)) != 0) {
    drain_body(req->fd, initial_size, content_size);
    snprintf(err, sizeof(err), "temp path: %s", strerror(errno));
    return serve_error(req, 500, err);
  }

  int out = open(write_path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  if(out < 0) {
    snprintf(err, sizeof(err), "%s", strerror(errno));
    drain_body(req->fd, initial_size, content_size);
    if(!activity.queued) {
      operation_log_write_simple("upload", final_path, -1, err,
                                 zip_job_long((uint64_t)content_size), 0);
    }
    return serve_error(req, 500, err);
  }

  char *buf = malloc(UPLOAD_BUF_SIZE);
  if(!buf) {
    close(out);
    drain_body(req->fd, initial_size, content_size);
    unlink(write_path);
    if(!activity.queued) {
      operation_log_write_simple("upload", final_path, -1, "out of memory",
                                 zip_job_long((uint64_t)content_size), 0);
    }
    return serve_error(req, 500, "out of memory");
  }

  int failed = 0;
  size_t bytes = 0;
  size_t remaining = content_size;

  if(initial_size > remaining) initial_size = remaining;
  if(initial_size > 0) {
    if(write_all_fd(out, initial_data, initial_size) != 0) {
      failed = 1;
      snprintf(err, sizeof(err), "write: %s", strerror(errno));
    } else {
      bytes += initial_size;
    }
    remaining -= initial_size;
  }

  while(remaining > 0) {
    size_t want = remaining < UPLOAD_BUF_SIZE ? remaining : UPLOAD_BUF_SIZE;
    ssize_t n = recv(req->fd, buf, want, 0);
    if(n < 0) {
      if(errno == EINTR) continue;
      failed = 1;
      snprintf(err, sizeof(err), "recv: %s", strerror(errno));
      break;
    }
    if(n == 0) {
      failed = 1;
      snprintf(err, sizeof(err), "short upload");
      break;
    }
    remaining -= (size_t)n;
    if(!failed) {
      if(write_all_fd(out, buf, (size_t)n) != 0) {
        failed = 1;
        snprintf(err, sizeof(err), "write: %s", strerror(errno));
      } else {
        bytes += (size_t)n;
      }
    }
  }

  free(buf);
  close(out);

  if(failed) {
    drain_body(req->fd, 0, remaining);
    unlink(write_path);
    if(!activity.queued) {
      operation_log_write_simple("upload", final_path, -1,
                                 err[0] ? err : "upload failed",
                                 zip_job_long((uint64_t)content_size),
                                 zip_job_long((uint64_t)bytes));
    }
    return serve_error(req, 500, err[0] ? err : "upload failed");
  }
  if(activity.queued) {
    if(rename(write_path, final_path) != 0) {
      snprintf(err, sizeof(err), "rename: %s", strerror(errno));
      unlink(write_path);
      return serve_error(req, 500, err);
    }
  } else {
    operation_log_write_simple("upload", final_path, 0, NULL,
                               zip_job_long((uint64_t)content_size),
                               zip_job_long((uint64_t)bytes));
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, final_path) != 0 ||
     json_appendf(&b, ",\"size\":%lu}", (unsigned long)bytes) != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


int
transfer_upload_chunk_request(const http_request_t *req,
                              const char *initial_data, size_t initial_size,
                              size_t content_size) {
  char final_path[1024];
  char write_path[1024];
  char err[200] = {0};
  int status = 400;
  uint64_t offset_u64 = 0;
  uint64_t total_u64 = 0;
  activity_request_ctx_t activity = {0};

  if(build_upload_target(req, final_path, sizeof(final_path), err, sizeof(err),
                         &status) != 0) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, status, err[0] ? err : "bad upload target");
  }
  snprintf(write_path, sizeof(write_path), "%s", final_path);
  if(activity_validate_lease(req, "upload", &activity, err, sizeof(err)) != 0) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 409, err[0] ? err : "bad activity lease");
  }
  if(activity.queued &&
     build_queue_temp_path(final_path, activity.queue_id,
                           write_path, sizeof(write_path)) != 0) {
    drain_body(req->fd, initial_size, content_size);
    snprintf(err, sizeof(err), "temp path: %s", strerror(errno));
    return serve_error(req, 500, err);
  }

  if(!parse_upload_size_arg(req, "offset", &offset_u64) ||
     !parse_upload_size_arg(req, "total", &total_u64) ||
     total_u64 == 0 ||
     content_size == 0 ||
     content_size > UPLOAD_CHUNK_MAX ||
     offset_u64 > total_u64 ||
     (uint64_t)content_size > total_u64 - offset_u64) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 400, "bad upload chunk");
  }

  int out = open(write_path, O_WRONLY | O_CREAT, 0777);
  if(out < 0) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 500, strerror(errno));
  }

  if(offset_u64 == 0 && ftruncate(out, (off_t)total_u64) != 0) {
    snprintf(err, sizeof(err), "truncate: %s", strerror(errno));
    close(out);
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 500, err);
  }

  char *buf = malloc(UPLOAD_BUF_SIZE);
  if(!buf) {
    close(out);
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 500, "out of memory");
  }

  int failed = 0;
  size_t bytes = 0;
  size_t remaining = content_size;
  off_t write_offset = (off_t)offset_u64;

  if(initial_size > remaining) initial_size = remaining;
  if(initial_size > 0) {
    if(pwrite_all_fd(out, initial_data, initial_size, write_offset) != 0) {
      failed = 1;
      snprintf(err, sizeof(err), "write: %s", strerror(errno));
    } else {
      bytes += initial_size;
      write_offset += (off_t)initial_size;
    }
    remaining -= initial_size;
  }

  while(remaining > 0) {
    size_t want = remaining < UPLOAD_BUF_SIZE ? remaining : UPLOAD_BUF_SIZE;
    ssize_t n = recv(req->fd, buf, want, 0);
    if(n < 0) {
      if(errno == EINTR) continue;
      failed = 1;
      snprintf(err, sizeof(err), "recv: %s", strerror(errno));
      break;
    }
    if(n == 0) {
      failed = 1;
      snprintf(err, sizeof(err), "short upload");
      break;
    }
    remaining -= (size_t)n;
    if(!failed) {
      if(pwrite_all_fd(out, buf, (size_t)n, write_offset) != 0) {
        failed = 1;
        snprintf(err, sizeof(err), "write: %s", strerror(errno));
      } else {
        bytes += (size_t)n;
        write_offset += (off_t)n;
      }
    }
  }

  free(buf);
  close(out);

  if(failed) {
    drain_body(req->fd, 0, remaining);
    if(activity.queued) {
      unlink(write_path);
    } else {
      operation_log_write_simple("upload", final_path, -1,
                                 err[0] ? err : "upload chunk failed",
                                 zip_job_long(total_u64),
                                 zip_job_long(offset_u64 + bytes));
    }
    return serve_error(req, 500, err[0] ? err : "upload chunk failed");
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, final_path) != 0 ||
     json_appendf(&b, ",\"offset\":%llu,\"size\":%lu,\"total\":%llu}",
                  (unsigned long long)offset_u64, (unsigned long)bytes,
                  (unsigned long long)total_u64) != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}
