/*
 * BFpilot - ZIP archive upload and extraction helpers.
 */

#include <ctype.h>
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
#include <unistd.h>

#include "archive_common.h"
#include "miniz_tinfl.h"
#include "transfer.h"
#include "transfer_internal.h"
#include "websrv.h"
#include "zip_archive.h"

typedef struct zip_stream {
  unsigned char *buf;
  size_t         pos;
  size_t         end;
  char           err[200];
  struct zip_pipe *pipe;
} zip_stream_t;


typedef struct zip_producer_arg {
  zip_pipe_t    *pipe;
  int            fd;
  unsigned char *initial;
  size_t         initial_size;
  uint64_t       remaining;
} zip_producer_arg_t;


typedef struct zip_file_producer_arg {
  zip_pipe_t *pipe;
  int         fd;
  uint64_t    remaining;
} zip_file_producer_arg_t;


typedef struct zip_upload_opts {
  char dest[1024];
  char filename[256];
  char base_path[1024];
  char dest_folder[256];
  char strip_prefix[ZIP_NAME_MAX];
  char extract_prefix[ZIP_NAME_MAX];
  archive_backup_ctx_t *backup;
} zip_upload_opts_t;


typedef struct zip_entry {
  char     name[ZIP_NAME_MAX];
  uint16_t flags;
  uint16_t method;
  uint64_t comp_size;
  uint64_t uncomp_size;
  int      has_descriptor;
  int      descriptor_zip64;
  int      is_dir;
} zip_entry_t;


static atomic_int g_zip_part_counter;
static pthread_mutex_t g_archive_upload_lock = PTHREAD_MUTEX_INITIALIZER;
static zip_pipe_t *g_archive_upload_pipe = NULL;
static int g_archive_upload_fd = -1;


static int
zip_job_cancel_cb(void *opaque) {
  (void)opaque;
  return job_cancelled();
}


static uint16_t
zip_le16(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}


static uint32_t
zip_le32(const unsigned char *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}


static uint64_t
zip_le64(const unsigned char *p) {
  return (uint64_t)zip_le32(p) | ((uint64_t)zip_le32(p + 4) << 32);
}


long
zip_job_long(uint64_t value) {
  return value > (uint64_t)LONG_MAX ? LONG_MAX : (long)value;
}


static void
zip_stream_error(zip_stream_t *zs, const char *fmt, ...) {
  if(!zs || zs->err[0]) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(zs->err, sizeof(zs->err), fmt, ap);
  va_end(ap);
}


static void
zip_pipe_set_error(zip_pipe_t *pipe, const char *fmt, ...) {
  pthread_mutex_lock(&pipe->lock);
  if(!pipe->err[0]) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(pipe->err, sizeof(pipe->err), fmt, ap);
    va_end(ap);
  }
  pipe->done = 1;
  pthread_cond_broadcast(&pipe->can_read);
  pthread_cond_broadcast(&pipe->can_write);
  pthread_mutex_unlock(&pipe->lock);
}


int
zip_pipe_init(zip_pipe_t *pipe, size_t cap) {
  memset(pipe, 0, sizeof(*pipe));
  pipe->buf = malloc(cap);
  if(!pipe->buf) return -1;
  pipe->cap = cap;
  pthread_mutex_init(&pipe->lock, NULL);
  pthread_cond_init(&pipe->can_read, NULL);
  pthread_cond_init(&pipe->can_write, NULL);
  return 0;
}


void
zip_pipe_destroy(zip_pipe_t *pipe) {
  pthread_cond_destroy(&pipe->can_write);
  pthread_cond_destroy(&pipe->can_read);
  pthread_mutex_destroy(&pipe->lock);
  free(pipe->buf);
  memset(pipe, 0, sizeof(*pipe));
}


static void
zip_pipe_finish(zip_pipe_t *pipe) {
  pthread_mutex_lock(&pipe->lock);
  pipe->done = 1;
  pthread_cond_broadcast(&pipe->can_read);
  pthread_cond_broadcast(&pipe->can_write);
  pthread_mutex_unlock(&pipe->lock);
}


void
zip_pipe_cancel(zip_pipe_t *pipe) {
  pthread_mutex_lock(&pipe->lock);
  pipe->cancel = 1;
  pipe->done = 1;
  pthread_cond_broadcast(&pipe->can_read);
  pthread_cond_broadcast(&pipe->can_write);
  pthread_mutex_unlock(&pipe->lock);
}


void
archive_upload_set_active(zip_pipe_t *pipe, int fd) {
  pthread_mutex_lock(&g_archive_upload_lock);
  g_archive_upload_pipe = pipe;
  g_archive_upload_fd = fd;
  pthread_mutex_unlock(&g_archive_upload_lock);
}


void
archive_upload_clear_active(zip_pipe_t *pipe, int fd) {
  pthread_mutex_lock(&g_archive_upload_lock);
  if(g_archive_upload_pipe == pipe && g_archive_upload_fd == fd) {
    g_archive_upload_pipe = NULL;
    g_archive_upload_fd = -1;
  }
  pthread_mutex_unlock(&g_archive_upload_lock);
}


void
archive_upload_cancel_active(void) {
  pthread_mutex_lock(&g_archive_upload_lock);
  zip_pipe_t *pipe = g_archive_upload_pipe;
  int fd = g_archive_upload_fd;
  pthread_mutex_unlock(&g_archive_upload_lock);

  if(pipe) zip_pipe_cancel(pipe);
  if(fd >= 0) shutdown(fd, SHUT_RDWR);
}


static int
zip_pipe_write(zip_pipe_t *pipe, const unsigned char *data, size_t size) {
  size_t pos = 0;
  while(pos < size) {
    pthread_mutex_lock(&pipe->lock);
    while(pipe->used == pipe->cap && !pipe->done && !pipe->cancel &&
          !job_cancelled()) {
      pthread_cond_wait(&pipe->can_write, &pipe->lock);
    }
    if(pipe->done || pipe->cancel || job_cancelled()) {
      pthread_mutex_unlock(&pipe->lock);
      return -1;
    }

    size_t free_space = pipe->cap - pipe->used;
    size_t contiguous = pipe->cap - pipe->write_pos;
    size_t take = size - pos;
    if(take > free_space) take = free_space;
    if(take > contiguous) take = contiguous;
    memcpy(pipe->buf + pipe->write_pos, data + pos, take);
    pipe->write_pos = (pipe->write_pos + take) % pipe->cap;
    pipe->used += take;
    pos += take;
    pthread_cond_signal(&pipe->can_read);
    pthread_mutex_unlock(&pipe->lock);
  }
  return 0;
}


