/*
 * File Explorer - RAR archive upload and extraction requests.
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
#include "rar_extract.h"
#include "rar_transfer.h"
#include "transfer.h"
#include "transfer_internal.h"
#include "websrv.h"
#include "zip_archive.h"

typedef struct upload_body_reader {
  int          fd;
  const char  *initial;
  size_t      initial_pos;
  size_t      initial_size;
  uint64_t    remaining;
  uint64_t    consumed;
} upload_body_reader_t;

typedef struct rar_volume_info {
  char     name[256];
  uint64_t size;
} rar_volume_info_t;

typedef struct rar_upload_manifest {
  char              archive_name[256];
  char              password[RAR_PASSWORD_MAX];
  int               volume_count;
  uint64_t          data_size;
  rar_volume_info_t volumes[RAR_VOLUME_MAX];
} rar_upload_manifest_t;

typedef struct rar_upload_ctx {
  zip_pipe_t *pipe;
  uint64_t    archive_remaining;
  int         logged_first_read;
  char        err[200];
} rar_upload_ctx_t;




static int
rar_error_is_password(const char *err) {
  if(!err || !*err) return 0;
  return strstr(err, "password required") ||
         strstr(err, "missing password") ||
         strstr(err, "bad rar password") ||
         strstr(err, "bad password");
}


static int
rar_error_is_interrupted(const char *err) {
  if(!err || !*err) return 0;
  return strstr(err, "upload interrupted") ||
         strstr(err, "short upload") ||
         strstr(err, "short rar upload") ||
         strstr(err, "recv:");
}


static void
rar_set_interrupted_error(char *err, size_t err_size, const char *detail) {
  if(err_size == 0) return;
  if(detail && operation_error_is_cancelled(detail)) {
    snprintf(err, err_size, "cancelled");
    return;
  }
  if(detail && *detail && !strstr(detail, "short upload") &&
     !strstr(detail, "short rar upload")) {
    snprintf(err, err_size, "upload interrupted: %s", detail);
    return;
  }
  snprintf(err, err_size, "upload interrupted");
}


static void
rar_normalize_stream_error(char *err, size_t err_size, rar_upload_ctx_t *ctx,
                           zip_pipe_t *pipe) {
  char pipe_err[200] = {0};
  const char *ctx_err = ctx ? ctx->err : NULL;
  if(!err || err_size == 0) return;

  if(pipe) zip_pipe_copy_error(pipe, pipe_err, sizeof(pipe_err));
  if(job_cancelled() || operation_error_is_cancelled(ctx_err) ||
     operation_error_is_cancelled(pipe_err)) {
    snprintf(err, err_size, "cancelled");
    return;
  }
  if(pipe_err[0]) {
    rar_set_interrupted_error(err, err_size, pipe_err);
    return;
  }
  if(ctx_err && ctx_err[0]) {
    if(rar_error_is_interrupted(ctx_err)) {
      rar_set_interrupted_error(err, err_size, ctx_err);
    } else {
      snprintf(err, err_size, "%s", ctx_err);
    }
  }
}

static pthread_mutex_t g_rar_log_lock = PTHREAD_MUTEX_INITIALIZER;


static void
rar_log_event(const char *fmt, ...) {
  pthread_mutex_lock(&g_rar_log_lock);

  mkdir("/data/FileExplorer", 0777);
  FILE *f = fopen(RAR_LOG_PATH, "a");
  if(f) {
    time_t now = time(NULL);
    fprintf(f, "%ld ", (long)now);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fputc('\n', f);
    fclose(f);
  }

  pthread_mutex_unlock(&g_rar_log_lock);
}


static void
body_reader_init(upload_body_reader_t *r, int fd, const char *initial,
                 size_t initial_size, size_t content_size) {
  memset(r, 0, sizeof(*r));
  r->fd = fd;
  r->initial = initial;
  r->initial_size = initial_size > content_size ? content_size : initial_size;
  r->remaining = (uint64_t)content_size;
}


static uint16_t
rar_le16(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}


static uint64_t
rar_le64(const unsigned char *p) {
  uint64_t lo = (uint64_t)p[0] |
                ((uint64_t)p[1] << 8) |
                ((uint64_t)p[2] << 16) |
                ((uint64_t)p[3] << 24);
  uint64_t hi = (uint64_t)p[4] |
                ((uint64_t)p[5] << 8) |
                ((uint64_t)p[6] << 16) |
                ((uint64_t)p[7] << 24);
  return lo | (hi << 32);
}


static int
body_reader_read(upload_body_reader_t *r, void *out, size_t size,
                 size_t *read_size) {
  unsigned char *dst = out;
  size_t done = 0;

  if(read_size) *read_size = 0;
  if(job_cancelled()) return -1;
  if(size == 0 || r->remaining == 0) return 0;
  if(size > r->remaining) size = (size_t)r->remaining;

  if(r->initial_pos < r->initial_size) {
    size_t avail = r->initial_size - r->initial_pos;
    size_t take = avail < size ? avail : size;
    memcpy(dst, r->initial + r->initial_pos, take);
    r->initial_pos += take;
    r->remaining -= take;
    r->consumed += take;
    done += take;
  }

  while(done < size) {
    if(job_cancelled()) return -1;
    ssize_t n = recv(r->fd, dst + done, size - done, 0);
    if(n < 0) {
      if(errno == EINTR) continue;
      return -1;
    }
    if(n == 0) break;
    done += (size_t)n;
    r->remaining -= (uint64_t)n;
    r->consumed += (uint64_t)n;
    break;
  }

  if(read_size) *read_size = done;
  return 0;
}


static int
body_reader_read_exact(upload_body_reader_t *r, void *out, size_t size) {
  unsigned char *dst = out;
  size_t done = 0;
  while(done < size) {
    size_t n = 0;
    if(body_reader_read(r, dst + done, size - done, &n) != 0) return -1;
    if(n == 0) {
      errno = EMSGSIZE;
      return -1;
    }
    done += n;
  }
  return 0;
}


static void
body_reader_drain(upload_body_reader_t *r, int count_progress) {
  unsigned char buf[8192];
  while(r->remaining > 0) {
    size_t n = 0;
    size_t want = r->remaining < sizeof(buf) ? (size_t)r->remaining : sizeof(buf);
    if(body_reader_read(r, buf, want, &n) != 0 || n == 0) break;
    if(count_progress) {
      atomic_fetch_add(&g_job.copied_bytes, zip_job_long((uint64_t)n));
    }
  }
}


static int
rar_read_string(upload_body_reader_t *r, char *out, size_t out_size,
                uint16_t len) {
  if(out_size == 0 || len >= out_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if(body_reader_read_exact(r, out, len) != 0) return -1;
  out[len] = 0;
  return 0;
}


static int
rar_parse_manifest(upload_body_reader_t *r, rar_upload_manifest_t *m,
                   char *err, size_t err_size) {
  unsigned char hdr[24];
  uint16_t archive_len, password_len, flags;
  uint64_t sum = 0;

  memset(m, 0, sizeof(*m));
  if(body_reader_read_exact(r, hdr, sizeof(hdr)) != 0) {
    snprintf(err, err_size, "short rar upload header");
    return -1;
  }
  if(memcmp(hdr, RAR_UPLOAD_MAGIC, 8) != 0) {
    snprintf(err, err_size, "bad rar upload header");
    return -1;
  }

  m->volume_count = (int)rar_le16(hdr + 8);
  archive_len = rar_le16(hdr + 10);
  password_len = rar_le16(hdr + 12);
  flags = rar_le16(hdr + 14);
  m->data_size = rar_le64(hdr + 16);

  if(flags != 0 || m->volume_count <= 0 ||
     m->volume_count > RAR_VOLUME_MAX || archive_len == 0 ||
     archive_len >= sizeof(m->archive_name) ||
     password_len >= sizeof(m->password) ||
     m->data_size == 0) {
    snprintf(err, err_size, "bad rar upload manifest");
    return -1;
  }
  if(rar_read_string(r, m->archive_name, sizeof(m->archive_name),
                     archive_len) != 0 ||
     rar_read_string(r, m->password, sizeof(m->password),
                     password_len) != 0) {
    snprintf(err, err_size, "bad rar upload manifest");
    return -1;
  }
  if(!upload_segment_safe(m->archive_name)) {
    snprintf(err, err_size, "bad rar filename");
    return -1;
  }

  for(int i = 0; i < m->volume_count; i++) {
    unsigned char vh[10];
    uint16_t name_len;
    if(body_reader_read_exact(r, vh, sizeof(vh)) != 0) {
      snprintf(err, err_size, "short rar volume manifest");
      return -1;
    }
    name_len = rar_le16(vh);
    m->volumes[i].size = rar_le64(vh + 2);
    if(name_len == 0 || name_len >= sizeof(m->volumes[i].name) ||
       m->volumes[i].size == 0 ||
       rar_read_string(r, m->volumes[i].name, sizeof(m->volumes[i].name),
                       name_len) != 0 ||
       !upload_segment_safe(m->volumes[i].name)) {
      snprintf(err, err_size, "bad rar volume manifest");
      return -1;
    }
    if(UINT64_MAX - sum < m->volumes[i].size) {
      snprintf(err, err_size, "rar upload is too large");
      return -1;
    }
    sum += m->volumes[i].size;
  }

  if(sum != m->data_size || r->remaining != m->data_size) {
    snprintf(err, err_size, "rar upload size mismatch");
    return -1;
  }
  return 0;
}


static int
rar_stream_read_cb(void *opaque, void *data, size_t size, size_t *read_size) {
  rar_upload_ctx_t *ctx = opaque;
  ssize_t n;

  if(read_size) *read_size = 0;
  if(job_cancelled()) {
    snprintf(ctx->err, sizeof(ctx->err), "cancelled");
    return -1;
  }
  if(ctx->archive_remaining == 0) return 0;
  if(size > ctx->archive_remaining) size = (size_t)ctx->archive_remaining;
  n = zip_pipe_read(ctx->pipe, data, size);
  if(n < 0) {
    zip_pipe_copy_error(ctx->pipe, ctx->err, sizeof(ctx->err));
    if(!ctx->err[0]) {
      snprintf(ctx->err, sizeof(ctx->err), "rar upload failed");
    }
    return -1;
  }
  if(n == 0) {
    zip_pipe_copy_error(ctx->pipe, ctx->err, sizeof(ctx->err));
    if(!ctx->err[0]) {
      snprintf(ctx->err, sizeof(ctx->err), "%s",
               job_cancelled() ? "cancelled" : "short rar upload");
    }
    return -1;
  }

  ctx->archive_remaining -= (uint64_t)n;
  if(!ctx->logged_first_read) {
    ctx->logged_first_read = 1;
    rar_log_event("stream first-read bytes=%ld remaining=%llu", (long)n,
                  (unsigned long long)ctx->archive_remaining);
  }
  if(read_size) *read_size = (size_t)n;
  return 0;
}


static int
rar_stream_cancel_cb(void *opaque) {
  rar_upload_ctx_t *ctx = opaque;
  if(job_cancelled()) {
    if(ctx && ctx->pipe) zip_pipe_cancel(ctx->pipe);
    return 1;
  }
  return 0;
}


static int
rar_pipe_drain_remaining(rar_upload_ctx_t *ctx) {
  unsigned char *buf = malloc(ZIP_PIPE_READ_SIZE);
  if(!buf) {
    snprintf(ctx->err, sizeof(ctx->err), "out of memory");
    return -1;
  }

  while(ctx->archive_remaining > 0) {
    size_t want = ctx->archive_remaining < ZIP_PIPE_READ_SIZE ?
                    (size_t)ctx->archive_remaining : ZIP_PIPE_READ_SIZE;
    ssize_t n = zip_pipe_read(ctx->pipe, buf, want);
    if(n < 0) {
      zip_pipe_copy_error(ctx->pipe, ctx->err, sizeof(ctx->err));
      if(!ctx->err[0]) snprintf(ctx->err, sizeof(ctx->err), "rar upload failed");
      free(buf);
      return -1;
    }
    if(n == 0) {
      zip_pipe_copy_error(ctx->pipe, ctx->err, sizeof(ctx->err));
      if(!ctx->err[0]) {
        snprintf(ctx->err, sizeof(ctx->err), "%s",
                 job_cancelled() ? "cancelled" : "short rar upload");
      }
      free(buf);
      return -1;
    }
    ctx->archive_remaining -= (uint64_t)n;
  }

  free(buf);
  return 0;
}


static int
rar_error_status(const char *err) {
  if(rar_error_is_password(err)) return 401;
  if(job_cancelled() || operation_error_is_cancelled(err)) return 409;
  if(rar_error_is_interrupted(err)) return 400;
  return 500;
}


static int
rar_job_cancel_cb(void *opaque) {
  (void)opaque;
  return job_cancelled();
}


static int
extract_local_rar(const http_request_t *req, const char *archive_path,
                  const char *dest, const struct stat *st,
                  const char *password) {
  rar_extract_opts_t opts;
  char stage[1024] = {0};
  char final_base[1024] = {0};
  char err[240] = {0};
  int rc = -1;
  long final_files = 0;

  if(!job_begin("unrar")) {
    return serve_error(req, 409, "another file-manager job is already running");
  }
  job_set_target(dest);
  atomic_store(&g_job.total_bytes, zip_job_long((uint64_t)st->st_size));
  atomic_store(&g_job.copied_bytes, zip_job_long((uint64_t)st->st_size));
  job_set_current("Opening RAR");

  if(mkdirs(dest) != 0) {
    snprintf(err, sizeof(err), "destination: %s", strerror(errno));
    goto done;
  }
  if(rar_make_stage_path(dest, stage, sizeof(stage)) != 0) {
    snprintf(err, sizeof(err), "stage: %s", strerror(errno));
    goto done;
  }

  memset(&opts, 0, sizeof(opts));
  opts.archive_name = archive_path;
  opts.dest_dir = stage;
  opts.password = password;
  opts.dictionary_limit = RAR_DICT_LIMIT;
  opts.cancel_cb = rar_job_cancel_cb;

  job_set_current("Extracting RAR");
  rc = rar_extract_file(&opts, err, sizeof(err));
  if(rc != 0 && job_cancelled()) snprintf(err, sizeof(err), "cancelled");

  if(rc == 0 &&
     archive_place_stage(stage, path_basename(archive_path), dest,
                         final_base, sizeof(final_base), &final_files,
                         err, sizeof(err)) != 0) {
    rc = -1;
  }
  if(rc == 0) {
    job_set_target(final_base);
  }

done:
  if(stage[0]) rar_delete_tree(stage);
  job_end(rc, rc == 0 ? NULL : (err[0] ? err : "rar extraction failed"));

  if(rc != 0) {
    return serve_error(req, rar_error_status(err),
                       err[0] ? err : "rar extraction failed");
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, final_base) != 0 ||
     json_appendf(&b, ",\"files\":%ld}", final_files) != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


int
extract_archive_handler(const http_request_t *req) {
  char archive_path[1024];
  char dest[1024];
  char password[RAR_PASSWORD_MAX] = {0};
  struct stat st;

  if(!websrv_get_query_arg(req, "path", archive_path, sizeof(archive_path)) ||
     !path_is_safe(archive_path) ||
     !websrv_get_query_arg(req, "dest", dest, sizeof(dest)) ||
     !path_is_safe(dest)) {
    return serve_error(req, 400, "bad archive extraction request");
  }
  websrv_get_query_arg(req, "password", password, sizeof(password));
  if(lstat(archive_path, &st) != 0) {
    return serve_error(req, 404, strerror(errno));
  }
  if(!S_ISREG(st.st_mode)) {
    return serve_error(req, 400, "archive path is not a file");
  }

  if(archive_ext_is_zip(archive_path)) {
    return extract_local_zip(req, archive_path, dest, &st);
  }
  if(archive_ext_is_rar_first_volume(archive_path)) {
    return extract_local_rar(req, archive_path, dest, &st, password);
  }
  const char *base = path_basename(archive_path);
  size_t n = strlen(base);
  if(strstr(base, ".part") ||
     (n > 4 && base[n - 4] == '.' &&
      tolower((unsigned char)base[n - 3]) == 'r' &&
      isdigit((unsigned char)base[n - 2]) &&
      isdigit((unsigned char)base[n - 1]))) {
    return serve_error(req, 400, "open the first archive volume");
  }
  return serve_error(req, 400, "unsupported archive type");
}


int
transfer_upload_rar_request(const http_request_t *req, const char *initial_data,
                            size_t initial_size, size_t content_size) {
  upload_body_reader_t reader;
  rar_upload_manifest_t manifest;
  rar_upload_ctx_t ctx;
  rar_extract_opts_t opts;
  zip_pipe_t pipe;
  pthread_t producer;
  char dest[1024], filename[256];
  char stage[1024], final_base[1024];
  char err[240] = {0};
  char lease_err[160] = {0};
  char defer_arg[16] = {0};
  char log_name[256] = {0};
  activity_request_ctx_t activity = {0};
  int pipe_ready = 0;
  int producer_started = 0;
  int rc = -1;
  int defer_activity = 0;
  int activity_deferred = 0;
  long final_files = 0;

  body_reader_init(&reader, req->fd, initial_data, initial_size, content_size);
  memset(&pipe, 0, sizeof(pipe));
  stage[0] = final_base[0] = 0;
  rar_log_event("request start content=%llu initial=%llu",
                (unsigned long long)content_size,
                (unsigned long long)initial_size);

  if(content_size == 0) {
    rar_log_event("request reject empty");
    body_reader_drain(&reader, 0);
    return serve_error(req, 400, "empty rar upload");
  }
  if(activity_validate_lease(req, "rar", &activity, lease_err,
                             sizeof(lease_err)) != 0) {
    rar_log_event("request reject bad-lease err=%s", lease_err);
    body_reader_drain(&reader, 0);
    return serve_error(req, 409, lease_err[0] ? lease_err : "bad activity lease");
  }
  if(websrv_get_query_arg(req, "deferActivity", defer_arg,
                          sizeof(defer_arg)) &&
     strcmp(defer_arg, "0") != 0) {
    defer_activity = 1;
  }
  if(!websrv_get_query_arg(req, "path", dest, sizeof(dest)) ||
     !path_is_safe(dest) ||
     !websrv_get_query_arg(req, "filename", filename, sizeof(filename)) ||
     !upload_segment_safe(filename)) {
    rar_log_event("request reject bad-target");
    body_reader_drain(&reader, 0);
    if(activity.queued) {
      activity_finish_queue(activity.queue_id, -1, "bad rar upload target",
                            "-", 0, 0, 1, NULL);
    }
    return serve_error(req, 400, "bad rar upload target");
  }
  if(!job_begin("unrar")) {
    rar_log_event("request reject busy");
    body_reader_drain(&reader, 0);
    if(activity.queued) {
      activity_finish_queue(activity.queue_id, -1,
                            "another file-manager job is already running",
                            dest, 0, 0, 1, NULL);
    }
    return serve_error(req, 409, "another file-manager job is already running");
  }
  job_set_target(dest);
  archive_upload_set_active(NULL, req->fd);

  atomic_store(&g_job.total_bytes, zip_job_long((uint64_t)content_size));
  atomic_store(&g_job.copied_bytes, 0);
  job_set_current("Reading RAR manifest");

  if(rar_parse_manifest(&reader, &manifest, err, sizeof(err)) != 0) {
    int was_cancelled = job_cancelled();
    if(was_cancelled) snprintf(err, sizeof(err), "cancelled");
    rar_log_event("manifest error consumed=%llu remaining=%llu err=%s",
                  (unsigned long long)reader.consumed,
                  (unsigned long long)reader.remaining,
                  err[0] ? err : "bad rar upload manifest");
    if(!was_cancelled) body_reader_drain(&reader, 1);
    archive_upload_clear_active(NULL, req->fd);
    job_end(-1, err[0] ? err : "bad rar upload manifest");
    if(activity.queued) {
      char log_name[256];
      job_log_name(log_name, sizeof(log_name));
      activity_finish_queue(activity.queue_id, -1,
                            err[0] ? err : "bad rar upload manifest",
                            dest, atomic_load(&g_job.copied_bytes), 0, 1,
                            log_name);
    }
    return serve_error(req, was_cancelled ? 409 : 400,
                       err[0] ? err : "bad rar upload manifest");
  }
  atomic_store(&g_job.copied_bytes, zip_job_long(reader.consumed));
  atomic_store(&g_job.total_files, manifest.volume_count);
  rar_log_event("manifest ok archive=%s volumes=%d data=%llu password=%s consumed=%llu buffered=%llu",
                manifest.archive_name, manifest.volume_count,
                (unsigned long long)manifest.data_size,
                manifest.password[0] ? "yes" : "no",
                (unsigned long long)reader.consumed,
                (unsigned long long)(reader.initial_size - reader.initial_pos));

  if(reader.remaining > (uint64_t)SIZE_MAX) {
    body_reader_drain(&reader, 1);
    snprintf(err, sizeof(err), "rar upload is too large");
    rar_log_event("request reject too-large remaining=%llu",
                  (unsigned long long)reader.remaining);
    archive_upload_clear_active(NULL, req->fd);
    job_end(-1, err);
    if(activity.queued) {
      char log_name[256];
      job_log_name(log_name, sizeof(log_name));
      activity_finish_queue(activity.queue_id, -1, err, dest,
                            atomic_load(&g_job.copied_bytes), 0, 1,
                            log_name);
    }
    return serve_error(req, 400, err);
  }

  if(rar_make_stage_path(dest, stage, sizeof(stage)) != 0) {
    body_reader_drain(&reader, 1);
    snprintf(err, sizeof(err), "stage: %s", strerror(errno));
    rar_log_event("stage error dest=%s err=%s", dest, err);
    archive_upload_clear_active(NULL, req->fd);
    job_end(-1, err);
    if(activity.queued) {
      char log_name[256];
      job_log_name(log_name, sizeof(log_name));
      activity_finish_queue(activity.queue_id, -1, err, dest,
                            atomic_load(&g_job.copied_bytes), 0, 1,
                            log_name);
    }
    return serve_error(req, 500, err);
  }
  rar_log_event("stage ok path=%s", stage);

  if(zip_pipe_init(&pipe, ARCHIVE_PIPE_SIZE) != 0) {
    body_reader_drain(&reader, 1);
    snprintf(err, sizeof(err), "out of memory");
    rar_log_event("pipe error");
    if(stage[0]) rar_delete_tree(stage);
    archive_upload_clear_active(NULL, req->fd);
    job_end(-1, err);
    if(activity.queued) {
      char log_name[256];
      job_log_name(log_name, sizeof(log_name));
      activity_finish_queue(activity.queue_id, -1, err, dest,
                            atomic_load(&g_job.copied_bytes), 0, 1,
                            log_name);
    }
    return serve_error(req, 500, err);
  }
  pipe_ready = 1;

  memset(&ctx, 0, sizeof(ctx));
  ctx.pipe = &pipe;
  ctx.archive_remaining = manifest.data_size;

  size_t buffered_pos = reader.initial_pos < reader.initial_size ?
                          reader.initial_pos : reader.initial_size;
  size_t buffered_size = reader.initial_size - buffered_pos;
  if(buffered_size > reader.remaining) buffered_size = (size_t)reader.remaining;
  const char *buffered_data = buffered_size > 0 ?
                                reader.initial + buffered_pos : NULL;
  if(zip_start_upload_producer(&pipe, req->fd, buffered_data, buffered_size,
                               (size_t)reader.remaining, &producer) != 0) {
    zip_pipe_copy_error(&pipe, err, sizeof(err));
    if(!err[0]) snprintf(err, sizeof(err), "rar upload failed");
    rar_log_event("producer error buffered=%llu remaining=%llu err=%s",
                  (unsigned long long)buffered_size,
                  (unsigned long long)reader.remaining, err);
    zip_pipe_cancel(&pipe);
    shutdown(req->fd, SHUT_RD);
    if(stage[0]) rar_delete_tree(stage);
    zip_pipe_destroy(&pipe);
    archive_upload_clear_active(NULL, req->fd);
    job_end(-1, err);
    if(activity.queued) {
      char log_name[256];
      job_log_name(log_name, sizeof(log_name));
      activity_finish_queue(activity.queue_id, -1, err, dest,
                            atomic_load(&g_job.copied_bytes), 0, 1,
                            log_name);
    }
    return serve_error(req, 500, err);
  }
  producer_started = 1;
  rar_log_event("producer ok buffered=%llu remaining=%llu pipe=%d",
                (unsigned long long)buffered_size,
                (unsigned long long)reader.remaining, ARCHIVE_PIPE_SIZE);

  memset(&opts, 0, sizeof(opts));
  opts.archive_name = manifest.archive_name;
  opts.dest_dir = stage;
  opts.password = manifest.password;
  opts.dictionary_limit = RAR_DICT_LIMIT;
  opts.read_cb = rar_stream_read_cb;
  opts.cancel_cb = rar_stream_cancel_cb;
  opts.opaque = &ctx;

  job_set_current("Extracting RAR");
  rar_log_event("extract start dict=%llu", (unsigned long long)RAR_DICT_LIMIT);
  rc = rar_extract_stream(&opts, err, sizeof(err));
  if(ctx.err[0] && (!err[0] || rar_error_is_interrupted(ctx.err) ||
                    operation_error_is_cancelled(ctx.err))) {
    snprintf(err, sizeof(err), "%s", ctx.err);
  }
  if(rc != 0) rar_normalize_stream_error(err, sizeof(err), &ctx, &pipe);
  rar_log_event("extract done rc=%d remaining=%llu err=%s", rc,
                (unsigned long long)ctx.archive_remaining,
                err[0] ? err : "none");
  if(rc != 0 && ctx.archive_remaining == 0 &&
     !strcmp(err, "rar extraction failed")) {
    struct stat st;
    du_state_t du = {0};
    if(lstat(stage, &st) == 0) {
      du.root_dev = st.st_dev;
      du_walk(stage, &du);
      if(du.files > 0) {
        rar_log_event("extract recovered generic fatal entries=%llu files=%llu dirs=%llu bytes=%llu",
                      (unsigned long long)du.entries,
                      (unsigned long long)du.files,
                      (unsigned long long)du.dirs,
                      (unsigned long long)du.bytes);
        rc = 0;
        err[0] = 0;
      }
    }
  }
  if(rc == 0 && ctx.archive_remaining > 0) {
    job_set_current("Finishing RAR upload");
    rar_log_event("drain start remaining=%llu",
                  (unsigned long long)ctx.archive_remaining);
    if(rar_pipe_drain_remaining(&ctx) != 0) {
      if(ctx.err[0]) snprintf(err, sizeof(err), "%s", ctx.err);
      rar_log_event("drain error err=%s", err[0] ? err : "rar upload failed");
      rc = -1;
    }
  }
  if(rc != 0) {
    rar_log_event("extract failed cancel-pipe err=%s",
                  err[0] ? err : "rar upload failed");
    zip_pipe_cancel(&pipe);
    shutdown(req->fd, SHUT_RD);
  }
  if(producer_started) {
    pthread_join(producer, NULL);
    producer_started = 0;
    archive_upload_clear_active(&pipe, req->fd);
    archive_upload_clear_active(NULL, req->fd);
    rar_log_event("producer joined");
  }
  if(rc != 0) rar_normalize_stream_error(err, sizeof(err), &ctx, &pipe);

  if(rc == 0) {
    job_set_current("Placing extracted files");
    rar_log_event("place start stage=%s", stage);
    if(archive_place_stage(stage, manifest.archive_name, dest,
                           final_base, sizeof(final_base), &final_files,
                           err, sizeof(err)) != 0) {
      rar_log_event("place error err=%s", err[0] ? err : "place failed");
      rc = -1;
    } else {
      rar_log_event("place ok dst=%s files=%ld", final_base, final_files);
    }
  }

  if(rc != 0 && stage[0]) {
    struct stat st;
    du_state_t du = {0};
    if(lstat(stage, &st) == 0) {
      du.root_dev = st.st_dev;
      du_walk(stage, &du);
      rar_log_event("failed stage stats entries=%llu files=%llu dirs=%llu bytes=%llu",
                    (unsigned long long)du.entries,
                    (unsigned long long)du.files,
                    (unsigned long long)du.dirs,
                    (unsigned long long)du.bytes);
    } else {
      rar_log_event("failed stage stats unavailable err=%s", strerror(errno));
    }
  }

  if(stage[0]) {
    rar_log_event("cleanup stage=%s", stage);
    rar_delete_tree(stage);
  }
  if(pipe_ready) zip_pipe_destroy(&pipe);
  job_end(rc, rc == 0 ? NULL : (err[0] ? err : "rar upload failed"));
  job_log_name(log_name, sizeof(log_name));
  if(activity.queued) {
    const char *target = final_base[0] ? final_base : dest;
    const char *activity_err = rc == 0 ? NULL : (err[0] ? err : "rar upload failed");
    if(rc != 0 && !manifest.password[0] && rar_error_is_password(err)) {
      activity_err = "password required";
    }
    if(rc == 0 && defer_activity) {
      activity_deferred = 1;
      activity_defer_queue_success(activity.queue_id, target,
                                   zip_job_long((uint64_t)content_size),
                                   atomic_load(&g_job.done_files), log_name);
    } else {
      activity_finish_queue(activity.queue_id, rc, activity_err,
                            target,
                            rc == 0 ? zip_job_long((uint64_t)content_size)
                                    : atomic_load(&g_job.copied_bytes),
                            rc == 0 ? atomic_load(&g_job.done_files) : 0,
                            rc == 0 ? 0 : 1,
                            log_name);
    }
  }
  rar_log_event("request done rc=%d err=%s", rc,
                err[0] ? err : "none");

  if(rc != 0) {
    return serve_error(req, rar_error_status(err),
                       err[0] ? err : "rar upload failed");
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, final_base) != 0 ||
     json_appendf(&b, ",\"files\":%ld,\"activityDeferred\":%s,\"logName\":",
                  final_files, activity_deferred ? "true" : "false") != 0 ||
     json_string(&b, log_name) != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}
