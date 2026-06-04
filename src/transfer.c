/*
 * BFpilot - file-manager copy/move/delete primitives and API.
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "archive_common.h"
#include "rar_transfer.h"
#include "transfer.h"
#include "transfer_internal.h"
#include "websrv.h"
#include "zip_archive.h"


#define COPY_BUF_SIZE   (1024 * 1024)
#define UPLOAD_BUF_SIZE (1024 * 1024)
#define UPLOAD_CHUNK_MAX (16 * 1024 * 1024)
#define OP_LOG_DIR "/data/BFpilot/logs"
#define OP_LOG_READ_MAX (512 * 1024)
#define ACTIVITY_MAX 128
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
  mkdir("/data/BFpilot", 0777);
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
operation_log_write_at(const char *path, const char *result, const char *verb,
                       const char *target, const char *err,
                       time_t started_at, time_t ended_at, long total_bytes,
                       long copied_bytes, int total_files, int done_files,
                       int failed_files, const char *current) {
  if(!path || !*path) return;
  if(!started_at) started_at = ended_at ? ended_at : time(NULL);
  if(!ended_at) ended_at = time(NULL);

  FILE *f = fopen(path, "w");
  if(!f) return;
  fprintf(f, "BFpilot operation log\n");
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
  fclose(f);
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


void
job_end(int rc, const char *err) {
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
    operation_log_write_at(log_path, operation_result_name(rc, final_err), verb,
                           target, final_err, started_at, ended_at,
                           total_bytes, copied_bytes, total_files, done_files,
                           failed_files, current);
  } else {
    operation_log_write(verb, target, rc, final_err, started_at, ended_at,
                        total_bytes, copied_bytes, total_files, done_files,
                        failed_files, current);
  }
  atomic_store(&g_job.busy, 0);
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
  char   log_path[1024];
  int    is_move;
  long   total_bytes;
  long   copied_bytes;
  int    total_files;
  int    done_files;
  int    failed_files;
  time_t started_at;
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
bg_file_op_write_running(bg_file_op_t *op) {
  if(!op) return;
  operation_log_write_at(op->log_path, "running", op->verb,
                         bg_file_op_target(op), "operation still running",
                         op->started_at, op->started_at, 0, 0, 0, 0, 0,
                         bg_file_op_target(op));
}


static void
bg_file_op_finish(bg_file_op_t *op, int rc) {
  time_t ended_at = time(NULL);
  if(!op) return;
  if(rc != 0 && op->failed_files <= 0) op->failed_files = 1;
  operation_log_write_at(op->log_path, rc == 0 ? "success" : "failed",
                         op->verb, bg_file_op_target(op),
                         rc == 0 ? NULL :
                           (op->error[0] ? op->error : "operation failed"),
                         op->started_at, ended_at, op->total_bytes,
                         op->copied_bytes, op->total_files, op->done_files,
                         op->failed_files,
                         op->current[0] ? op->current : bg_file_op_target(op));
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
usb_request(const http_request_t *req) {
  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"mounts\":[") != 0) return -1;
  int first = 1;
  for(int i = 0; i < 8; i++) {
    char path[24];
    snprintf(path, sizeof(path), "/mnt/usb%d", i);
    struct stat st;
    if(stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    char probe[40];
    snprintf(probe, sizeof(probe), "%s/.el_probe", path);
    int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int writable = 0;
    if(fd >= 0) {
      writable = 1;
      close(fd);
      unlink(probe);
    }
    if(!first && json_append(&b, ",") != 0) break;
    first = 0;
    if(json_append(&b, "{\"path\":") != 0 ||
       json_string(&b, path) != 0 ||
       json_appendf(&b, ",\"writable\":%s}", writable ? "true" : "false") != 0) {
      break;
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
      tb, cb, tf, df, ff, (long)started_at, (long)ended_at,
      elapsed, speed, eta) != 0) {
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
                         const char *target, const char *error) {
  char id[320];
  json_buf_t *jb = b;
  snprintf(id, sizeof(id), "log:%s", name ? name : "");
  if(json_append(jb, "{\"source\":\"log\",\"queueId\":") != 0 ||
     json_string(jb, id) != 0 ||
     json_append(jb, ",\"clientId\":\"\",\"localId\":\"\",\"kind\":") != 0 ||
     json_string(jb, operation) != 0 ||
     json_append(jb, ",\"operation\":") != 0 ||
     json_string(jb, operation) != 0 ||
     json_append(jb, ",\"displayName\":") != 0 ||
     json_string(jb, operation) != 0 ||
     json_append(jb, ",\"target\":") != 0 ||
     json_string(jb, target) != 0 ||
     json_append(jb, ",\"status\":") != 0 ||
     json_string(jb, result) != 0 ||
     json_append(jb, ",\"result\":") != 0 ||
     json_string(jb, result) != 0 ||
     json_appendf(jb,
       ",\"position\":0,\"totalBytes\":0,\"copiedBytes\":0,"
       "\"totalItems\":0,\"doneItems\":0,\"failedItems\":0,"
       "\"createdAt\":%ld,\"updatedAt\":%ld,\"startedAt\":0,"
       "\"endedAt\":%ld,\"mtime\":%ld,\"owned\":false",
       (long)st->st_mtime, (long)st->st_mtime,
       (long)st->st_mtime, (long)st->st_mtime) != 0 ||
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
activity_append_logs(json_buf_t *b, int *first) {
  if(operation_log_dir_ready() != 0) return 0;
  DIR *d = opendir(OP_LOG_DIR);
  if(!d) return 0;

  struct dirent *ent;
  while((ent = readdir(d))) {
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    if(!log_name_safe(ent->d_name)) continue;

    pthread_mutex_lock(&g_activity.lock);
    int skip = activity_queue_has_log_locked(ent->d_name);
    pthread_mutex_unlock(&g_activity.lock);
    if(skip) continue;

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

    if(!*first && json_append(b, ",") != 0) break;
    *first = 0;
    if(activity_append_log_json(b, ent->d_name, &st, result, operation,
                                target, error) != 0) {
      break;
    }
  }
  closedir(d);
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

  activity_append_logs(&b, &first);
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

  operation_log_write_named(log_name, sizeof(log_name), kind[0] ? kind : "upload",
                            target[0] ? target : name, rc,
                            err[0] ? err : NULL, time(NULL), time(NULL),
                            zip_job_long(total), zip_job_long(copied),
                            total_items, done_items, failed_items,
                            target[0] ? target : name);
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