ssize_t
zip_pipe_read(zip_pipe_t *pipe, unsigned char *out, size_t size) {
  pthread_mutex_lock(&pipe->lock);
  while(pipe->used == 0 && !pipe->done && !pipe->cancel &&
        !job_cancelled()) {
    pthread_cond_wait(&pipe->can_read, &pipe->lock);
  }
  if(pipe->used == 0) {
    if(job_cancelled()) pipe->cancel = 1;
    if(pipe->err[0]) {
      pthread_mutex_unlock(&pipe->lock);
      return -1;
    }
    pthread_mutex_unlock(&pipe->lock);
    return 0;
  }

  size_t contiguous = pipe->cap - pipe->read_pos;
  size_t take = pipe->used < size ? pipe->used : size;
  if(take > contiguous) take = contiguous;
  memcpy(out, pipe->buf + pipe->read_pos, take);
  pipe->read_pos = (pipe->read_pos + take) % pipe->cap;
  pipe->used -= take;
  pthread_cond_signal(&pipe->can_write);
  pthread_mutex_unlock(&pipe->lock);
  return (ssize_t)take;
}


void
zip_pipe_copy_error(zip_pipe_t *pipe, char *out, size_t out_size) {
  pthread_mutex_lock(&pipe->lock);
  snprintf(out, out_size, "%s", pipe->err);
  pthread_mutex_unlock(&pipe->lock);
}


static void *
zip_upload_producer(void *arg) {
  zip_producer_arg_t *a = arg;
  unsigned char *buf = malloc(ZIP_PIPE_READ_SIZE);

  archive_upload_set_active(a->pipe, a->fd);
  if(!buf) {
    zip_pipe_set_error(a->pipe, "out of memory");
    goto done;
  }

  if(a->initial_size > 0) {
    if(zip_pipe_write(a->pipe, a->initial, a->initial_size) != 0) goto done;
    atomic_fetch_add(&g_job.copied_bytes, zip_job_long(a->initial_size));
  }

  while(a->remaining > 0 && !job_cancelled()) {
    size_t want = a->remaining < ZIP_PIPE_READ_SIZE ?
                    (size_t)a->remaining : ZIP_PIPE_READ_SIZE;
    ssize_t n = recv(a->fd, buf, want, 0);
    if(n < 0) {
      if(errno == EINTR) continue;
      zip_pipe_set_error(a->pipe, "recv: %s", strerror(errno));
      goto done;
    }
    if(n == 0) {
      zip_pipe_set_error(a->pipe, "short upload");
      goto done;
    }
    if(zip_pipe_write(a->pipe, buf, (size_t)n) != 0) goto done;
    a->remaining -= (uint64_t)n;
    atomic_fetch_add(&g_job.copied_bytes, (long)n);
  }

  if(job_cancelled()) {
    zip_pipe_set_error(a->pipe, "cancelled");
  } else {
    zip_pipe_finish(a->pipe);
  }

done:
  archive_upload_clear_active(a->pipe, a->fd);
  free(buf);
  free(a->initial);
  free(a);
  return NULL;
}


static void *
zip_file_producer(void *arg) {
  zip_file_producer_arg_t *a = arg;
  unsigned char *buf = malloc(ZIP_PIPE_READ_SIZE);

  archive_upload_set_active(a->pipe, -1);
  if(!buf) {
    zip_pipe_set_error(a->pipe, "out of memory");
    goto done;
  }

  while(a->remaining > 0 && !job_cancelled()) {
    size_t want = a->remaining < ZIP_PIPE_READ_SIZE ?
                    (size_t)a->remaining : ZIP_PIPE_READ_SIZE;
    ssize_t n = read(a->fd, buf, want);
    if(n < 0) {
      if(errno == EINTR) continue;
      zip_pipe_set_error(a->pipe, "read: %s", strerror(errno));
      goto done;
    }
    if(n == 0) {
      zip_pipe_set_error(a->pipe, "short zip file");
      goto done;
    }
    if(zip_pipe_write(a->pipe, buf, (size_t)n) != 0) goto done;
    a->remaining -= (uint64_t)n;
    atomic_fetch_add(&g_job.copied_bytes, (long)n);
  }

  if(job_cancelled()) {
    zip_pipe_set_error(a->pipe, "cancelled");
  } else {
    zip_pipe_finish(a->pipe);
  }

done:
  archive_upload_clear_active(a->pipe, -1);
  if(a->fd >= 0) close(a->fd);
  free(buf);
  free(a);
  return NULL;
}


int
zip_start_upload_producer(zip_pipe_t *pipe, int fd, const char *initial_data,
                          size_t initial_size, size_t content_size,
                          pthread_t *thread) {
  zip_producer_arg_t *arg = calloc(1, sizeof(*arg));
  if(!arg) {
    zip_pipe_set_error(pipe, "out of memory");
    return -1;
  }
  arg->initial = initial_size > 0 ? malloc(initial_size) : NULL;
  if(initial_size > 0 && !arg->initial) {
    free(arg);
    zip_pipe_set_error(pipe, "out of memory");
    return -1;
  }
  if(initial_size > content_size) initial_size = content_size;
  if(initial_size > 0) memcpy(arg->initial, initial_data, initial_size);
  arg->initial_size = initial_size;
  arg->remaining = (uint64_t)(content_size - initial_size);
  arg->fd = fd;
  arg->pipe = pipe;

  int rc = pthread_create(thread, NULL, zip_upload_producer, arg);
  if(rc != 0) {
    free(arg->initial);
    free(arg);
    zip_pipe_set_error(pipe, "pthread_create");
    return -1;
  }
  return 0;
}


int
zip_start_file_producer(zip_pipe_t *pipe, int fd, uint64_t size,
                        pthread_t *thread) {
  zip_file_producer_arg_t *arg = calloc(1, sizeof(*arg));
  if(!arg) {
    zip_pipe_set_error(pipe, "out of memory");
    return -1;
  }
  arg->fd = fd;
  arg->pipe = pipe;
  arg->remaining = size;

  int rc = pthread_create(thread, NULL, zip_file_producer, arg);
  if(rc != 0) {
    free(arg);
    zip_pipe_set_error(pipe, "pthread_create");
    return -1;
  }
  return 0;
}


static int
zip_stream_init(zip_stream_t *zs, zip_pipe_t *pipe) {
  memset(zs, 0, sizeof(*zs));
  zs->pipe = pipe;
  zs->buf = malloc(ZIP_STREAM_BUF_SIZE);
  if(!zs->buf) {
    zip_stream_error(zs, "out of memory");
    return -1;
  }
  return 0;
}


static void
zip_stream_free(zip_stream_t *zs) {
  free(zs->buf);
  zs->buf = NULL;
}


static int
zip_stream_fill(zip_stream_t *zs) {
  if(zs->pos < zs->end) return 1;
  zs->pos = 0;
  zs->end = 0;

  if(job_cancelled()) {
    zip_stream_error(zs, "cancelled");
    return -1;
  }

  ssize_t n = zip_pipe_read(zs->pipe, zs->buf, ZIP_STREAM_BUF_SIZE);
  if(n < 0) {
    char err[200];
    zip_pipe_copy_error(zs->pipe, err, sizeof(err));
    zip_stream_error(zs, "%s", err[0] ? err : "zip upload failed");
    return -1;
  }
  if(n == 0) return 0;
  zs->end = (size_t)n;
  return 1;
}


static int
zip_stream_read(zip_stream_t *zs, void *out, size_t size) {
  unsigned char *dst = out;
  while(size > 0) {
    int rc = zip_stream_fill(zs);
    if(rc <= 0) {
      if(rc == 0) zip_stream_error(zs, "short zip data");
      return -1;
    }
    size_t avail = zs->end - zs->pos;
    size_t take = avail < size ? avail : size;
    memcpy(dst, zs->buf + zs->pos, take);
    zs->pos += take;
    dst += take;
    size -= take;
  }
  return 0;
}


static int
zip_stream_skip(zip_stream_t *zs, uint64_t size) {
  while(size > 0) {
    int rc = zip_stream_fill(zs);
    if(rc <= 0) {
      if(rc == 0) zip_stream_error(zs, "short zip data");
      return -1;
    }
    size_t avail = zs->end - zs->pos;
    size_t take = avail < size ? avail : (size_t)size;
    zs->pos += take;
    size -= take;
  }
  return 0;
}


static void
zip_stream_drain(zip_stream_t *zs) {
  zs->pos = zs->end;
  while(!job_cancelled()) {
    int n = zip_stream_fill(zs);
    if(n == 0) break;
    if(n < 0) break;
    zs->pos = zs->end;
  }
}


static int
zip_read_u32(zip_stream_t *zs, uint32_t *out) {
  unsigned char b[4];
  if(zip_stream_read(zs, b, sizeof(b)) != 0) return -1;
  *out = zip_le32(b);
  return 0;
}


static int
zip_append_norm_segment(char *out, size_t out_size, size_t *pos,
                        const unsigned char *seg, size_t seg_len) {
  if(seg_len == 0) return 0;
  if((seg_len == 1 && seg[0] == '.') ||
     (seg_len == 2 && seg[0] == '.' && seg[1] == '.')) {
    return -1;
  }
  for(size_t i = 0; i < seg_len; i++) {
    if(seg[i] == ':' || seg[i] < 0x20) return -1;
  }
  if(*pos + (*pos ? 1 : 0) + seg_len + 1 > out_size) return -1;
  if(*pos) out[(*pos)++] = '/';
  memcpy(out + *pos, seg, seg_len);
  *pos += seg_len;
  out[*pos] = 0;
  return 0;
}


static int
zip_normalize_path_bytes(const unsigned char *src, size_t len,
                         char *out, size_t out_size, int allow_empty) {
  size_t pos = 0;
  size_t seg_start = 0;

  if(out_size == 0) return -1;
  out[0] = 0;
  if(len == 0) return allow_empty ? 0 : -1;
  if(src[0] == '/' || src[0] == '\\') return -1;

  for(size_t i = 0; i <= len; i++) {
    int at_end = i == len;
    unsigned char ch = at_end ? '/' : src[i];
    if(!at_end && ch == 0) return -1;
    if(ch == '\\') ch = '/';
    if(ch != '/') continue;
    if(zip_append_norm_segment(out, out_size, &pos, src + seg_start,
                               i - seg_start) != 0) {
      return -1;
    }
    seg_start = i + 1;
  }

  return pos > 0 || allow_empty ? 0 : -1;
}


static int
zip_normalize_query_path(const char *src, char *out, size_t out_size,
                         int allow_empty) {
  return zip_normalize_path_bytes((const unsigned char *)(src ? src : ""),
                                  src ? strlen(src) : 0,
                                  out, out_size, allow_empty);
}


static int
zip_has_prefix(const char *path, const char *prefix) {
  size_t n;
  if(!prefix || !prefix[0]) return 1;
  n = strlen(prefix);
  return !strncmp(path, prefix, n) && (path[n] == 0 || path[n] == '/');
}


static const char *
zip_after_prefix(const char *path, const char *prefix) {
  size_t n;
  if(!prefix || !prefix[0]) return path;
  n = strlen(prefix);
  if(path[n] == 0) return path + n;
  return path + n + 1;
}


static int
zip_build_base_path(const zip_upload_opts_t *opts, char *out, size_t out_size) {
  size_t dest_len = strlen(opts->dest);
  size_t folder_len = strlen(opts->dest_folder);
  int needs_slash = dest_len > 1 && opts->dest[dest_len - 1] != '/';

  if(folder_len == 0) {
    if(dest_len + 1 > out_size) return -1;
    snprintf(out, out_size, "%s", opts->dest);
    return 0;
  }
  if(dest_len + (needs_slash ? 1 : 0) + folder_len + 1 > out_size) return -1;
  snprintf(out, out_size, "%s%s%s", opts->dest, needs_slash ? "/" : "",
           opts->dest_folder);
  return 0;
}


static int
zip_target_path(const zip_upload_opts_t *opts, const zip_entry_t *entry,
                char *out, size_t out_size, int *selected) {
  char base[1024];
  const char *rel = entry->name;

  *selected = 0;
  if(opts->extract_prefix[0] && !zip_has_prefix(entry->name,
                                                opts->extract_prefix)) {
    return 0;
  }

  if(opts->strip_prefix[0]) {
    if(!zip_has_prefix(entry->name, opts->strip_prefix)) return 0;
    rel = zip_after_prefix(entry->name, opts->strip_prefix);
  } else if(opts->extract_prefix[0]) {
    rel = zip_after_prefix(entry->name, opts->extract_prefix);
  }
  while(*rel == '/') rel++;

  if(zip_build_base_path(opts, base, sizeof(base)) != 0) return -1;
  if(!*rel) {
    if(!entry->is_dir) return -1;
    if(strlen(base) + 1 > out_size) return -1;
    snprintf(out, out_size, "%s", base);
    *selected = 1;
    return 0;
  }
  if(strlen(base) + strlen(rel) + 2 > out_size) return -1;
  join_path(out, out_size, base, rel);
  *selected = 1;
  return 0;
}


static int
zip_parent_dir(const char *path, char *out, size_t out_size) {
  if(strlen(path) + 1 > out_size) return -1;
  snprintf(out, out_size, "%s", path);
  char *slash = strrchr(out, '/');
  if(!slash) return -1;
  if(slash == out) {
    out[1] = 0;
  } else {
    *slash = 0;
  }
  return 0;
}


static int
zip_open_part_file(const char *final_path, char *part_path,
                   size_t part_path_size) {
  char dir[1024];
  if(zip_parent_dir(final_path, dir, sizeof(dir)) != 0) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if(mkdirs(dir) != 0) return -1;

  int seq = atomic_fetch_add(&g_zip_part_counter, 1);
  for(int i = 0; i < 32; i++) {
    int n = snprintf(part_path, part_path_size, "%s/.bfpilot-part-%ld-%d",
                     dir, (long)getpid(), seq + i);
    if(n < 0 || (size_t)n >= part_path_size) {
      errno = ENAMETOOLONG;
      return -1;
    }
    int fd = open(part_path, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL, 0777);
    if(fd >= 0) return fd;
    if(errno != EEXIST) return -1;
  }
  errno = EEXIST;
  return -1;
}


static int
zip_parse_zip64_extra(const unsigned char *extra, size_t extra_len,
                      uint32_t comp32, uint32_t uncomp32,
                      uint64_t *comp_size, uint64_t *uncomp_size) {
  size_t pos = 0;
  while(pos + 4 <= extra_len) {
    uint16_t id = zip_le16(extra + pos);
    uint16_t len = zip_le16(extra + pos + 2);
    pos += 4;
    if(pos + len > extra_len) return -1;
    if(id == 0x0001) {
      size_t zpos = pos;
      if(uncomp32 == 0xffffffffU) {
        if(zpos + 8 > pos + len) return -1;
        *uncomp_size = zip_le64(extra + zpos);
        zpos += 8;
      }
      if(comp32 == 0xffffffffU) {
        if(zpos + 8 > pos + len) return -1;
        *comp_size = zip_le64(extra + zpos);
      }
      return 0;
    }
    pos += len;
  }
  return 0;
}


static int
zip_read_local_entry(zip_stream_t *zs, zip_entry_t *entry) {
  unsigned char hdr[26];
  unsigned char *name = NULL;
  unsigned char *extra = NULL;

  memset(entry, 0, sizeof(*entry));
  if(zip_stream_read(zs, hdr, sizeof(hdr)) != 0) return -1;

  uint16_t version = zip_le16(hdr);
  entry->flags = zip_le16(hdr + 2);
  entry->method = zip_le16(hdr + 4);
  uint32_t comp32 = zip_le32(hdr + 14);
  uint32_t uncomp32 = zip_le32(hdr + 18);
  uint16_t name_len = zip_le16(hdr + 22);
  uint16_t extra_len = zip_le16(hdr + 24);

  if(name_len == 0 || name_len >= ZIP_NAME_MAX) {
    zip_stream_error(zs, "bad zip entry name");
    return -1;
  }

  name = malloc(name_len);
  extra = extra_len ? malloc(extra_len) : NULL;
  if(!name || (extra_len && !extra)) {
    free(name);
    free(extra);
    zip_stream_error(zs, "out of memory");
    return -1;
  }
  if(zip_stream_read(zs, name, name_len) != 0 ||
     (extra_len && zip_stream_read(zs, extra, extra_len) != 0)) {
    free(name);
    free(extra);
    return -1;
  }

  entry->is_dir = name_len > 0 &&
                  (name[name_len - 1] == '/' || name[name_len - 1] == '\\');
  if(zip_normalize_path_bytes(name, name_len, entry->name,
                              sizeof(entry->name), 0) != 0) {
    free(name);
    free(extra);
    zip_stream_error(zs, "unsafe zip path");
    return -1;
  }
  free(name);

  entry->comp_size = comp32 == 0xffffffffU ? UINT64_MAX : comp32;
  entry->uncomp_size = uncomp32 == 0xffffffffU ? UINT64_MAX : uncomp32;
  entry->descriptor_zip64 = comp32 == 0xffffffffU || uncomp32 == 0xffffffffU;
  if(extra_len &&
     zip_parse_zip64_extra(extra, extra_len, comp32, uncomp32,
                           &entry->comp_size, &entry->uncomp_size) != 0) {
    free(extra);
    zip_stream_error(zs, "bad zip64 extra field");
    return -1;
  }
  free(extra);

  entry->has_descriptor = (entry->flags & ZIP_FLAG_DATA_DESCRIPTOR) != 0;
  if(entry->has_descriptor) {
    entry->comp_size = UINT64_MAX;
    entry->uncomp_size = UINT64_MAX;
  } else if(entry->comp_size == UINT64_MAX || entry->uncomp_size == UINT64_MAX) {
    zip_stream_error(zs, "missing zip64 sizes");
    return -1;
  }

  if(version >= 45 &&
     (entry->has_descriptor || comp32 == 0xffffffffU ||
      uncomp32 == 0xffffffffU)) {
    entry->descriptor_zip64 = 1;
  }
  return 0;
}


static int
zip_read_data_descriptor(zip_stream_t *zs, const zip_entry_t *entry,
                         uint64_t comp_read, uint64_t out_total) {
  unsigned char first[4];
  unsigned char rest[20];
  uint32_t crc;
  uint64_t comp_size;
  uint64_t uncomp_size;

  if(zip_stream_read(zs, first, sizeof(first)) != 0) return -1;
  if(zip_le32(first) == ZIP_SIG_DATA_DESCRIPTOR) {
    if(zip_stream_read(zs, first, sizeof(first)) != 0) return -1;
    crc = zip_le32(first);
  } else {
    crc = zip_le32(first);
  }
  (void)crc;

  if(entry->descriptor_zip64) {
    if(zip_stream_read(zs, rest, 16) != 0) return -1;
    comp_size = zip_le64(rest);
    uncomp_size = zip_le64(rest + 8);
  } else {
    if(zip_stream_read(zs, rest, 8) != 0) return -1;
    comp_size = zip_le32(rest);
    uncomp_size = zip_le32(rest + 4);
  }

  if(comp_size != comp_read || uncomp_size != out_total) {
    zip_stream_error(zs, "zip data descriptor mismatch");
    return -1;
  }
  return 0;
}


static int
zip_write_out(int fd, const void *data, size_t size) {
  if(fd < 0 || size == 0) return 0;
  if(job_cancelled()) {
    errno = ECANCELED;
    return -1;
  }
  return write_all_fd(fd, data, size);
}


static int
zip_inflate_payload(zip_stream_t *zs, const zip_entry_t *entry, int out_fd,
                    uint64_t *out_total) {
  tinfl_decompressor decomp;
  unsigned char *dict = malloc(TINFL_LZ_DICT_SIZE);
  size_t dict_ofs = 0;
  uint64_t comp_left = entry->comp_size;
  uint64_t comp_read = 0;

  *out_total = 0;
  if(!dict) {
    zip_stream_error(zs, "out of memory");
    return -1;
  }
  memset(dict, 0, TINFL_LZ_DICT_SIZE);
  tinfl_init(&decomp);

  for(;;) {
    if(job_cancelled()) {
      free(dict);
      zip_stream_error(zs, "cancelled");
      return -1;
    }

    int fill_rc = zip_stream_fill(zs);
    if(fill_rc <= 0) {
      free(dict);
      zip_stream_error(zs, "short deflate stream");
      return -1;
    }

    size_t in_size = zs->end - zs->pos;
    if(!entry->has_descriptor && in_size > comp_left) {
      in_size = (size_t)comp_left;
    }
    if(in_size == 0) {
      free(dict);
      zip_stream_error(zs, "short deflate stream");
      return -1;
    }

    size_t out_size = TINFL_LZ_DICT_SIZE - dict_ofs;
    mz_uint32 flags = 0;
    if(entry->has_descriptor ||
       (!entry->has_descriptor && comp_left > (uint64_t)in_size)) {
      flags |= TINFL_FLAG_HAS_MORE_INPUT;
    }

    tinfl_status status = tinfl_decompress(
        &decomp, zs->buf + zs->pos, &in_size, dict, dict + dict_ofs,
        &out_size, flags);
    zs->pos += in_size;
    comp_read += (uint64_t)in_size;
    if(!entry->has_descriptor) comp_left -= (uint64_t)in_size;

    if(out_size > 0) {
      if(zip_write_out(out_fd, dict + dict_ofs, out_size) != 0) {
        free(dict);
        zip_stream_error(zs, "write: %s", strerror(errno));
        return -1;
      }
      *out_total += (uint64_t)out_size;
      dict_ofs = (dict_ofs + out_size) & (TINFL_LZ_DICT_SIZE - 1);
    }

    if(status == TINFL_STATUS_DONE) break;
    if(status == TINFL_STATUS_HAS_MORE_OUTPUT ||
       status == TINFL_STATUS_NEEDS_MORE_INPUT) {
      continue;
    }

    free(dict);
    zip_stream_error(zs, "deflate failed");
    return -1;
  }

  free(dict);
  if(!entry->has_descriptor) {
    if(comp_left != 0) {
      zip_stream_error(zs, "deflate size mismatch");
      return -1;
    }
    if(entry->uncomp_size != UINT64_MAX && entry->uncomp_size != *out_total) {
      zip_stream_error(zs, "uncompressed size mismatch");
      return -1;
    }
    return 0;
  }

  return zip_read_data_descriptor(zs, entry, comp_read, *out_total);
}


static int
zip_extract_store(zip_stream_t *zs, const zip_entry_t *entry, int out_fd,
                  uint64_t *out_total) {
  uint64_t remaining = entry->comp_size;
  *out_total = 0;
  if(entry->has_descriptor) {
    zip_stream_error(zs, "stored entries with data descriptors are unsupported");
    return -1;
  }

  while(remaining > 0) {
    int rc = zip_stream_fill(zs);
    if(rc <= 0) {
      if(rc == 0) zip_stream_error(zs, "short stored entry");
      return -1;
    }
    size_t avail = zs->end - zs->pos;
    size_t take = avail < remaining ? avail : (size_t)remaining;
    if(zip_write_out(out_fd, zs->buf + zs->pos, take) != 0) {
      zip_stream_error(zs, "write: %s", strerror(errno));
      return -1;
    }
    zs->pos += take;
    remaining -= (uint64_t)take;
    *out_total += (uint64_t)take;
  }

  if(entry->uncomp_size != UINT64_MAX && entry->uncomp_size != *out_total) {
    zip_stream_error(zs, "stored size mismatch");
    return -1;
  }
  return 0;
}


static int
zip_skip_entry_payload(zip_stream_t *zs, const zip_entry_t *entry) {
  uint64_t ignored = 0;
  if(entry->method == ZIP_METHOD_STORE) {
    if(entry->has_descriptor) {
      zip_stream_error(zs, "cannot skip stored descriptor entry");
      return -1;
    }
    return zip_stream_skip(zs, entry->comp_size);
  }
  if(entry->method == ZIP_METHOD_DEFLATE) {
    if(!entry->has_descriptor) return zip_stream_skip(zs, entry->comp_size);
    return zip_inflate_payload(zs, entry, -1, &ignored);
  }
  if(!entry->has_descriptor && entry->comp_size != UINT64_MAX) {
    return zip_stream_skip(zs, entry->comp_size);
  }
  zip_stream_error(zs, "unsupported compression method");
  return -1;
}


static int
zip_process_entry(zip_stream_t *zs, const zip_upload_opts_t *opts,
                  const zip_entry_t *entry, int *extracted_files) {
  char final_path[1024];
  char part_path[1024] = {0};
  char rel[ZIP_NAME_MAX];
  int selected = 0;
  int out = -1;
  uint64_t out_total = 0;

  if(zip_target_path(opts, entry, final_path, sizeof(final_path),
                     &selected) != 0) {
    zip_stream_error(zs, "target path too long");
    return -1;
  }
  if(!selected) return zip_skip_entry_payload(zs, entry);

  if(entry->flags & ZIP_FLAG_ENCRYPTED) {
    zip_stream_error(zs, "encrypted zip entries are unsupported");
    return -1;
  }
  if(entry->method != ZIP_METHOD_STORE && entry->method != ZIP_METHOD_DEFLATE) {
    zip_stream_error(zs, "unsupported compression method");
    return -1;
  }

  job_set_current(final_path);
  if(entry->is_dir) {
    struct stat dir_st;
    if(opts->backup && opts->backup->enabled &&
       lstat(final_path, &dir_st) != 0 && errno == ENOENT &&
       archive_rel_from_path(opts->base_path, final_path,
                             rel, sizeof(rel)) == 0 &&
       archive_backup_record_added(opts->backup, rel, 1,
                                   zs->err, sizeof(zs->err)) != 0) {
      return -1;
    }
    if(mkdirs(final_path) != 0) {
      zip_stream_error(zs, "mkdir: %s", strerror(errno));
      return -1;
    }
    return zip_skip_entry_payload(zs, entry);
  }

  out = zip_open_part_file(final_path, part_path, sizeof(part_path));
  if(out < 0) {
    zip_stream_error(zs, "open: %s", strerror(errno));
    return -1;
  }

  int rc = entry->method == ZIP_METHOD_STORE ?
             zip_extract_store(zs, entry, out, &out_total) :
             zip_inflate_payload(zs, entry, out, &out_total);
  int close_rc = close(out);
  out = -1;
  if(rc == 0 && close_rc != 0) {
    zip_stream_error(zs, "close: %s", strerror(errno));
    rc = -1;
  }
  if(rc == 0 && opts->backup && opts->backup->enabled) {
    if(archive_rel_from_path(opts->base_path, final_path,
                             rel, sizeof(rel)) != 0) {
      zip_stream_error(zs, "backup path: %s", strerror(errno));
      rc = -1;
    } else {
      struct stat dst_st;
      if(lstat(final_path, &dst_st) == 0) {
        if(archive_backup_move_existing(opts->backup, final_path, rel,
                                        zs->err, sizeof(zs->err)) != 0) {
          rc = -1;
        }
      } else if(errno == ENOENT) {
        if(archive_backup_record_added(opts->backup, rel, 0,
                                       zs->err, sizeof(zs->err)) != 0) {
          rc = -1;
        }
      } else {
        zip_stream_error(zs, "stat: %s", strerror(errno));
        rc = -1;
      }
    }
  }
  if(rc == 0 && rename(part_path, final_path) != 0) {
    zip_stream_error(zs, "rename: %s", strerror(errno));
    rc = -1;
  }
  if(rc != 0) {
    if(out >= 0) close(out);
    unlink(part_path);
    return -1;
  }

  (void)out_total;
  atomic_fetch_add(&g_job.done_files, 1);
  (*extracted_files)++;
  return 0;
}


static int
zip_process_archive(zip_stream_t *zs, const zip_upload_opts_t *opts,
                    int *extracted_files) {
  for(;;) {
    uint32_t sig;
    if(job_cancelled()) {
      zip_stream_error(zs, "cancelled");
      return -1;
    }
    if(zip_read_u32(zs, &sig) != 0) return -1;

    if(sig == ZIP_SIG_LOCAL_FILE) {
      zip_entry_t entry;
      if(zip_read_local_entry(zs, &entry) != 0) return -1;
      if(zip_process_entry(zs, opts, &entry, extracted_files) != 0) {
        return -1;
      }
      continue;
    }

    if(sig == ZIP_SIG_CENTRAL_DIR ||
       sig == ZIP_SIG_END_CENTRAL_DIR ||
       sig == ZIP_SIG_ZIP64_END_CENTRAL_DIR ||
       sig == ZIP_SIG_ZIP64_LOCATOR) {
      zip_stream_drain(zs);
      return 0;
    }

    zip_stream_error(zs, "bad zip signature");
    return -1;
  }
}


static int
parse_upload_int_arg(const http_request_t *req, const char *name, int *out) {
  uint64_t value = 0;
  if(!parse_upload_size_arg(req, name, &value)) return 0;
  *out = value > (uint64_t)INT_MAX ? INT_MAX : (int)value;
  return 1;
}


int
transfer_upload_zip_request(const http_request_t *req, const char *initial_data,
                            size_t initial_size, size_t content_size) {
  zip_upload_opts_t opts;
  zip_pipe_t pipe;
  zip_stream_t zs;
  char raw_strip[ZIP_NAME_MAX];
  char raw_extract[ZIP_NAME_MAX];
  char final_base[1024];
  char lease_err[160] = {0};
  char backup_err[200] = {0};
  archive_backup_ctx_t backup;
  activity_request_ctx_t activity = {0};
  int total_files = 0;
  int extracted_files = 0;
  int rc = -1;
  int final_existed = 0;
  int backup_started = 0;
  int content_complete = 0;
  pthread_t producer;
  int producer_started = 0;

  memset(&opts, 0, sizeof(opts));
  memset(&pipe, 0, sizeof(pipe));
  memset(&zs, 0, sizeof(zs));
  memset(&backup, 0, sizeof(backup));
  raw_strip[0] = 0;
  raw_extract[0] = 0;

  if(content_size == 0) {
    return serve_error(req, 400, "empty zip upload");
  }
  if(!websrv_get_query_arg(req, "path", opts.dest, sizeof(opts.dest)) ||
     !path_is_safe(opts.dest) ||
     !websrv_get_query_arg(req, "filename", opts.filename,
                           sizeof(opts.filename)) ||
     !upload_segment_safe(opts.filename)) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 400, "bad zip upload target");
  }

  if(websrv_get_query_arg(req, "destFolder", opts.dest_folder,
                          sizeof(opts.dest_folder)) &&
     opts.dest_folder[0] &&
     !upload_segment_safe(opts.dest_folder)) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 400, "bad destination folder");
  }
  if(websrv_get_query_arg(req, "stripPrefix", raw_strip, sizeof(raw_strip)) &&
     zip_normalize_query_path(raw_strip, opts.strip_prefix,
                              sizeof(opts.strip_prefix), 1) != 0) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 400, "bad strip prefix");
  }
  if(websrv_get_query_arg(req, "extractPrefix", raw_extract,
                          sizeof(raw_extract)) &&
     zip_normalize_query_path(raw_extract, opts.extract_prefix,
                              sizeof(opts.extract_prefix), 1) != 0) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 400, "bad extract prefix");
  }
  parse_upload_int_arg(req, "files", &total_files);

  if(zip_build_base_path(&opts, final_base, sizeof(final_base)) != 0) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 400, "upload path too long");
  }
  snprintf(opts.base_path, sizeof(opts.base_path), "%s", final_base);
  struct stat final_st;
  if(lstat(final_base, &final_st) == 0) {
    if(!S_ISDIR(final_st.st_mode)) {
      drain_body(req->fd, initial_size, content_size);
      return serve_error(req, 400, "zip destination exists and is not a folder");
    }
    final_existed = 1;
  } else if(errno != ENOENT) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 500, strerror(errno));
  }
  if(mkdirs(final_base) != 0) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 500, strerror(errno));
  }
  if(activity_validate_lease(req, "zip", &activity, lease_err,
                             sizeof(lease_err)) != 0) {
    drain_body(req->fd, initial_size, content_size);
    return serve_error(req, 409, lease_err[0] ? lease_err : "bad activity lease");
  }
  if(!job_begin("unzip")) {
    drain_body(req->fd, initial_size, content_size);
    if(activity.queued) {
      activity_finish_queue(activity.queue_id, -1,
                            "another file-manager job is already running",
                            final_base, 0, 0, 1, NULL);
    }
    return serve_error(req, 409, "another file-manager job is already running");
  }
  job_set_target(final_base);

  if(initial_size > content_size) initial_size = content_size;
  atomic_store(&g_job.total_bytes, zip_job_long((uint64_t)content_size));
  atomic_store(&g_job.copied_bytes, 0);
  atomic_store(&g_job.total_files, total_files);
  job_set_current("Reading zip");
  if(final_existed) {
    if(archive_backup_begin(&backup, final_base, opts.filename,
                            backup_err, sizeof(backup_err)) != 0) {
      job_end(-1, backup_err[0] ? backup_err : "backup failed");
      if(activity.queued) {
        char log_name[256];
        job_log_name(log_name, sizeof(log_name));
        activity_finish_queue(activity.queue_id, -1,
                              backup_err[0] ? backup_err : "backup failed",
                              final_base, 0, 0, 1, log_name);
      }
      return serve_error(req, 500,
                         backup_err[0] ? backup_err : "backup failed");
    }
    backup_started = 1;
    opts.backup = &backup;
  }

  if(zip_pipe_init(&pipe, ARCHIVE_PIPE_SIZE) != 0) {
    job_end(-1, "out of memory");
    if(backup_started) archive_backup_close(&backup, 0);
    if(activity.queued) {
      char log_name[256];
      job_log_name(log_name, sizeof(log_name));
      activity_finish_queue(activity.queue_id, -1, "out of memory",
                            final_base, 0, 0, 1, log_name);
    }
    return serve_error(req, 500, "out of memory");
  }
  if(zip_stream_init(&zs, &pipe) != 0 ||
     zip_start_upload_producer(&pipe, req->fd, initial_data, initial_size,
                               content_size, &producer) != 0) {
    job_end(-1, zs.err[0] ? zs.err : "zip upload failed");
    if(backup_started) archive_backup_close(&backup, 0);
    if(activity.queued) {
      char log_name[256];
      job_log_name(log_name, sizeof(log_name));
      activity_finish_queue(activity.queue_id, -1,
                            zs.err[0] ? zs.err : "zip upload failed",
                            final_base, atomic_load(&g_job.copied_bytes),
                            extracted_files, 1, log_name);
    }
    zip_pipe_cancel(&pipe);
    shutdown(req->fd, SHUT_RD);
    if(producer_started) pthread_join(producer, NULL);
    zip_stream_free(&zs);
    zip_pipe_destroy(&pipe);
    return serve_error(req, 500, zs.err[0] ? zs.err : "zip upload failed");
  }
  producer_started = 1;

  rc = zip_process_archive(&zs, &opts, &extracted_files);
  if(rc == 0 && extracted_files == 0) {
    zip_stream_error(&zs, "zip contained no matching files");
    rc = -1;
  }
  if(rc == 0) {
    content_complete = 1;
    job_set_current("Setting permissions");
    char chmod_err[160] = {0};
    if(archive_chmod_777_recursive(final_base, zip_job_cancel_cb, NULL,
                                   NULL, NULL, NULL, chmod_err,
                                   sizeof(chmod_err)) != 0) {
      zip_stream_error(&zs, "%s", chmod_err[0] ? chmod_err : "chmod failed");
      rc = -1;
    }
  }
  if(rc != 0) {
    zip_pipe_cancel(&pipe);
    shutdown(req->fd, SHUT_RD);
  }
  if(producer_started) pthread_join(producer, NULL);
  if(backup_started) archive_backup_close(&backup, content_complete);
  job_end(rc, rc == 0 ? NULL : (zs.err[0] ? zs.err : "zip upload failed"));
  if(activity.queued) {
    char log_name[256];
    job_log_name(log_name, sizeof(log_name));
    activity_finish_queue(activity.queue_id, rc,
                          rc == 0 ? NULL : (zs.err[0] ? zs.err : "zip upload failed"),
                          final_base,
                          rc == 0 ? zip_job_long((uint64_t)content_size)
                                  : atomic_load(&g_job.copied_bytes),
                          extracted_files,
                          rc == 0 ? 0 : 1,
                          log_name);
  }
  zip_stream_free(&zs);
  zip_pipe_destroy(&pipe);

  if(rc != 0) {
    return serve_error(req, job_cancelled() ? 409 : 500,
                       zs.err[0] ? zs.err : "zip upload failed");
  }

  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":true,\"path\":") != 0 ||
     json_string(&b, final_base) != 0 ||
     json_appendf(&b, ",\"files\":%d}", extracted_files) != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}



int
extract_local_zip(const http_request_t *req, const char *archive_path,
                  const char *dest, const struct stat *st) {
  zip_upload_opts_t opts;
  zip_pipe_t pipe;
  zip_stream_t zs;
  pthread_t producer;
  char stage[1024] = {0};
  char final_base[1024] = {0};
  char err[240] = {0};
  int archive_fd = -1;
  int pipe_ready = 0;
  int stream_ready = 0;
  int producer_started = 0;
  int extracted_files = 0;
  int rc = -1;
  long final_files = 0;

  memset(&opts, 0, sizeof(opts));
  memset(&pipe, 0, sizeof(pipe));
  memset(&zs, 0, sizeof(zs));

  if(!job_begin("unzip")) {
    return serve_error(req, 409, "another file-manager job is already running");
  }
  job_set_target(dest);
  atomic_store(&g_job.total_bytes, zip_job_long((uint64_t)st->st_size));
  atomic_store(&g_job.copied_bytes, 0);
  job_set_current("Opening ZIP");

  if(mkdirs(dest) != 0) {
    snprintf(err, sizeof(err), "destination: %s", strerror(errno));
    goto done;
  }
  if(rar_make_stage_path(dest, stage, sizeof(stage)) != 0) {
    snprintf(err, sizeof(err), "stage: %s", strerror(errno));
    goto done;
  }
  archive_fd = open(archive_path, O_RDONLY);
  if(archive_fd < 0) {
    snprintf(err, sizeof(err), "open: %s", strerror(errno));
    goto done;
  }
  if(zip_pipe_init(&pipe, ARCHIVE_PIPE_SIZE) != 0) {
    snprintf(err, sizeof(err), "out of memory");
    goto done;
  }
  pipe_ready = 1;
  if(zip_stream_init(&zs, &pipe) != 0) {
    snprintf(err, sizeof(err), "%s", zs.err[0] ? zs.err : "out of memory");
    goto done;
  }
  stream_ready = 1;
  if(zip_start_file_producer(&pipe, archive_fd, (uint64_t)st->st_size,
                             &producer) != 0) {
    archive_fd = -1;
    zip_pipe_copy_error(&pipe, err, sizeof(err));
    if(!err[0]) snprintf(err, sizeof(err), "zip file read failed");
    goto done;
  }
  archive_fd = -1;
  producer_started = 1;

  snprintf(opts.dest, sizeof(opts.dest), "%s", stage);
  snprintf(opts.filename, sizeof(opts.filename), "%s", path_basename(archive_path));
  job_set_current("Extracting ZIP");
  rc = zip_process_archive(&zs, &opts, &extracted_files);
  if(rc == 0 && extracted_files == 0) {
    snprintf(err, sizeof(err), "zip contained no files");
    rc = -1;
  } else if(rc != 0 && zs.err[0]) {
    snprintf(err, sizeof(err), "%s", zs.err);
  }
  if(rc != 0) {
    zip_pipe_cancel(&pipe);
  }
  if(producer_started) {
    pthread_join(producer, NULL);
    producer_started = 0;
    archive_upload_clear_active(&pipe, -1);
  }
  if(rc != 0 && !err[0]) {
    zip_pipe_copy_error(&pipe, err, sizeof(err));
    if(!err[0]) snprintf(err, sizeof(err), "zip extraction failed");
  }

  if(rc == 0 &&
     archive_place_stage(stage, path_basename(archive_path), dest,
                         final_base, sizeof(final_base), &final_files,
                         err, sizeof(err)) != 0) {
    rc = -1;
  }

done:
  if(producer_started) {
    zip_pipe_cancel(&pipe);
    pthread_join(producer, NULL);
    archive_upload_clear_active(&pipe, -1);
  }
  if(archive_fd >= 0) close(archive_fd);
  if(stream_ready) zip_stream_free(&zs);
  if(pipe_ready) zip_pipe_destroy(&pipe);
  if(stage[0]) rar_delete_tree(stage);
  if(rc != 0 && job_cancelled()) snprintf(err, sizeof(err), "cancelled");
  job_end(rc, rc == 0 ? NULL : (err[0] ? err : "zip extraction failed"));

  if(rc != 0) {
    return serve_error(req, job_cancelled() || operation_error_is_cancelled(err)
                       ? 409 : 500,
                       err[0] ? err : "zip extraction failed");
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
