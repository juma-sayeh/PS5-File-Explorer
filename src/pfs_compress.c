/*
 * File Explorer - streamed PFS/PFSC app compression.
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
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "miniz.h"
#include "pfs_compress.h"
#include "transfer_internal.h"

#define PFS_BLOCK_SIZE 65536ULL
#define PFS_INODE_SIZE 0xA8
#define PFS_VERSION_PS5 2LL
#define PFS_MAGIC 20130315LL
#define PFS_MODE_CASE_INSENSITIVE 0x8

#define PFS_INODE_MODE_O_READ 0x001
#define PFS_INODE_MODE_O_WRITE 0x002
#define PFS_INODE_MODE_O_EXEC 0x004
#define PFS_INODE_MODE_G_READ 0x008
#define PFS_INODE_MODE_G_WRITE 0x010
#define PFS_INODE_MODE_G_EXEC 0x020
#define PFS_INODE_MODE_U_READ 0x040
#define PFS_INODE_MODE_U_WRITE 0x080
#define PFS_INODE_MODE_U_EXEC 0x100
#define PFS_INODE_MODE_DIR 0x4000
#define PFS_INODE_MODE_FILE 0x8000
#define PFS_INODE_RX_ONLY \
  (PFS_INODE_MODE_O_READ | PFS_INODE_MODE_O_EXEC | \
   PFS_INODE_MODE_G_READ | PFS_INODE_MODE_G_EXEC | \
   PFS_INODE_MODE_U_READ | PFS_INODE_MODE_U_EXEC)
#define PFS_INODE_RWX_ALL \
  (PFS_INODE_RX_ONLY | PFS_INODE_MODE_O_WRITE | \
   PFS_INODE_MODE_G_WRITE | PFS_INODE_MODE_U_WRITE)

#define PFS_INODE_FLAG_COMPRESSED 0x1
#define PFS_INODE_FLAG_READONLY 0x10
#define PFS_INODE_FLAG_INTERNAL 0x20000

#define PFS_DIRENT_TYPE_FILE 2
#define PFS_DIRENT_TYPE_DIRECTORY 3
#define PFS_DIRENT_TYPE_DOT 4
#define PFS_DIRENT_TYPE_DOTDOT 5

#define PFSC_MAGIC 0x43534650U
#define PFSC_UNK4 0
#define PFSC_UNK8 6
#define PFSC_HEADER_SIZE 0x30
#define PFSC_BLOCK_OFFSETS_OFFSET 0x400
#define PFSC_INITIAL_DATA_OFFSET 0x10000ULL
#define PFSC_OFFSET_ENTRY_SIZE 8
#define PFSC_ZLIB_LEVEL MZ_BEST_SPEED
#define PFSC_SLOTS_PER_WORKER 32
#define PFSC_OUTPUT_BUFFER_SIZE (16U * 1024U * 1024U)
#define PFSC_OUTPUT_BUFFER_MIN_SIZE (64U * 1024U)
#define PFS_READ_CACHE_SIZE (16U * 1024U * 1024U)
#define PFS_READ_CACHE_MIN_SIZE (64U * 1024U)

#define EXFAT_SECTOR_SIZE 512ULL
#define EXFAT_SECTORS_PER_CLUSTER 128ULL
#define EXFAT_CLUSTER_SIZE (EXFAT_SECTOR_SIZE * EXFAT_SECTORS_PER_CLUSTER)
#define EXFAT_BOOT_REGION_SECTORS 24ULL
#define EXFAT_BOOT_CHECKSUM_SECTOR 11ULL
#define EXFAT_FAT_OFFSET_SECTORS 128ULL
#define EXFAT_END_OF_CHAIN 0xffffffffU
#define EXFAT_ROOT_SLACK_CLUSTERS 1024ULL

typedef struct byte_buf {
  unsigned char *data;
  size_t len;
  size_t cap;
} byte_buf_t;

typedef struct scan_file {
  char rel[1024];
  char abs[1024];
  uint64_t size;
} scan_file_t;

typedef struct scan_list {
  scan_file_t *items;
  size_t count;
  size_t cap;
} scan_list_t;

typedef struct int_list {
  int *items;
  size_t count;
  size_t cap;
} int_list_t;

typedef struct pfs_dir_node {
  char rel[1024];
  char name[256];
  int parent;
  int inode;
  int nlink;
  int_list_t child_dirs;
  int_list_t child_files;
  unsigned char *blob;
  size_t blob_size;
  uint64_t block_start;
  uint64_t blocks;
} pfs_dir_node_t;

typedef struct pfs_file_node {
  char rel[1024];
  char abs[1024];
  char name[256];
  int parent;
  int inode;
  uint64_t raw_size;
  uint64_t block_start;
  uint64_t blocks;
  int source_deleted;
} pfs_file_node_t;

typedef enum pfs_segment_type {
  PFS_SEG_MEM = 1,
  PFS_SEG_FILE = 2,
} pfs_segment_type_t;

typedef struct pfs_segment {
  uint64_t offset;
  uint64_t size;
  pfs_segment_type_t type;
  const unsigned char *mem;
  char path[1024];
} pfs_segment_t;

typedef struct pfs_layout {
  pfs_dir_node_t *dirs;
  size_t dir_count;
  size_t dir_cap;
  pfs_file_node_t *files;
  size_t file_count;
  size_t file_cap;
  pfs_segment_t *segments;
  size_t segment_count;
  size_t segment_cap;
  unsigned char *header_blob;
  unsigned char *inode_blob;
  size_t inode_blob_size;
  unsigned char *superroot_blob;
  size_t superroot_blob_size;
  unsigned char *fpt_blob;
  size_t fpt_blob_size;
  unsigned char *collision_blob;
  size_t collision_blob_size;
  unsigned char *exfat_boot_blob;
  size_t exfat_boot_blob_size;
  unsigned char *exfat_fat_blob;
  size_t exfat_fat_blob_size;
  unsigned char *exfat_bitmap_blob;
  size_t exfat_bitmap_blob_size;
  unsigned char *exfat_upcase_blob;
  size_t exfat_upcase_blob_size;
  int has_fpt_collision;
  uint64_t inode_count;
  uint64_t inode_block_count;
  uint64_t final_ndblock;
  uint64_t image_size;
} pfs_layout_t;

typedef struct fpt_entry {
  uint32_t hash;
  uint32_t value;
  uint32_t seq;
  uint32_t index;
  int is_dir;
} fpt_entry_t;

typedef struct virtual_reader {
  size_t seg_index;
  ssize_t open_seg;
  int fd;
  unsigned char *cache;
  size_t cache_cap;
  size_t cache_len;
  uint64_t cache_offset;
  ssize_t cache_seg;
} virtual_reader_t;

typedef enum pfsc_slot_state {
  PFSC_SLOT_FREE = 0,
  PFSC_SLOT_FILLING,
  PFSC_SLOT_READY,
  PFSC_SLOT_BUSY,
  PFSC_SLOT_DONE,
} pfsc_slot_state_t;

typedef struct pfsc_slot {
  uint64_t index;
  size_t comp_len;
  int force_raw;
  unsigned char *raw;
  unsigned char *comp;
  pfsc_slot_state_t state;
} pfsc_slot_t;

typedef struct pfsc_pool {
  pthread_mutex_t lock;
  pthread_cond_t cond;
  pfsc_slot_t *slots;
  int slot_count;
  int stop;
  int error;
  mz_uint flags;
} pfsc_pool_t;

typedef struct pfsc_output_buffer {
  unsigned char *data;
  size_t len;
  size_t cap;
  uint64_t offset;
} pfsc_output_buffer_t;

static void
set_err(char *err, size_t err_size, const char *fmt, ...) {
  if(!err || err_size == 0 || err[0]) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

static uint64_t
ceil_div_u64(uint64_t a, uint64_t b) {
  return b ? ((a + b - 1) / b) : 0;
}

static int
ends_with_ci(const char *s, const char *suffix) {
  size_t slen = s ? strlen(s) : 0;
  size_t suffix_len = suffix ? strlen(suffix) : 0;
  if(slen < suffix_len) return 0;
  return strcasecmp(s + slen - suffix_len, suffix) == 0;
}

static int
starts_with_ci(const char *s, const char *prefix) {
  size_t prefix_len = prefix ? strlen(prefix) : 0;
  if(!s || strlen(s) < prefix_len) return 0;
  return strncasecmp(s, prefix, prefix_len) == 0;
}

static const char *
path_basename_const(const char *path) {
  const char *slash;
  if(!path || !path[0]) return "";
  slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static int
path_is_executable_payload(const char *path) {
  const char *name = path_basename_const(path);
  if(starts_with_ci(name, "eboot") && ends_with_ci(name, ".bin")) return 1;
  if(ends_with_ci(name, ".prx") || ends_with_ci(name, ".sprx")) return 1;
  return 0;
}

static int
clamp_worker_count(int workers) {
  if(workers <= 0) return PFS_COMPRESS_DEFAULT_WORKERS;
  if(workers > PFS_COMPRESS_MAX_WORKERS) return PFS_COMPRESS_MAX_WORKERS;
  return workers;
}

static int
ascii_tolower(int ch) {
  return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
}

static int
ascii_toupper(int ch) {
  return ch >= 'a' && ch <= 'z' ? ch - 32 : ch;
}

static int
ascii_casecmp(const char *a, const char *b) {
  const unsigned char *pa = (const unsigned char *)(a ? a : "");
  const unsigned char *pb = (const unsigned char *)(b ? b : "");
  while(*pa || *pb) {
    int ca = ascii_tolower(*pa);
    int cb = ascii_tolower(*pb);
    if(ca != cb) return ca - cb;
    if(*pa) pa++;
    if(*pb) pb++;
  }
  return 0;
}

static int
path_segment_supported(const char *name) {
  if(!name || !*name || !strcmp(name, ".") || !strcmp(name, "..")) return 0;
  if(strlen(name) >= 256) return 0;
  for(const unsigned char *p = (const unsigned char *)name; *p; p++) {
    if(*p < 0x20 || *p >= 0x7f || *p == '/' || *p == '\\') return 0;
  }
  return 1;
}

static int
title_id_safe(const char *title) {
  if(!title || !*title || strlen(title) >= 64) return 0;
  for(const unsigned char *p = (const unsigned char *)title; *p; p++) {
    if(!isalnum(*p) && *p != '_' && *p != '-') return 0;
  }
  return 1;
}

static int
normalize_app_path(const char *path, char *out, size_t out_size) {
  if(!path || path[0] != '/' || strstr(path, "..")) {
    errno = EINVAL;
    return -1;
  }
  size_t n = strlen(path);
  while(n > 1 && path[n - 1] == '/') n--;
  if(n == 0 || n >= out_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(out, path, n);
  out[n] = 0;
  return 0;
}

static int
path_parent_base(const char *path, char *parent, size_t parent_size,
                 char *base, size_t base_size) {
  const char *slash = strrchr(path ? path : "", '/');
  if(!slash || !slash[1]) {
    errno = EINVAL;
    return -1;
  }
  size_t parent_len = slash == path ? 1 : (size_t)(slash - path);
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
join_rel(char *out, size_t out_size, const char *parent, const char *name) {
  int n;
  if(!parent || !parent[0]) {
    n = snprintf(out, out_size, "%s", name);
  } else {
    n = snprintf(out, out_size, "%s/%s", parent, name);
  }
  if(n < 0 || (size_t)n >= out_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

static int
join_abs(char *out, size_t out_size, const char *dir, const char *name) {
  size_t n = strlen(dir);
  int rc = snprintf(out, out_size, "%s%s%s", dir,
                    (n > 1 && dir[n - 1] != '/') ? "/" : "", name);
  if(rc < 0 || (size_t)rc >= out_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

static int
remove_tree_local(const char *path) {
  struct stat st;
  if(lstat(path, &st) != 0) {
    if(errno == ENOENT) return 0;
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if(!d) return -1;
    int rc = 0;
    struct dirent *ent;
    while((ent = readdir(d))) {
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      char child[1024];
      if(join_abs(child, sizeof(child), path, ent->d_name) != 0 ||
         remove_tree_local(child) != 0) {
        rc = -1;
        break;
      }
    }
    closedir(d);
    if(rc != 0) return -1;
    return rmdir(path);
  }
  return unlink(path);
}

static int
buf_reserve(byte_buf_t *b, size_t add) {
  if(b->len + add <= b->cap) return 0;
  size_t next = b->cap ? b->cap : 256;
  while(next < b->len + add) next *= 2;
  unsigned char *p = realloc(b->data, next);
  if(!p) return -1;
  b->data = p;
  b->cap = next;
  return 0;
}

static int
buf_append_zero(byte_buf_t *b, size_t n) {
  if(buf_reserve(b, n) != 0) return -1;
  memset(b->data + b->len, 0, n);
  b->len += n;
  return 0;
}

static int
buf_append(byte_buf_t *b, const void *data, size_t n) {
  if(buf_reserve(b, n) != 0) return -1;
  memcpy(b->data + b->len, data, n);
  b->len += n;
  return 0;
}

static void
le16(unsigned char *p, uint16_t v) {
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void
le32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
  p[2] = (unsigned char)((v >> 16) & 0xff);
  p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void
le64(unsigned char *p, uint64_t v) {
  for(int i = 0; i < 8; i++) p[i] = (unsigned char)((v >> (i * 8)) & 0xff);
}

static int
pwrite_all_local(int fd, const void *data, size_t size, off_t offset) {
  const unsigned char *p = data;
  while(size > 0) {
    ssize_t n = pwrite(fd, p, size, offset);
    if(n < 0) {
      if(errno == EINTR) continue;
      return -1;
    }
    if(n == 0) {
      errno = EIO;
      return -1;
    }
    p += n;
    size -= (size_t)n;
    offset += n;
  }
  return 0;
}

static uint64_t monotonic_us(void);
static void virtual_reader_close_file(virtual_reader_t *vr);

static void
pfsc_output_buffer_init(pfsc_output_buffer_t *b) {
  size_t cap = PFSC_OUTPUT_BUFFER_SIZE;
  memset(b, 0, sizeof(*b));
  while(cap >= PFSC_OUTPUT_BUFFER_MIN_SIZE) {
    b->data = malloc(cap);
    if(b->data) {
      b->cap = cap;
      return;
    }
    cap /= 2;
  }
}

static int
pfsc_output_buffer_flush(int fd, uint64_t file_start, pfsc_output_buffer_t *b,
                         char *err, size_t err_size) {
  if(!b || b->len == 0) return 0;
  if(pwrite_all_local(fd, b->data, b->len,
                      (off_t)(file_start + b->offset)) != 0) {
    set_err(err, err_size, "write compressed payload: %s", strerror(errno));
    return -1;
  }
  b->len = 0;
  return 0;
}

static int
pfsc_output_buffer_write(int fd, uint64_t file_start, pfsc_output_buffer_t *b,
                         uint64_t offset, const void *data, size_t size,
                         char *err, size_t err_size) {
  if(size == 0) return 0;
  if(!b || !b->data || b->cap == 0) {
    if(pwrite_all_local(fd, data, size, (off_t)(file_start + offset)) != 0) {
      set_err(err, err_size, "write compressed payload: %s", strerror(errno));
      return -1;
    }
    return 0;
  }

  if(b->len > 0 && b->offset + b->len != offset) {
    if(pfsc_output_buffer_flush(fd, file_start, b, err, err_size) != 0) return -1;
  }
  if(size > b->cap) {
    if(pfsc_output_buffer_flush(fd, file_start, b, err, err_size) != 0) return -1;
    if(pwrite_all_local(fd, data, size, (off_t)(file_start + offset)) != 0) {
      set_err(err, err_size, "write compressed payload: %s", strerror(errno));
      return -1;
    }
    return 0;
  }
  if(b->len == 0) b->offset = offset;
  if(b->len + size > b->cap) {
    if(pfsc_output_buffer_flush(fd, file_start, b, err, err_size) != 0) return -1;
    b->offset = offset;
  }
  memcpy(b->data + b->len, data, size);
  b->len += size;
  return 0;
}

static void
pfsc_output_buffer_free(pfsc_output_buffer_t *b) {
  if(!b) return;
  free(b->data);
  memset(b, 0, sizeof(*b));
}

static int
path_is_child_of_root(const char *root, const char *path) {
  size_t root_len;
  if(!root || !path) return 0;
  root_len = strlen(root);
  if(root_len == 0 || strcmp(root, "/") == 0) return 0;
  return strncmp(path, root, root_len) == 0 && path[root_len] == '/';
}

static void
try_remove_empty_parent_dirs(const char *root, const char *path) {
  char dir[1024];
  char *slash;
  size_t root_len;

  if(!root || !path || !path_is_child_of_root(root, path)) return;
  snprintf(dir, sizeof(dir), "%s", path);
  slash = strrchr(dir, '/');
  if(!slash) return;
  *slash = 0;
  root_len = strlen(root);

  while(strlen(dir) > root_len && path_is_child_of_root(root, dir)) {
    if(rmdir(dir) != 0) {
      if(errno == ENOENT) {
        /* Treat already-removed parents like empty parents and keep walking. */
      } else {
        break;
      }
    }
    slash = strrchr(dir, '/');
    if(!slash) break;
    *slash = 0;
  }
}

static int
delete_committed_source_files(const char *source_root, pfs_layout_t *nested,
                              virtual_reader_t *vr, int fd,
                              uint64_t file_start,
                              pfsc_output_buffer_t *outbuf,
                              uint64_t completed_blocks,
                              size_t *next_delete_index,
                              int *delete_started,
                              char *err, size_t err_size) {
  size_t i;
  if(!source_root || !nested) return 0;
  i = next_delete_index ? *next_delete_index : 0;
  if(i >= nested->file_count) return 0;

  if(nested->files[i].block_start + nested->files[i].blocks > completed_blocks) {
    return 0;
  }

  if(pfsc_output_buffer_flush(fd, file_start, outbuf, err, err_size) != 0) {
    return -1;
  }
  if(vr) virtual_reader_close_file(vr);

  for(; i < nested->file_count; i++) {
    pfs_file_node_t *f = &nested->files[i];
    if(f->source_deleted) continue;
    if(f->block_start + f->blocks > completed_blocks) break;

    if(!path_is_child_of_root(source_root, f->abs)) {
      set_err(err, err_size, "refusing to delete source outside app folder");
      errno = EINVAL;
      return -1;
    }
    if(unlink(f->abs) != 0 && errno != ENOENT) {
      set_err(err, err_size, "delete source file: %s", strerror(errno));
      return -1;
    }
    f->source_deleted = 1;
    if(delete_started) *delete_started = 1;
    try_remove_empty_parent_dirs(source_root, f->abs);
  }
  if(next_delete_index) *next_delete_index = i;

  return 0;
}

static int
read_exact_at(int fd, void *data, size_t size, off_t offset) {
  unsigned char *p = data;
  while(size > 0) {
    ssize_t n = pread(fd, p, size, offset);
    if(n < 0) {
      if(errno == EINTR) continue;
      return -1;
    }
    if(n == 0) {
      errno = EIO;
      return -1;
    }
    p += n;
    size -= (size_t)n;
    offset += n;
  }
  return 0;
}

static uint64_t
monotonic_us(void) {
  struct timespec ts;
#if defined(CLOCK_MONOTONIC)
  clock_gettime(CLOCK_MONOTONIC, &ts);
#else
  clock_gettime(CLOCK_REALTIME, &ts);
#endif
  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void
job_add_wait_us(atomic_long *counter, uint64_t started_at) {
  uint64_t now = monotonic_us();
  if(now <= started_at) return;
  uint64_t delta = now - started_at;
  atomic_fetch_add(counter, delta > (uint64_t)LONG_MAX ? LONG_MAX : (long)delta);
}

static int
scan_push(scan_list_t *list, const char *abs, const char *rel, uint64_t size) {
  if(list->count == list->cap) {
    size_t next = list->cap ? list->cap * 2 : 128;
    scan_file_t *p = realloc(list->items, next * sizeof(*p));
    if(!p) return -1;
    list->items = p;
    list->cap = next;
  }
  scan_file_t *it = &list->items[list->count++];
  snprintf(it->abs, sizeof(it->abs), "%s", abs);
  snprintf(it->rel, sizeof(it->rel), "%s", rel);
  it->size = size;
  return 0;
}

static int
scan_collect(const char *root, const char *rel, scan_list_t *files,
             char *err, size_t err_size) {
  char dir_path[1024];
  if(rel && rel[0]) join_abs(dir_path, sizeof(dir_path), root, rel);
  else snprintf(dir_path, sizeof(dir_path), "%s", root);

  DIR *d = opendir(dir_path);
  if(!d) {
    set_err(err, err_size, "scan: %s", strerror(errno));
    return -1;
  }

  int rc = 0;
  struct dirent *ent;
  while((ent = readdir(d))) {
    char child_abs[1024];
    char child_rel[1024];
    struct stat st;

    if(job_cancelled()) {
      set_err(err, err_size, "cancelled");
      errno = EINTR;
      rc = -1;
      break;
    }
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    if(!path_segment_supported(ent->d_name)) {
      set_err(err, err_size, "unsupported path name: %s", ent->d_name);
      rc = -1;
      break;
    }
    if(join_abs(child_abs, sizeof(child_abs), dir_path, ent->d_name) != 0 ||
       join_rel(child_rel, sizeof(child_rel), rel ? rel : "", ent->d_name) != 0) {
      set_err(err, err_size, "path too long");
      rc = -1;
      break;
    }
    if(lstat(child_abs, &st) != 0) {
      set_err(err, err_size, "stat: %s", strerror(errno));
      rc = -1;
      break;
    }
    if(S_ISLNK(st.st_mode)) {
      set_err(err, err_size, "symlinks are not supported");
      rc = -1;
      break;
    }
    if(S_ISDIR(st.st_mode)) {
      if(scan_collect(root, child_rel, files, err, err_size) != 0) {
        rc = -1;
        break;
      }
    } else if(S_ISREG(st.st_mode)) {
      if(scan_push(files, child_abs, child_rel,
                   st.st_size > 0 ? (uint64_t)st.st_size : 0) != 0) {
        set_err(err, err_size, "out of memory");
        rc = -1;
        break;
      }
    } else {
      set_err(err, err_size, "unsupported filesystem node: %s", child_rel);
      rc = -1;
      break;
    }
  }

  closedir(d);
  return rc;
}

static int
scan_file_cmp(const void *a, const void *b) {
  const scan_file_t *fa = a;
  const scan_file_t *fb = b;
  return ascii_casecmp(fa->rel, fb->rel);
}

static int
int_list_push(int_list_t *list, int value) {
  if(list->count == list->cap) {
    size_t next = list->cap ? list->cap * 2 : 8;
    int *p = realloc(list->items, next * sizeof(*p));
    if(!p) return -1;
    list->items = p;
    list->cap = next;
  }
  list->items[list->count++] = value;
  return 0;
}

static int
layout_add_dir(pfs_layout_t *l, const char *rel, const char *name,
               int parent) {
  if(l->dir_count == l->dir_cap) {
    size_t next = l->dir_cap ? l->dir_cap * 2 : 64;
    pfs_dir_node_t *p = realloc(l->dirs, next * sizeof(*p));
    if(!p) return -1;
    l->dirs = p;
    l->dir_cap = next;
  }
  int idx = (int)l->dir_count++;
  memset(&l->dirs[idx], 0, sizeof(l->dirs[idx]));
  snprintf(l->dirs[idx].rel, sizeof(l->dirs[idx].rel), "%s", rel ? rel : "");
  snprintf(l->dirs[idx].name, sizeof(l->dirs[idx].name), "%s", name ? name : "");
  l->dirs[idx].parent = parent;
  return idx;
}

static int
layout_find_dir(const pfs_layout_t *l, const char *rel) {
  for(size_t i = 0; i < l->dir_count; i++) {
    if(!strcmp(l->dirs[i].rel, rel ? rel : "")) return (int)i;
  }
  return -1;
}

static int
layout_add_file(pfs_layout_t *l, const scan_file_t *src, int parent) {
  if(l->file_count == l->file_cap) {
    size_t next = l->file_cap ? l->file_cap * 2 : 128;
    pfs_file_node_t *p = realloc(l->files, next * sizeof(*p));
    if(!p) return -1;
    l->files = p;
    l->file_cap = next;
  }
  int idx = (int)l->file_count++;
  memset(&l->files[idx], 0, sizeof(l->files[idx]));
  snprintf(l->files[idx].rel, sizeof(l->files[idx].rel), "%s", src->rel);
  snprintf(l->files[idx].abs, sizeof(l->files[idx].abs), "%s", src->abs);
  const char *base = strrchr(src->rel, '/');
  snprintf(l->files[idx].name, sizeof(l->files[idx].name), "%s",
           base ? base + 1 : src->rel);
  l->files[idx].parent = parent;
  l->files[idx].raw_size = src->size;
  return idx;
}

static int
ensure_parent_dirs(pfs_layout_t *l, const char *file_rel, int *parent_out) {
  char path[1024] = {0};
  char rel[1024];
  snprintf(rel, sizeof(rel), "%s", file_rel);
  char *slash = strrchr(rel, '/');
  if(!slash) {
    *parent_out = 0;
    return 0;
  }
  *slash = 0;

  int parent = 0;
  char *seg = rel;
  while(seg && *seg) {
    char *next_slash = strchr(seg, '/');
    if(next_slash) *next_slash = 0;
    char next_rel[1024];
    if(join_rel(next_rel, sizeof(next_rel), path, seg) != 0) return -1;
    int idx = layout_find_dir(l, next_rel);
    if(idx < 0) {
      idx = layout_add_dir(l, next_rel, seg, parent);
      if(idx < 0 || int_list_push(&l->dirs[parent].child_dirs, idx) != 0) {
        return -1;
      }
    }
    snprintf(path, sizeof(path), "%s", next_rel);
    parent = idx;
    if(!next_slash) break;
    seg = next_slash + 1;
  }
  *parent_out = parent;
  return 0;
}

static uint32_t
pfs_hash_path(const char *path) {
  uint32_t h = 0;
  for(const unsigned char *p = (const unsigned char *)path; *p; p++) {
    h = (uint32_t)(ascii_toupper(*p) + (31U * h));
  }
  return h;
}

static int
append_dirent(byte_buf_t *b, uint32_t inode, int type, const char *name) {
  size_t name_len = strlen(name);
  size_t ent_size = name_len + 17;
  size_t rem = ent_size % 8;
  if(rem) ent_size += 8 - rem;
  unsigned char hdr[16];
  le32(hdr + 0, inode);
  le32(hdr + 4, (uint32_t)type);
  le32(hdr + 8, (uint32_t)name_len);
  le32(hdr + 12, (uint32_t)ent_size);
  if(buf_append(b, hdr, sizeof(hdr)) != 0 ||
     buf_append(b, name, name_len) != 0) {
    return -1;
  }
  if(ent_size > 16 + name_len) {
    return buf_append_zero(b, ent_size - 16 - name_len);
  }
  return 0;
}

static int
build_dir_blob(pfs_layout_t *l, int dir_index) {
  pfs_dir_node_t *d = &l->dirs[dir_index];
  byte_buf_t b = {0};
  int parent_inode = dir_index == 0 ? d->inode : l->dirs[d->parent].inode;

  if(append_dirent(&b, (uint32_t)d->inode, PFS_DIRENT_TYPE_DOT, ".") != 0 ||
     append_dirent(&b, (uint32_t)parent_inode, PFS_DIRENT_TYPE_DOTDOT, "..") != 0) {
    free(b.data);
    return -1;
  }
  for(size_t i = 0; i < d->child_dirs.count; i++) {
    pfs_dir_node_t *child = &l->dirs[d->child_dirs.items[i]];
    if(append_dirent(&b, (uint32_t)child->inode, PFS_DIRENT_TYPE_DIRECTORY,
                     child->name) != 0) {
      free(b.data);
      return -1;
    }
  }
  for(size_t i = 0; i < d->child_files.count; i++) {
    pfs_file_node_t *child = &l->files[d->child_files.items[i]];
    if(append_dirent(&b, (uint32_t)child->inode, PFS_DIRENT_TYPE_FILE,
                     child->name) != 0) {
      free(b.data);
      return -1;
    }
  }
  d->blob = b.data;
  d->blob_size = b.len;
  return 0;
}

static int
fpt_entry_cmp(const void *a, const void *b) {
  const fpt_entry_t *ea = a;
  const fpt_entry_t *eb = b;
  if(ea->hash < eb->hash) return -1;
  if(ea->hash > eb->hash) return 1;
  if(ea->seq < eb->seq) return -1;
  if(ea->seq > eb->seq) return 1;
  return 0;
}

static int
uint32_cmp(const void *a, const void *b) {
  uint32_t aa = *(const uint32_t *)a;
  uint32_t bb = *(const uint32_t *)b;
  if(aa < bb) return -1;
  if(aa > bb) return 1;
  return 0;
}

static int
layout_detect_fpt_collision(const pfs_layout_t *l, char *err, size_t err_size) {
  size_t count = (l->dir_count > 0 ? l->dir_count - 1 : 0) + l->file_count;
  uint32_t *hashes = calloc(count ? count : 1, sizeof(*hashes));
  if(!hashes) {
    set_err(err, err_size, "out of memory");
    return -1;
  }

  size_t pos = 0;
  for(size_t i = 1; i < l->dir_count; i++) {
    char full[1024];
    int n = snprintf(full, sizeof(full), "/%s", l->dirs[i].rel);
    if(n < 0 || (size_t)n >= sizeof(full)) {
      free(hashes);
      set_err(err, err_size, "path too long");
      return -1;
    }
    hashes[pos++] = pfs_hash_path(full);
  }
  for(size_t i = 0; i < l->file_count; i++) {
    char full[1024];
    int n = snprintf(full, sizeof(full), "/%s", l->files[i].rel);
    if(n < 0 || (size_t)n >= sizeof(full)) {
      free(hashes);
      set_err(err, err_size, "path too long");
      return -1;
    }
    hashes[pos++] = pfs_hash_path(full);
  }

  qsort(hashes, count, sizeof(*hashes), uint32_cmp);
  for(size_t i = 1; i < count; i++) {
    if(hashes[i - 1] == hashes[i]) {
      free(hashes);
      return 1;
    }
  }
  free(hashes);
  return 0;
}

static int
fpt_entry_full_path(const pfs_layout_t *l, const fpt_entry_t *entry,
                    char *out, size_t out_size) {
  const char *rel = entry->is_dir ? l->dirs[entry->index].rel
                                  : l->files[entry->index].rel;
  int n = snprintf(out, out_size, "/%s", rel);
  if(n < 0 || (size_t)n >= out_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

static int
build_fpt_blob(pfs_layout_t *l, char *err, size_t err_size) {
  size_t count = (l->dir_count > 0 ? l->dir_count - 1 : 0) + l->file_count;
  fpt_entry_t *entries = calloc(count ? count : 1, sizeof(*entries));
  if(!entries) {
    set_err(err, err_size, "out of memory");
    return -1;
  }

  size_t pos = 0;
  for(size_t i = 1; i < l->dir_count; i++) {
    char full[1024];
    int n = snprintf(full, sizeof(full), "/%s", l->dirs[i].rel);
    if(n < 0 || (size_t)n >= sizeof(full)) {
      free(entries);
      set_err(err, err_size, "path too long");
      return -1;
    }
    entries[pos].hash = pfs_hash_path(full);
    entries[pos].value = (uint32_t)l->dirs[i].inode | 0x20000000U;
    entries[pos].seq = (uint32_t)pos;
    entries[pos].index = (uint32_t)i;
    entries[pos].is_dir = 1;
    pos++;
  }
  for(size_t i = 0; i < l->file_count; i++) {
    char full[1024];
    int n = snprintf(full, sizeof(full), "/%s", l->files[i].rel);
    if(n < 0 || (size_t)n >= sizeof(full)) {
      free(entries);
      set_err(err, err_size, "path too long");
      return -1;
    }
    entries[pos].hash = pfs_hash_path(full);
    entries[pos].value = (uint32_t)l->files[i].inode;
    entries[pos].seq = (uint32_t)pos;
    entries[pos].index = (uint32_t)i;
    entries[pos].is_dir = 0;
    pos++;
  }
  qsort(entries, count, sizeof(*entries), fpt_entry_cmp);

  size_t unique_count = 0;
  for(size_t i = 0; i < count;) {
    size_t group_end = i + 1;
    while(group_end < count && entries[group_end].hash == entries[i].hash) {
      group_end++;
    }
    if(group_end - i > 1) {
      byte_buf_t collision = {0};
      if(l->collision_blob) {
        collision.data = l->collision_blob;
        collision.len = l->collision_blob_size;
        collision.cap = l->collision_blob_size;
        l->collision_blob = NULL;
        l->collision_blob_size = 0;
      }
      if(collision.len > 0x7fffffffU) {
        free(collision.data);
        free(entries);
        set_err(err, err_size, "flat path table collision resolver is too large");
        return -1;
      }
      uint32_t offset = (uint32_t)collision.len;
      for(size_t j = i; j < group_end; j++) {
        char full[1024];
        uint32_t inode = entries[j].is_dir
          ? (uint32_t)l->dirs[entries[j].index].inode
          : (uint32_t)l->files[entries[j].index].inode;
        int type = entries[j].is_dir
          ? PFS_DIRENT_TYPE_DIRECTORY
          : PFS_DIRENT_TYPE_FILE;
        if(fpt_entry_full_path(l, &entries[j], full, sizeof(full)) != 0 ||
           append_dirent(&collision, inode, type, full) != 0) {
          free(collision.data);
          free(entries);
          set_err(err, err_size, "out of memory");
          return -1;
        }
      }
      if(buf_append_zero(&collision, 0x18) != 0) {
        free(collision.data);
        free(entries);
        set_err(err, err_size, "out of memory");
        return -1;
      }
      l->collision_blob = collision.data;
      l->collision_blob_size = collision.len;
      entries[i].value = 0x80000000U | offset;
    }
    unique_count++;
    i = group_end;
  }

  l->fpt_blob_size = unique_count * 8;
  l->fpt_blob = calloc(1, l->fpt_blob_size ? l->fpt_blob_size : 1);
  if(!l->fpt_blob) {
    free(entries);
    set_err(err, err_size, "out of memory");
    return -1;
  }
  size_t out = 0;
  for(size_t i = 0; i < count;) {
    size_t group_end = i + 1;
    while(group_end < count && entries[group_end].hash == entries[i].hash) {
      group_end++;
    }
    le32(l->fpt_blob + out * 8, entries[i].hash);
    le32(l->fpt_blob + out * 8 + 4, entries[i].value);
    out++;
    i = group_end;
  }
  free(entries);
  return 0;
}

static int
build_superroot_blob(pfs_layout_t *l) {
  byte_buf_t b = {0};
  if(append_dirent(&b, 1, PFS_DIRENT_TYPE_FILE, "flat_path_table") != 0) {
    free(b.data);
    return -1;
  }
  if(l->has_fpt_collision &&
     append_dirent(&b, 2, PFS_DIRENT_TYPE_FILE, "collision_resolver") != 0) {
    free(b.data);
    return -1;
  }
  if(append_dirent(&b, (uint32_t)l->dirs[0].inode,
                   PFS_DIRENT_TYPE_DIRECTORY, "uroot") != 0) {
    free(b.data);
    return -1;
  }
  l->superroot_blob = b.data;
  l->superroot_blob_size = b.len;
  return 0;
}

static void
write_inode(unsigned char *out, uint16_t mode, uint16_t nlink, uint32_t flags,
            uint64_t size, uint64_t size_comp, uint32_t blocks,
            int32_t db0, int fill_rest_minus_one, time_t now) {
  memset(out, 0, PFS_INODE_SIZE);
  le16(out + 0x00, mode);
  le16(out + 0x02, nlink);
  le32(out + 0x04, flags);
  le64(out + 0x08, size);
  le64(out + 0x10, size_comp);
  le64(out + 0x18, (uint64_t)now);
  le64(out + 0x20, (uint64_t)now);
  le64(out + 0x28, (uint64_t)now);
  le64(out + 0x30, (uint64_t)now);
  le32(out + 0x60, blocks);
  le32(out + 0x64, (uint32_t)db0);
  if(fill_rest_minus_one) {
    for(int i = 1; i < 12; i++) le32(out + 0x64 + i * 4, 0xffffffffU);
    for(int i = 0; i < 5; i++) le32(out + 0x94 + i * 4, 0xffffffffU);
  }
}

static void
write_header_blob(unsigned char *hdr, uint64_t inode_count,
                  uint64_t inode_block_count, uint64_t final_ndblock,
                  time_t now) {
  memset(hdr, 0, PFS_BLOCK_SIZE);
  le64(hdr + 0x00, PFS_VERSION_PS5);
  le64(hdr + 0x08, PFS_MAGIC);
  le64(hdr + 0x10, 0);
  hdr[0x18] = 0;
  hdr[0x19] = 0;
  hdr[0x1a] = 1;
  hdr[0x1b] = 0;
  le16(hdr + 0x1c, PFS_MODE_CASE_INSENSITIVE);
  le16(hdr + 0x1e, 0);
  le32(hdr + 0x20, (uint32_t)PFS_BLOCK_SIZE);
  le32(hdr + 0x24, 0);
  le64(hdr + 0x28, 1);
  le64(hdr + 0x30, inode_count);
  le64(hdr + 0x38, final_ndblock);
  le64(hdr + 0x40, inode_block_count);

  unsigned char *sig = hdr + 0x50;
  uint64_t inode_bytes = inode_block_count * PFS_BLOCK_SIZE;
  le16(sig + 0x00, 0);
  le16(sig + 0x02, 1);
  le32(sig + 0x04, PFS_INODE_FLAG_READONLY);
  le64(sig + 0x08, inode_bytes);
  le64(sig + 0x10, inode_bytes);
  le64(sig + 0x18, (uint64_t)now);
  le64(sig + 0x20, (uint64_t)now);
  le64(sig + 0x28, (uint64_t)now);
  le64(sig + 0x30, (uint64_t)now);
  le32(sig + 0x60, (uint32_t)inode_block_count);
  le64(sig + 0x68 + 32, 1);
  le32(hdr + 0x368, 1);
}

static unsigned char *
inode_slot(pfs_layout_t *l, int inode) {
  uint64_t per_block = PFS_BLOCK_SIZE / PFS_INODE_SIZE;
  uint64_t block = (uint64_t)inode / per_block;
  uint64_t index = (uint64_t)inode % per_block;
  return l->inode_blob + block * PFS_BLOCK_SIZE + index * PFS_INODE_SIZE;
}

static int
layout_add_segment(pfs_layout_t *l, uint64_t offset, uint64_t size,
                   pfs_segment_type_t type, const unsigned char *mem,
                   const char *path) {
  if(size == 0) return 0;
  if(l->segment_count == l->segment_cap) {
    size_t next = l->segment_cap ? l->segment_cap * 2 : 128;
    pfs_segment_t *p = realloc(l->segments, next * sizeof(*p));
    if(!p) return -1;
    l->segments = p;
    l->segment_cap = next;
  }
  pfs_segment_t *s = &l->segments[l->segment_count++];
  memset(s, 0, sizeof(*s));
  s->offset = offset;
  s->size = size;
  s->type = type;
  s->mem = mem;
  if(path) snprintf(s->path, sizeof(s->path), "%s", path);
  return 0;
}

static int
build_layout_from_files(const char *root, pfs_layout_t *l,
                        char *err, size_t err_size) {
  scan_list_t scans = {0};
  int rc = -1;
  time_t now = time(NULL);

  job_set_current("Scanning app folder");
  if(scan_collect(root, "", &scans, err, err_size) != 0) goto done;
  if(scans.count == 0) {
    set_err(err, err_size, "app folder contains no files");
    goto done;
  }
  qsort(scans.items, scans.count, sizeof(scans.items[0]), scan_file_cmp);

  if(layout_add_dir(l, "", "uroot", -1) != 0) {
    set_err(err, err_size, "out of memory");
    goto done;
  }

  for(size_t i = 0; i < scans.count; i++) {
    int parent = 0;
    if(ensure_parent_dirs(l, scans.items[i].rel, &parent) != 0) {
      set_err(err, err_size, "path too long");
      goto done;
    }
    int file_idx = layout_add_file(l, &scans.items[i], parent);
    if(file_idx < 0 || int_list_push(&l->dirs[parent].child_files,
                                     file_idx) != 0) {
      set_err(err, err_size, "out of memory");
      goto done;
    }
  }

  int collision = layout_detect_fpt_collision(l, err, err_size);
  if(collision < 0) goto done;
  l->has_fpt_collision = collision;

  l->dirs[0].inode = l->has_fpt_collision ? 3 : 2;
  int next_inode = l->has_fpt_collision ? 4 : 3;
  for(size_t i = 1; i < l->dir_count; i++) l->dirs[i].inode = next_inode++;
  for(size_t i = 0; i < l->file_count; i++) l->files[i].inode = next_inode++;
  l->inode_count = (uint64_t)next_inode;

  for(size_t i = 0; i < l->dir_count; i++) {
    l->dirs[i].nlink = i == 0 ? 3 : 2;
    for(size_t j = 0; j < l->dirs[i].child_dirs.count; j++) {
      l->dirs[i].nlink++;
    }
    if(build_dir_blob(l, (int)i) != 0) {
      set_err(err, err_size, "out of memory");
      goto done;
    }
  }
  if(build_fpt_blob(l, err, err_size) != 0) goto done;
  if(build_superroot_blob(l) != 0) {
    set_err(err, err_size, "out of memory");
    goto done;
  }

  uint64_t inodes_per_block = PFS_BLOCK_SIZE / PFS_INODE_SIZE;
  l->inode_block_count = ceil_div_u64(l->inode_count, inodes_per_block);
  l->inode_blob_size = (size_t)(l->inode_block_count * PFS_BLOCK_SIZE);
  l->inode_blob = calloc(1, l->inode_blob_size);
  l->header_blob = calloc(1, PFS_BLOCK_SIZE);
  if(!l->inode_blob || !l->header_blob) {
    set_err(err, err_size, "out of memory");
    goto done;
  }

  uint64_t nd = 1 + l->inode_block_count;
  uint64_t superroot_block = nd++;
  uint64_t fpt_blocks = ceil_div_u64(l->fpt_blob_size, PFS_BLOCK_SIZE);
  if(fpt_blocks == 0) fpt_blocks = 1;
  uint64_t fpt_block = nd;
  nd += fpt_blocks;
  uint64_t collision_blocks = 1;
  uint64_t collision_block = nd;
  if(l->has_fpt_collision) {
    collision_blocks = ceil_div_u64(l->collision_blob_size, PFS_BLOCK_SIZE);
    if(collision_blocks == 0) collision_blocks = 1;
  }
  nd += collision_blocks;

  for(size_t i = 0; i < l->dir_count; i++) {
    l->dirs[i].blocks = ceil_div_u64(l->dirs[i].blob_size, PFS_BLOCK_SIZE);
    if(l->dirs[i].blocks == 0) l->dirs[i].blocks = 1;
    l->dirs[i].block_start = nd;
    nd += l->dirs[i].blocks;
  }
  for(size_t i = 0; i < l->file_count; i++) {
    l->files[i].blocks = ceil_div_u64(l->files[i].raw_size, PFS_BLOCK_SIZE);
    if(l->files[i].blocks == 0) l->files[i].blocks = 1;
    l->files[i].block_start = nd;
    nd += l->files[i].blocks;
  }
  l->final_ndblock = nd;
  l->image_size = nd * PFS_BLOCK_SIZE;

  write_header_blob(l->header_blob, l->inode_count, l->inode_block_count,
                    l->final_ndblock, now);
  write_inode(inode_slot(l, 0), PFS_INODE_MODE_DIR | PFS_INODE_RWX_ALL, 1,
              PFS_INODE_FLAG_INTERNAL | PFS_INODE_FLAG_READONLY,
              PFS_BLOCK_SIZE, PFS_BLOCK_SIZE, 1, (int32_t)superroot_block, 0,
              now);
  write_inode(inode_slot(l, 1), PFS_INODE_MODE_FILE | PFS_INODE_RWX_ALL, 1,
              PFS_INODE_FLAG_INTERNAL | PFS_INODE_FLAG_READONLY,
              l->fpt_blob_size, l->fpt_blob_size, (uint32_t)fpt_blocks,
              (int32_t)fpt_block, 1, now);
  if(l->has_fpt_collision) {
    write_inode(inode_slot(l, 2), PFS_INODE_MODE_FILE | PFS_INODE_RWX_ALL, 1,
                PFS_INODE_FLAG_INTERNAL | PFS_INODE_FLAG_READONLY,
                l->collision_blob_size, l->collision_blob_size,
                (uint32_t)collision_blocks, (int32_t)collision_block, 1,
                now);
  }
  for(size_t i = 0; i < l->dir_count; i++) {
    pfs_dir_node_t *d = &l->dirs[i];
    write_inode(inode_slot(l, d->inode),
                PFS_INODE_MODE_DIR | PFS_INODE_RWX_ALL,
                (uint16_t)d->nlink, PFS_INODE_FLAG_READONLY,
                d->blocks * PFS_BLOCK_SIZE, d->blocks * PFS_BLOCK_SIZE,
                (uint32_t)d->blocks, (int32_t)d->block_start, 1, now);
  }
  for(size_t i = 0; i < l->file_count; i++) {
    pfs_file_node_t *f = &l->files[i];
    write_inode(inode_slot(l, f->inode),
                PFS_INODE_MODE_FILE | PFS_INODE_RWX_ALL, 1,
                PFS_INODE_FLAG_READONLY,
                f->raw_size, f->raw_size, (uint32_t)f->blocks,
                (int32_t)f->block_start, 1, now);
  }

  if(layout_add_segment(l, 0, PFS_BLOCK_SIZE, PFS_SEG_MEM,
                        l->header_blob, NULL) != 0 ||
     layout_add_segment(l, PFS_BLOCK_SIZE, l->inode_blob_size, PFS_SEG_MEM,
                        l->inode_blob, NULL) != 0 ||
     layout_add_segment(l, superroot_block * PFS_BLOCK_SIZE,
                        l->superroot_blob_size, PFS_SEG_MEM,
                        l->superroot_blob, NULL) != 0 ||
     layout_add_segment(l, fpt_block * PFS_BLOCK_SIZE,
                        l->fpt_blob_size, PFS_SEG_MEM,
                        l->fpt_blob, NULL) != 0) {
    set_err(err, err_size, "out of memory");
    goto done;
  }
  if(l->has_fpt_collision &&
     layout_add_segment(l, collision_block * PFS_BLOCK_SIZE,
                        l->collision_blob_size, PFS_SEG_MEM,
                        l->collision_blob, NULL) != 0) {
    set_err(err, err_size, "out of memory");
    goto done;
  }
  for(size_t i = 0; i < l->dir_count; i++) {
    if(layout_add_segment(l, l->dirs[i].block_start * PFS_BLOCK_SIZE,
                          l->dirs[i].blob_size, PFS_SEG_MEM,
                          l->dirs[i].blob, NULL) != 0) {
      set_err(err, err_size, "out of memory");
      goto done;
    }
  }
  for(size_t i = 0; i < l->file_count; i++) {
    if(layout_add_segment(l, l->files[i].block_start * PFS_BLOCK_SIZE,
                          l->files[i].raw_size, PFS_SEG_FILE,
                          NULL, l->files[i].abs) != 0) {
      set_err(err, err_size, "out of memory");
      goto done;
    }
  }

  rc = 0;

done:
  free(scans.items);
  return rc;
}

typedef struct exfat_allocation {
  uint32_t first_cluster;
  uint32_t cluster_count;
} exfat_allocation_t;

static uint16_t
exfat_upcase_char(uint16_t ch) {
  if(ch >= 'a' && ch <= 'z') return (uint16_t)(ch - 32);
  return ch;
}

static uint16_t
exfat_rotate16(uint16_t value) {
  return (uint16_t)(((value & 1U) ? 0x8000U : 0U) + (value >> 1));
}

static uint32_t
exfat_rotate32(uint32_t value) {
  return ((value & 1U) ? 0x80000000U : 0U) + (value >> 1);
}

static uint32_t
exfat_table_checksum(const unsigned char *data, size_t size) {
  uint32_t sum = 0;
  for(size_t i = 0; i < size; i++) sum = exfat_rotate32(sum) + data[i];
  return sum;
}

static uint32_t
exfat_boot_checksum(const unsigned char *boot, size_t sector_size) {
  uint32_t sum = 0;
  size_t limit = (size_t)EXFAT_BOOT_CHECKSUM_SECTOR * sector_size;
  for(size_t i = 0; i < limit; i++) {
    if(i == 106 || i == 107 || i == 112) continue;
    sum = exfat_rotate32(sum) + boot[i];
  }
  return sum;
}

static uint16_t
exfat_name_hash(const char *name) {
  uint16_t hash = 0;
  for(const unsigned char *p = (const unsigned char *)name; *p; p++) {
    uint16_t ch = exfat_upcase_char(*p);
    hash = (uint16_t)(exfat_rotate16(hash) + (ch & 0xff));
    hash = (uint16_t)(exfat_rotate16(hash) + ((ch >> 8) & 0xff));
  }
  return hash;
}

static uint16_t
exfat_entry_set_checksum(const unsigned char *entries, size_t entry_count) {
  uint16_t sum = 0;
  size_t size = entry_count * 32;
  for(size_t i = 0; i < size; i++) {
    if(i == 2 || i == 3) continue;
    sum = (uint16_t)(exfat_rotate16(sum) + entries[i]);
  }
  return sum;
}

static uint64_t
exfat_cluster_offset(uint32_t cluster_heap_offset, uint32_t cluster) {
  return ((uint64_t)cluster_heap_offset * EXFAT_SECTOR_SIZE) +
         ((uint64_t)cluster - 2ULL) * EXFAT_CLUSTER_SIZE;
}

static size_t
exfat_name_entry_count(const char *name) {
  size_t len = strlen(name);
  return len ? ceil_div_u64(len, 15) : 1;
}

static size_t
exfat_dir_entry_set_count(const char *name) {
  return 2 + exfat_name_entry_count(name);
}

static size_t
exfat_estimate_dir_entries(const pfs_layout_t *l, int dir_index) {
  const pfs_dir_node_t *d = &l->dirs[dir_index];
  size_t entries = dir_index == 0 ? 3 : 0; /* volume label + bitmap + upcase */
  for(size_t i = 0; i < d->child_dirs.count; i++) {
    entries += exfat_dir_entry_set_count(l->dirs[d->child_dirs.items[i]].name);
  }
  for(size_t i = 0; i < d->child_files.count; i++) {
    entries += exfat_dir_entry_set_count(l->files[d->child_files.items[i]].name);
  }
  return entries + 1; /* terminating unused entry */
}

static int
exfat_append_file_entry(byte_buf_t *b, const char *name, int is_dir,
                        uint32_t first_cluster, uint64_t data_len) {
  size_t name_len = strlen(name);
  size_t name_entries = exfat_name_entry_count(name);
  size_t entry_count = 2 + name_entries;
  unsigned char entries[32 * (2 + 17)];
  if(name_len == 0 || name_len > 255 || name_entries > 17) return -1;
  memset(entries, 0, sizeof(entries));

  entries[0] = 0x85;
  entries[1] = (unsigned char)(entry_count - 1);
  le16(entries + 4, (uint16_t)(is_dir ? 0x10 : 0x20));

  unsigned char *stream = entries + 32;
  stream[0] = 0xC0;
  stream[1] = first_cluster ? 0x03 : 0x00;
  stream[3] = (unsigned char)name_len;
  le16(stream + 4, exfat_name_hash(name));
  le64(stream + 8, data_len);
  le32(stream + 20, first_cluster);
  le64(stream + 24, data_len);

  size_t consumed = 0;
  for(size_t e = 0; e < name_entries; e++) {
    unsigned char *fn = entries + 32 * (2 + e);
    fn[0] = 0xC1;
    fn[1] = 0x00;
    for(size_t i = 0; i < 15; i++) {
      uint16_t ch = 0;
      if(consumed < name_len) ch = (unsigned char)name[consumed++];
      le16(fn + 2 + i * 2, ch);
    }
  }

  uint16_t checksum = exfat_entry_set_checksum(entries, entry_count);
  le16(entries + 2, checksum);
  return buf_append(b, entries, entry_count * 32);
}

static int
exfat_build_dir_blob(pfs_layout_t *l, int dir_index,
                     const exfat_allocation_t *dir_allocs,
                     const exfat_allocation_t *file_allocs,
                     uint32_t bitmap_cluster, uint64_t bitmap_size,
                     uint32_t upcase_cluster, uint64_t upcase_size,
                     uint32_t upcase_checksum,
                     char *err, size_t err_size) {
  pfs_dir_node_t *d = &l->dirs[dir_index];
  byte_buf_t b = {0};

  if(dir_index == 0) {
    unsigned char volume_label[32] = {0};
    volume_label[0] = 0x83;

    unsigned char bitmap[32] = {0};
    bitmap[0] = 0x81;
    le32(bitmap + 20, bitmap_cluster);
    le64(bitmap + 24, bitmap_size);

    unsigned char upcase[32] = {0};
    upcase[0] = 0x82;
    le32(upcase + 4, upcase_checksum);
    le32(upcase + 20, upcase_cluster);
    le64(upcase + 24, upcase_size);
    if(buf_append(&b, volume_label, sizeof(volume_label)) != 0 ||
       buf_append(&b, bitmap, sizeof(bitmap)) != 0 ||
       buf_append(&b, upcase, sizeof(upcase)) != 0) {
      set_err(err, err_size, "out of memory");
      free(b.data);
      return -1;
    }
  }

  for(size_t i = 0; i < d->child_dirs.count; i++) {
    int child_idx = d->child_dirs.items[i];
    const pfs_dir_node_t *child = &l->dirs[child_idx];
    uint64_t data_len = (uint64_t)dir_allocs[child_idx].cluster_count *
                        EXFAT_CLUSTER_SIZE;
    if(exfat_append_file_entry(&b, child->name, 1,
                               dir_allocs[child_idx].first_cluster,
                               data_len) != 0) {
      set_err(err, err_size, "out of memory");
      free(b.data);
      return -1;
    }
  }
  for(size_t i = 0; i < d->child_files.count; i++) {
    int child_idx = d->child_files.items[i];
    const pfs_file_node_t *child = &l->files[child_idx];
    if(exfat_append_file_entry(&b, child->name, 0,
                               file_allocs[child_idx].first_cluster,
                               child->raw_size) != 0) {
      set_err(err, err_size, "out of memory");
      free(b.data);
      return -1;
    }
  }
  if(buf_append_zero(&b, 32) != 0) {
    set_err(err, err_size, "out of memory");
    free(b.data);
    return -1;
  }

  uint64_t dir_bytes = (uint64_t)dir_allocs[dir_index].cluster_count *
                       EXFAT_CLUSTER_SIZE;
  if(b.len > dir_bytes || buf_append_zero(&b, (size_t)(dir_bytes - b.len)) != 0) {
    set_err(err, err_size, "out of memory");
    free(b.data);
    return -1;
  }
  d->blob = b.data;
  d->blob_size = b.len;
  return 0;
}

static void
exfat_mark_cluster_range(unsigned char *bitmap, uint32_t first,
                         uint32_t count) {
  if(first < 2 || count == 0) return;
  for(uint32_t c = first; c < first + count; c++) {
    uint32_t bit = c - 2;
    bitmap[bit / 8] |= (unsigned char)(1U << (bit % 8));
  }
}

static void
exfat_set_fat_range(unsigned char *fat, uint32_t first, uint32_t count) {
  if(first < 2 || count == 0) return;
  for(uint32_t i = 0; i < count; i++) {
    uint32_t cluster = first + i;
    uint32_t value = i + 1 < count ? cluster + 1 : EXFAT_END_OF_CHAIN;
    le32(fat + (size_t)cluster * 4, value);
  }
}

static int
exfat_make_upcase_blob(pfs_layout_t *l, uint32_t *checksum,
                       char *err, size_t err_size) {
  byte_buf_t b = {0};
  uint32_t ch = 0;
  while(ch <= 0xffffU) {
    uint16_t up = exfat_upcase_char((uint16_t)ch);
    if(ch != 0xffffU && up == (uint16_t)ch) {
      uint32_t run = 1;
      while(ch + run < 0xffffU &&
            exfat_upcase_char((uint16_t)(ch + run)) ==
                (uint16_t)(ch + run) &&
            run < 0xffffU) {
        run++;
      }
      if(run > 1) {
        unsigned char marker[4];
        le16(marker, 0xffffU);
        le16(marker + 2, (uint16_t)run);
        if(buf_append(&b, marker, sizeof(marker)) != 0) {
          set_err(err, err_size, "out of memory");
          free(b.data);
          return -1;
        }
        ch += run;
        continue;
      }
    }

    unsigned char word[2];
    le16(word, up);
    if(buf_append(&b, word, sizeof(word)) != 0) {
      set_err(err, err_size, "out of memory");
      free(b.data);
      return -1;
    }
    if(ch == 0xffffU) break;
    ch++;
  }
  l->exfat_upcase_blob = b.data;
  l->exfat_upcase_blob_size = b.len;
  *checksum = exfat_table_checksum(l->exfat_upcase_blob,
                                   l->exfat_upcase_blob_size);
  return 0;
}

static int
exfat_make_boot_blob(pfs_layout_t *l, uint64_t volume_sectors,
                     uint32_t fat_offset, uint32_t fat_length,
                     uint32_t cluster_heap_offset, uint32_t cluster_count,
                     uint32_t root_cluster, uint8_t percent_in_use,
                     char *err, size_t err_size) {
  size_t size = (size_t)EXFAT_BOOT_REGION_SECTORS * (size_t)EXFAT_SECTOR_SIZE;
  l->exfat_boot_blob = calloc(1, size);
  if(!l->exfat_boot_blob) {
    set_err(err, err_size, "out of memory");
    return -1;
  }
  l->exfat_boot_blob_size = size;

  for(int copy = 0; copy < 2; copy++) {
    unsigned char *base = l->exfat_boot_blob +
      copy * 12U * (size_t)EXFAT_SECTOR_SIZE;
    unsigned char *boot = base;
    boot[0] = 0xEB;
    boot[1] = 0x76;
    boot[2] = 0x90;
    memcpy(boot + 3, "EXFAT   ", 8);
    le64(boot + 0x40, 0);
    le64(boot + 0x48, volume_sectors);
    le32(boot + 0x50, fat_offset);
    le32(boot + 0x54, fat_length);
    le32(boot + 0x58, cluster_heap_offset);
    le32(boot + 0x5c, cluster_count);
    le32(boot + 0x60, root_cluster);
    le32(boot + 0x64, (uint32_t)time(NULL));
    le16(boot + 0x68, 0x0100);
    le16(boot + 0x6a, 0);
    boot[0x6c] = 9;  /* 512-byte sectors */
    boot[0x6d] = 7;  /* 64 KiB clusters */
    boot[0x6e] = 1;  /* one FAT */
    boot[0x6f] = 0x80;
    boot[0x70] = percent_in_use;
    boot[510] = 0x55;
    boot[511] = 0xAA;
    for(int s = 1; s <= 8; s++) {
      unsigned char *ext = base + (size_t)s * (size_t)EXFAT_SECTOR_SIZE;
      ext[510] = 0x55;
      ext[511] = 0xAA;
    }
    uint32_t checksum = exfat_boot_checksum(base, (size_t)EXFAT_SECTOR_SIZE);
    unsigned char *check = base + 11U * (size_t)EXFAT_SECTOR_SIZE;
    for(size_t off = 0; off < EXFAT_SECTOR_SIZE; off += 4) {
      le32(check + off, checksum);
    }
  }
  return 0;
}

static int
build_exfat_layout_from_files(const char *root, const char *title_id,
                              pfs_layout_t *l,
                              char *err, size_t err_size) {
  scan_list_t scans = {0};
  exfat_allocation_t *dir_allocs = NULL;
  exfat_allocation_t *file_allocs = NULL;
  int rc = -1;
  uint32_t upcase_checksum = 0;

  job_set_current("Scanning app folder");
  if(scan_collect(root, "", &scans, err, err_size) != 0) goto done;
  if(scans.count == 0) {
    set_err(err, err_size, "app folder contains no files");
    goto done;
  }
  qsort(scans.items, scans.count, sizeof(scans.items[0]), scan_file_cmp);

  if(layout_add_dir(l, "", title_id ? title_id : "root", -1) != 0) {
    set_err(err, err_size, "out of memory");
    goto done;
  }
  for(size_t i = 0; i < scans.count; i++) {
    int parent = 0;
    if(ensure_parent_dirs(l, scans.items[i].rel, &parent) != 0) {
      set_err(err, err_size, "path too long");
      goto done;
    }
    int file_idx = layout_add_file(l, &scans.items[i], parent);
    if(file_idx < 0 ||
       int_list_push(&l->dirs[parent].child_files, file_idx) != 0) {
      set_err(err, err_size, "out of memory");
      goto done;
    }
  }

  dir_allocs = calloc(l->dir_count ? l->dir_count : 1, sizeof(*dir_allocs));
  file_allocs = calloc(l->file_count ? l->file_count : 1, sizeof(*file_allocs));
  if(!dir_allocs || !file_allocs) {
    set_err(err, err_size, "out of memory");
    goto done;
  }
  if(exfat_make_upcase_blob(l, &upcase_checksum, err, err_size) != 0) goto done;

  uint64_t upcase_clusters = ceil_div_u64(l->exfat_upcase_blob_size,
                                          EXFAT_CLUSTER_SIZE);
  uint64_t dir_clusters_total = 0;
  for(size_t i = 0; i < l->dir_count; i++) {
    uint64_t entries = exfat_estimate_dir_entries(l, (int)i);
    uint64_t clusters = ceil_div_u64(entries * 32ULL, EXFAT_CLUSTER_SIZE);
    if(clusters == 0) clusters = 1;
    dir_allocs[i].cluster_count = (uint32_t)clusters;
    dir_clusters_total += clusters;
  }
  uint64_t file_clusters_total = 0;
  for(size_t i = 0; i < l->file_count; i++) {
    uint64_t clusters = ceil_div_u64(l->files[i].raw_size, EXFAT_CLUSTER_SIZE);
    if(clusters > UINT32_MAX) {
      set_err(err, err_size, "file is too large for generated exFAT image");
      errno = EFBIG;
      goto done;
    }
    file_allocs[i].cluster_count = (uint32_t)clusters;
    file_clusters_total += clusters;
  }

  uint64_t bitmap_clusters = 1;
  for(;;) {
    uint64_t cluster_count64 = bitmap_clusters + upcase_clusters +
      dir_clusters_total + file_clusters_total + EXFAT_ROOT_SLACK_CLUSTERS;
    uint64_t bitmap_bytes = ceil_div_u64(cluster_count64, 8);
    uint64_t needed = ceil_div_u64(bitmap_bytes, EXFAT_CLUSTER_SIZE);
    if(needed == bitmap_clusters) break;
    bitmap_clusters = needed;
  }
  uint64_t cluster_count64 = bitmap_clusters + upcase_clusters +
    dir_clusters_total + file_clusters_total + EXFAT_ROOT_SLACK_CLUSTERS;
  if(cluster_count64 == 0 || cluster_count64 > 0xffffff00ULL) {
    set_err(err, err_size, "generated exFAT image is too large");
    errno = EFBIG;
    goto done;
  }
  uint32_t cluster_count = (uint32_t)cluster_count64;
  uint64_t bitmap_size = ceil_div_u64(cluster_count64, 8);
  if(bitmap_size > SIZE_MAX ||
     l->exfat_upcase_blob_size > SIZE_MAX ||
     bitmap_clusters > UINT32_MAX ||
     upcase_clusters > UINT32_MAX) {
    set_err(err, err_size, "generated exFAT image is too large");
    errno = EFBIG;
    goto done;
  }

  l->exfat_bitmap_blob_size = (size_t)bitmap_clusters *
                              (size_t)EXFAT_CLUSTER_SIZE;
  l->exfat_bitmap_blob = calloc(1, l->exfat_bitmap_blob_size);
  if(!l->exfat_bitmap_blob) {
    set_err(err, err_size, "out of memory");
    goto done;
  }

  uint32_t next_cluster = 2;
  uint32_t bitmap_cluster = next_cluster;
  next_cluster += (uint32_t)bitmap_clusters;
  uint32_t upcase_cluster = next_cluster;
  next_cluster += (uint32_t)upcase_clusters;
  for(size_t i = 0; i < l->dir_count; i++) {
    dir_allocs[i].first_cluster = next_cluster;
    next_cluster += dir_allocs[i].cluster_count;
  }
  for(size_t i = 0; i < l->file_count; i++) {
    if(file_allocs[i].cluster_count > 0) {
      file_allocs[i].first_cluster = next_cluster;
      next_cluster += file_allocs[i].cluster_count;
    }
  }

  exfat_mark_cluster_range(l->exfat_bitmap_blob, bitmap_cluster,
                           (uint32_t)bitmap_clusters);
  exfat_mark_cluster_range(l->exfat_bitmap_blob, upcase_cluster,
                           (uint32_t)upcase_clusters);
  for(size_t i = 0; i < l->dir_count; i++) {
    exfat_mark_cluster_range(l->exfat_bitmap_blob, dir_allocs[i].first_cluster,
                             dir_allocs[i].cluster_count);
  }
  for(size_t i = 0; i < l->file_count; i++) {
    exfat_mark_cluster_range(l->exfat_bitmap_blob,
                             file_allocs[i].first_cluster,
                             file_allocs[i].cluster_count);
  }

  for(size_t i = 0; i < l->dir_count; i++) {
    if(exfat_build_dir_blob(l, (int)i, dir_allocs, file_allocs,
                            bitmap_cluster, bitmap_size, upcase_cluster,
                            l->exfat_upcase_blob_size, upcase_checksum,
                            err, err_size) != 0) {
      goto done;
    }
  }

  uint64_t fat_entries = (uint64_t)cluster_count + 2ULL;
  uint64_t fat_bytes = fat_entries * 4ULL;
  uint64_t fat_sectors = ceil_div_u64(fat_bytes, EXFAT_SECTOR_SIZE);
  uint64_t fat_sectors_aligned = ceil_div_u64(fat_sectors,
                                              EXFAT_SECTORS_PER_CLUSTER) *
                                 EXFAT_SECTORS_PER_CLUSTER;
  uint32_t fat_offset = (uint32_t)EXFAT_FAT_OFFSET_SECTORS;
  uint32_t fat_length = (uint32_t)fat_sectors_aligned;
  uint32_t cluster_heap_offset = fat_offset + fat_length;
  uint64_t volume_sectors = (uint64_t)cluster_heap_offset +
                            (uint64_t)cluster_count *
                            EXFAT_SECTORS_PER_CLUSTER;
  if(volume_sectors > UINT32_MAX) {
    set_err(err, err_size, "generated exFAT image is too large");
    errno = EFBIG;
    goto done;
  }

  l->exfat_fat_blob_size = (size_t)fat_sectors_aligned *
                           (size_t)EXFAT_SECTOR_SIZE;
  l->exfat_fat_blob = calloc(1, l->exfat_fat_blob_size);
  if(!l->exfat_fat_blob) {
    set_err(err, err_size, "out of memory");
    goto done;
  }
  le32(l->exfat_fat_blob + 0, 0xfffffff8U);
  le32(l->exfat_fat_blob + 4, EXFAT_END_OF_CHAIN);
  exfat_set_fat_range(l->exfat_fat_blob, bitmap_cluster,
                      (uint32_t)bitmap_clusters);
  exfat_set_fat_range(l->exfat_fat_blob, upcase_cluster,
                      (uint32_t)upcase_clusters);
  for(size_t i = 0; i < l->dir_count; i++) {
    exfat_set_fat_range(l->exfat_fat_blob, dir_allocs[i].first_cluster,
                        dir_allocs[i].cluster_count);
  }
  for(size_t i = 0; i < l->file_count; i++) {
    exfat_set_fat_range(l->exfat_fat_blob, file_allocs[i].first_cluster,
                        file_allocs[i].cluster_count);
  }

  uint64_t allocated_clusters = bitmap_clusters + upcase_clusters +
    dir_clusters_total + file_clusters_total;
  uint8_t percent_in_use = cluster_count ?
    (uint8_t)((allocated_clusters * 100ULL + cluster_count - 1ULL) /
              cluster_count) : 0;
  if(percent_in_use > 100) percent_in_use = 100;
  if(exfat_make_boot_blob(l, volume_sectors, fat_offset, fat_length,
                          cluster_heap_offset, cluster_count,
                          dir_allocs[0].first_cluster, percent_in_use,
                          err, err_size) != 0) {
    goto done;
  }

  l->image_size = volume_sectors * EXFAT_SECTOR_SIZE;
  if(layout_add_segment(l, 0, l->exfat_boot_blob_size, PFS_SEG_MEM,
                        l->exfat_boot_blob, NULL) != 0 ||
     layout_add_segment(l, (uint64_t)fat_offset * EXFAT_SECTOR_SIZE,
                        l->exfat_fat_blob_size, PFS_SEG_MEM,
                        l->exfat_fat_blob, NULL) != 0 ||
     layout_add_segment(l, exfat_cluster_offset(cluster_heap_offset,
                                                bitmap_cluster),
                        l->exfat_bitmap_blob_size, PFS_SEG_MEM,
                        l->exfat_bitmap_blob, NULL) != 0 ||
     layout_add_segment(l, exfat_cluster_offset(cluster_heap_offset,
                                                upcase_cluster),
                        l->exfat_upcase_blob_size, PFS_SEG_MEM,
                        l->exfat_upcase_blob, NULL) != 0) {
    set_err(err, err_size, "out of memory");
    goto done;
  }
  for(size_t i = 0; i < l->dir_count; i++) {
    if(layout_add_segment(l, exfat_cluster_offset(cluster_heap_offset,
                                                  dir_allocs[i].first_cluster),
                          l->dirs[i].blob_size, PFS_SEG_MEM,
                          l->dirs[i].blob, NULL) != 0) {
      set_err(err, err_size, "out of memory");
      goto done;
    }
  }
  for(size_t i = 0; i < l->file_count; i++) {
    uint64_t file_offset;
    if(file_allocs[i].cluster_count == 0) {
      l->files[i].block_start = 0;
      l->files[i].blocks = 0;
      continue;
    }
    file_offset = exfat_cluster_offset(cluster_heap_offset,
                                       file_allocs[i].first_cluster);
    l->files[i].block_start = file_offset / PFS_BLOCK_SIZE;
    l->files[i].blocks = ceil_div_u64(l->files[i].raw_size, PFS_BLOCK_SIZE);
    if(layout_add_segment(l, file_offset, l->files[i].raw_size, PFS_SEG_FILE,
                          NULL, l->files[i].abs) != 0) {
      set_err(err, err_size, "out of memory");
      goto done;
    }
  }

  rc = 0;

done:
  free(scans.items);
  free(dir_allocs);
  free(file_allocs);
  return rc;
}

static void
layout_free(pfs_layout_t *l) {
  if(!l) return;
  for(size_t i = 0; i < l->dir_count; i++) {
    free(l->dirs[i].child_dirs.items);
    free(l->dirs[i].child_files.items);
    free(l->dirs[i].blob);
  }
  free(l->dirs);
  free(l->files);
  free(l->segments);
  free(l->header_blob);
  free(l->inode_blob);
  free(l->superroot_blob);
  free(l->fpt_blob);
  free(l->collision_blob);
  free(l->exfat_boot_blob);
  free(l->exfat_fat_blob);
  free(l->exfat_bitmap_blob);
  free(l->exfat_upcase_blob);
  memset(l, 0, sizeof(*l));
}

static void
virtual_reader_init(virtual_reader_t *vr) {
  size_t cap = PFS_READ_CACHE_SIZE;
  memset(vr, 0, sizeof(*vr));
  vr->fd = -1;
  vr->open_seg = -1;
  vr->cache_seg = -1;
  while(cap >= PFS_READ_CACHE_MIN_SIZE) {
    vr->cache = malloc(cap);
    if(vr->cache) {
      vr->cache_cap = cap;
      return;
    }
    cap /= 2;
  }
}

static void
virtual_reader_close_file(virtual_reader_t *vr) {
  if(vr && vr->fd >= 0) close(vr->fd);
  if(vr) {
    vr->fd = -1;
    vr->open_seg = -1;
    vr->cache_len = 0;
    vr->cache_offset = 0;
    vr->cache_seg = -1;
  }
}

static void
virtual_reader_free(virtual_reader_t *vr) {
  if(!vr) return;
  virtual_reader_close_file(vr);
  free(vr->cache);
  memset(vr, 0, sizeof(*vr));
  vr->fd = -1;
  vr->open_seg = -1;
  vr->cache_seg = -1;
}

static int
virtual_reader_file_read(virtual_reader_t *vr, ssize_t seg_index,
                         uint64_t file_size, uint64_t file_offset,
                         unsigned char *out, size_t size) {
  if(!vr->cache || vr->cache_cap == 0) {
    return read_exact_at(vr->fd, out, size, (off_t)file_offset);
  }

  while(size > 0) {
    uint64_t cache_end = vr->cache_offset + vr->cache_len;
    if(vr->cache_seg != seg_index ||
       file_offset < vr->cache_offset ||
       file_offset >= cache_end) {
      uint64_t remaining = file_size > file_offset ? file_size - file_offset : 0;
      size_t fill = remaining > vr->cache_cap ? vr->cache_cap : (size_t)remaining;
      if(fill == 0) {
        errno = EIO;
        return -1;
      }
      if(read_exact_at(vr->fd, vr->cache, fill, (off_t)file_offset) != 0) {
        return -1;
      }
      vr->cache_seg = seg_index;
      vr->cache_offset = file_offset;
      vr->cache_len = fill;
      cache_end = vr->cache_offset + vr->cache_len;
    }

    size_t cache_off = (size_t)(file_offset - vr->cache_offset);
    size_t avail = (size_t)(cache_end - file_offset);
    size_t n = avail < size ? avail : size;
    memcpy(out, vr->cache + cache_off, n);
    out += n;
    file_offset += n;
    size -= n;
  }
  return 0;
}

static int
virtual_reader_read(pfs_layout_t *l, virtual_reader_t *vr, uint64_t offset,
                    unsigned char *out, size_t size,
                    char *err, size_t err_size) {
  memset(out, 0, size);
  if(offset >= l->image_size) return 0;

  uint64_t end = offset + size;
  while(vr->seg_index < l->segment_count &&
        l->segments[vr->seg_index].offset + l->segments[vr->seg_index].size <= offset) {
    vr->seg_index++;
  }

  for(size_t i = vr->seg_index; i < l->segment_count; i++) {
    pfs_segment_t *s = &l->segments[i];
    uint64_t seg_end = s->offset + s->size;
    if(s->offset >= end) break;
    if(seg_end <= offset) continue;

    uint64_t copy_start = s->offset > offset ? s->offset : offset;
    uint64_t copy_end = seg_end < end ? seg_end : end;
    size_t n = (size_t)(copy_end - copy_start);
    size_t dst_off = (size_t)(copy_start - offset);
    size_t src_off = (size_t)(copy_start - s->offset);

    if(s->type == PFS_SEG_MEM) {
      memcpy(out + dst_off, s->mem + src_off, n);
    } else if(s->type == PFS_SEG_FILE) {
      if(vr->open_seg != (ssize_t)i) {
        virtual_reader_close_file(vr);
        vr->fd = open(s->path, O_RDONLY);
        if(vr->fd < 0) {
          set_err(err, err_size, "open source file: %s", strerror(errno));
          return -1;
        }
        vr->open_seg = (ssize_t)i;
      }
      if(virtual_reader_file_read(vr, (ssize_t)i, s->size, src_off,
                                  out + dst_off, n) != 0) {
        set_err(err, err_size, "read source file: %s", strerror(errno));
        return -1;
      }
    }
  }
  return 0;
}

static int
layout_block_overlaps_executable_file(const pfs_layout_t *l, uint64_t offset) {
  uint64_t end = offset + PFS_BLOCK_SIZE;
  if(!l) return 0;
  for(size_t i = 0; i < l->segment_count; i++) {
    const pfs_segment_t *s = &l->segments[i];
    uint64_t seg_end = s->offset + s->size;
    if(s->offset >= end) break;
    if(seg_end <= offset || s->type != PFS_SEG_FILE) continue;
    if(path_is_executable_payload(s->path)) return 1;
  }
  return 0;
}

static uint64_t
pfsc_header_span(uint64_t block_count) {
  uint64_t pointer_table_size = (block_count + 1) * PFSC_OFFSET_ENTRY_SIZE;
  uint64_t initial_capacity = PFSC_INITIAL_DATA_OFFSET - PFSC_BLOCK_OFFSETS_OFFSET;
  uint64_t extra = pointer_table_size > initial_capacity
                       ? pointer_table_size - initial_capacity
                       : 0;
  return PFSC_INITIAL_DATA_OFFSET + ceil_div_u64(extra, PFS_BLOCK_SIZE) * PFS_BLOCK_SIZE;
}

static int
write_pfsc_header(int fd, uint64_t file_start, uint64_t header_size,
                  uint64_t logical_size, const uint64_t *offsets,
                  uint64_t block_count, char *err, size_t err_size) {
  unsigned char *header = calloc(1, (size_t)header_size);
  if(!header) {
    set_err(err, err_size, "out of memory");
    return -1;
  }
  le32(header + 0x00, PFSC_MAGIC);
  le32(header + 0x04, PFSC_UNK4);
  le32(header + 0x08, PFSC_UNK8);
  le32(header + 0x0c, (uint32_t)PFS_BLOCK_SIZE);
  le64(header + 0x10, PFS_BLOCK_SIZE);
  le64(header + 0x18, PFSC_BLOCK_OFFSETS_OFFSET);
  le64(header + 0x20, header_size);
  le64(header + 0x28, logical_size);
  for(uint64_t i = 0; i <= block_count; i++) {
    le64(header + PFSC_BLOCK_OFFSETS_OFFSET + i * 8, offsets[i]);
  }
  int rc = pwrite_all_local(fd, header, (size_t)header_size, (off_t)file_start);
  if(rc != 0) set_err(err, err_size, "write PFSC header: %s", strerror(errno));
  free(header);
  return rc;
}

static size_t
pfsc_compress_block_smaller(const unsigned char *raw, unsigned char *out,
                            mz_uint flags, tdefl_compressor *comp) {
  size_t in_size = (size_t)PFS_BLOCK_SIZE;
  size_t out_size = (size_t)PFS_BLOCK_SIZE - 1;
  if(tdefl_init(comp, NULL, NULL, (int)flags) != TDEFL_STATUS_OKAY) {
    return 0;
  }
  tdefl_status st = tdefl_compress(comp, raw, &in_size, out, &out_size,
                                   TDEFL_FINISH);
  if(st == TDEFL_STATUS_DONE &&
     in_size == (size_t)PFS_BLOCK_SIZE &&
     out_size < (size_t)PFS_BLOCK_SIZE) {
    return out_size;
  }
  return 0;
}

static int
pfsc_find_ready_slot(pfsc_pool_t *pool) {
  for(int i = 0; i < pool->slot_count; i++) {
    if(pool->slots[i].state == PFSC_SLOT_READY) return i;
  }
  return -1;
}

static void
pfsc_pool_set_error(pfsc_pool_t *pool, int err) {
  pthread_mutex_lock(&pool->lock);
  if(!pool->error) pool->error = err ? err : EIO;
  pool->stop = 1;
  pthread_cond_broadcast(&pool->cond);
  pthread_mutex_unlock(&pool->lock);
}

static void *
pfsc_worker_main(void *arg) {
  pfsc_pool_t *pool = arg;
  tdefl_compressor *comp = malloc(sizeof(*comp));
  if(!comp) {
    pfsc_pool_set_error(pool, ENOMEM);
    return NULL;
  }

  for(;;) {
    pthread_mutex_lock(&pool->lock);
    int slot_index;
    while(!pool->stop && (slot_index = pfsc_find_ready_slot(pool)) < 0) {
      uint64_t wait_started = monotonic_us();
      pthread_cond_wait(&pool->cond, &pool->lock);
      job_add_wait_us(&g_job.worker_wait_us, wait_started);
    }
    if(pool->stop) {
      pthread_mutex_unlock(&pool->lock);
      break;
    }
    pfsc_slot_t *slot = &pool->slots[slot_index];
    slot->state = PFSC_SLOT_BUSY;
    pthread_mutex_unlock(&pool->lock);

    if(slot->force_raw) {
      slot->comp_len = 0;
      atomic_fetch_add(&g_job.skipped_zlib_blocks, 1);
    } else {
      slot->comp_len = pfsc_compress_block_smaller(slot->raw, slot->comp,
                                                   pool->flags, comp);
    }

    pthread_mutex_lock(&pool->lock);
    slot->state = PFSC_SLOT_DONE;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->lock);
  }

  free(comp);
  return NULL;
}

static void
pfsc_pool_stop(pfsc_pool_t *pool) {
  pthread_mutex_lock(&pool->lock);
  pool->stop = 1;
  pthread_cond_broadcast(&pool->cond);
  pthread_mutex_unlock(&pool->lock);
}

static void
pfsc_free_slots(pfsc_slot_t *slots, int slot_count) {
  if(!slots) return;
  for(int i = 0; i < slot_count; i++) {
    free(slots[i].raw);
    free(slots[i].comp);
  }
  free(slots);
}

static int
compress_nested_to_pfsc(int fd, uint64_t file_start, pfs_layout_t *nested,
                        int requested_workers, int delete_stream,
                        const char *nested_name,
                        const char *source_root, int *delete_started,
                        uint64_t *stored_size,
                        char *err, size_t err_size) {
  uint64_t block_count = ceil_div_u64(nested->image_size, PFS_BLOCK_SIZE);
  uint64_t logical_size = block_count * PFS_BLOCK_SIZE;
  uint64_t header_size = pfsc_header_span(block_count);
  uint64_t *offsets = calloc((size_t)(block_count + 1), sizeof(*offsets));
  int worker_count = clamp_worker_count(requested_workers);
  int slot_count;
  pfsc_slot_t *slots = NULL;
  pthread_t *threads = NULL;
  int workers_started = 0;
  int pool_initialized = 0;
  pfsc_pool_t pool;
  pfsc_output_buffer_t outbuf;
  virtual_reader_t vr;
  int rc = -1;
  uint64_t data_pos = header_size;
  size_t next_delete_file = 0;
  mz_uint flags = tdefl_create_comp_flags_from_zip_params(PFSC_ZLIB_LEVEL, 15,
                                                          MZ_DEFAULT_STRATEGY);

  memset(&pool, 0, sizeof(pool));
  pfsc_output_buffer_init(&outbuf);
  virtual_reader_init(&vr);

  if(block_count < (uint64_t)worker_count) worker_count = (int)block_count;
  if(worker_count < 1) worker_count = 1;
  slot_count = worker_count * PFSC_SLOTS_PER_WORKER;
  if(block_count < (uint64_t)slot_count) slot_count = (int)block_count;
  if(slot_count < 1) slot_count = 1;

  slots = calloc((size_t)slot_count, sizeof(*slots));
  threads = calloc((size_t)worker_count, sizeof(*threads));
  if(!offsets || !slots || !threads) {
    set_err(err, err_size, "out of memory");
    goto done;
  }
  for(int i = 0; i < slot_count; i++) {
    slots[i].raw = malloc((size_t)PFS_BLOCK_SIZE);
    slots[i].comp = malloc((size_t)PFS_BLOCK_SIZE);
    if(!slots[i].raw || !slots[i].comp) {
      set_err(err, err_size, "out of memory");
      goto done;
    }
  }
  if(pthread_mutex_init(&pool.lock, NULL) != 0 ||
     pthread_cond_init(&pool.cond, NULL) != 0) {
    set_err(err, err_size, "init compression tasks failed");
    goto done;
  }
  pool_initialized = 1;
  pool.slots = slots;
  pool.slot_count = slot_count;
  pool.flags = flags;

  for(int i = 0; i < worker_count; i++) {
    int trc = pthread_create(&threads[i], NULL, pfsc_worker_main, &pool);
    if(trc != 0) {
      set_err(err, err_size, "start compression task: %s", strerror(trc));
      goto done;
    }
    workers_started++;
  }

  offsets[0] = header_size;
  atomic_store(&g_job.total_bytes,
               nested->image_size > (uint64_t)LONG_MAX ? LONG_MAX :
               (long)nested->image_size);
  atomic_store(&g_job.copied_bytes, 0);
  atomic_store(&g_job.compressed_output_bytes, 0);
  atomic_store(&g_job.raw_blocks, 0);
  atomic_store(&g_job.compressed_blocks, 0);
  atomic_store(&g_job.skipped_zlib_blocks, 0);
  atomic_store(&g_job.total_blocks,
               block_count > (uint64_t)LONG_MAX ? LONG_MAX :
               (long)block_count);
  atomic_store(&g_job.writer_wait_us, 0);
  atomic_store(&g_job.worker_wait_us, 0);
  atomic_store(&g_job.total_files,
               block_count > (uint64_t)INT_MAX ? INT_MAX : (int)block_count);
  atomic_store(&g_job.done_files, 0);
  char label[320];
  snprintf(label, sizeof(label), "Compressing %s",
           nested_name && nested_name[0] ? nested_name : "nested image");
  job_set_current(label);

  uint64_t next_read = 0;
  uint64_t next_write = 0;
  while(next_write < block_count) {
    while(next_read < block_count &&
          next_read - next_write < (uint64_t)slot_count) {
      pfsc_slot_t *slot = &slots[next_read % (uint64_t)slot_count];

      pthread_mutex_lock(&pool.lock);
      while(slot->state != PFSC_SLOT_FREE && !pool.error) {
        pthread_cond_wait(&pool.cond, &pool.lock);
      }
      if(pool.error) {
        int saved = pool.error;
        pthread_mutex_unlock(&pool.lock);
        errno = saved;
        set_err(err, err_size, "compression task failed: %s", strerror(saved));
        goto done;
      }
      slot->state = PFSC_SLOT_FILLING;
      slot->index = next_read;
      slot->comp_len = 0;
      slot->force_raw = 0;
      pthread_mutex_unlock(&pool.lock);

      if(job_cancelled()) {
        pthread_mutex_lock(&pool.lock);
        slot->state = PFSC_SLOT_FREE;
        pthread_cond_broadcast(&pool.cond);
        pthread_mutex_unlock(&pool.lock);
        set_err(err, err_size, "cancelled");
        errno = EINTR;
        goto done;
      }

      if(virtual_reader_read(nested, &vr, next_read * PFS_BLOCK_SIZE,
                             slot->raw, (size_t)PFS_BLOCK_SIZE,
                             err, err_size) != 0) {
        pthread_mutex_lock(&pool.lock);
        slot->state = PFSC_SLOT_FREE;
        pthread_cond_broadcast(&pool.cond);
        pthread_mutex_unlock(&pool.lock);
        goto done;
      }
      slot->force_raw =
          layout_block_overlaps_executable_file(nested,
                                                next_read * PFS_BLOCK_SIZE);
      pthread_mutex_lock(&pool.lock);
      slot->state = PFSC_SLOT_READY;
      pthread_cond_broadcast(&pool.cond);
      pthread_mutex_unlock(&pool.lock);
      next_read++;
    }

    pfsc_slot_t *slot = &slots[next_write % (uint64_t)slot_count];
    pthread_mutex_lock(&pool.lock);
    while((slot->state != PFSC_SLOT_DONE || slot->index != next_write) &&
          !pool.error) {
      uint64_t wait_started = monotonic_us();
      pthread_cond_wait(&pool.cond, &pool.lock);
      job_add_wait_us(&g_job.writer_wait_us, wait_started);
    }
    if(pool.error) {
      int saved = pool.error;
      pthread_mutex_unlock(&pool.lock);
      errno = saved;
      set_err(err, err_size, "compression task failed: %s", strerror(saved));
      goto done;
    }
    pthread_mutex_unlock(&pool.lock);

    const void *chosen = slot->raw;
    size_t chosen_len = (size_t)PFS_BLOCK_SIZE;
    int block_compressed = 0;
    if(slot->comp_len > 0 && slot->comp_len < (size_t)PFS_BLOCK_SIZE) {
      chosen = slot->comp;
      chosen_len = slot->comp_len;
      block_compressed = 1;
    }
    if(pfsc_output_buffer_write(fd, file_start, &outbuf, data_pos,
                                chosen, chosen_len, err, err_size) != 0) {
      goto done;
    }
    data_pos += chosen_len;
    offsets[next_write + 1] = data_pos;
    uint64_t compressed_output = data_pos > header_size ? data_pos - header_size : 0;
    atomic_store(&g_job.compressed_output_bytes,
                 compressed_output > (uint64_t)LONG_MAX ? LONG_MAX :
                 (long)compressed_output);
    if(block_compressed) atomic_fetch_add(&g_job.compressed_blocks, 1);
    else atomic_fetch_add(&g_job.raw_blocks, 1);

    pthread_mutex_lock(&pool.lock);
    slot->state = PFSC_SLOT_FREE;
    pthread_cond_broadcast(&pool.cond);
    pthread_mutex_unlock(&pool.lock);

    uint64_t copied = (next_write + 1) * PFS_BLOCK_SIZE;
    if(copied > nested->image_size) copied = nested->image_size;
    atomic_store(&g_job.copied_bytes,
                 copied > (uint64_t)LONG_MAX ? LONG_MAX : (long)copied);
    atomic_store(&g_job.done_files,
                 next_write + 1 > (uint64_t)INT_MAX ? INT_MAX :
                 (int)(next_write + 1));
    if(delete_stream &&
       delete_committed_source_files(source_root, nested, &vr, fd, file_start,
                                     &outbuf, next_write + 1,
                                     &next_delete_file,
                                     delete_started, err, err_size) != 0) {
      goto done;
    }
    next_write++;
  }

  pfsc_pool_stop(&pool);
  for(int i = 0; i < workers_started; i++) pthread_join(threads[i], NULL);
  workers_started = 0;

  job_set_current("Writing PFSC header");
  if(pfsc_output_buffer_flush(fd, file_start, &outbuf, err, err_size) != 0) {
    goto done;
  }
  if(write_pfsc_header(fd, file_start, header_size, logical_size, offsets,
                       block_count, err, err_size) != 0) {
    goto done;
  }
  *stored_size = data_pos;
  rc = 0;

done:
  if(pool_initialized) pfsc_pool_stop(&pool);
  for(int i = 0; i < workers_started; i++) pthread_join(threads[i], NULL);
  if(pool_initialized) {
    pthread_cond_destroy(&pool.cond);
    pthread_mutex_destroy(&pool.lock);
  }
  virtual_reader_free(&vr);
  pfsc_output_buffer_free(&outbuf);
  free(offsets);
  free(threads);
  pfsc_free_slots(slots, slot_count);
  return rc;
}

static int
write_blob_padded(int fd, uint64_t block, const unsigned char *blob,
                  size_t blob_size, char *err, size_t err_size) {
  if(blob_size == 0) return 0;
  if(pwrite_all_local(fd, blob, blob_size, (off_t)(block * PFS_BLOCK_SIZE)) != 0) {
    set_err(err, err_size, "write PFS metadata: %s", strerror(errno));
    return -1;
  }
  return 0;
}

static int
write_outer_pfs_metadata(int fd, uint64_t nested_size, uint64_t stored_size,
                         const char *nested_name,
                         char *err, size_t err_size) {
  const uint64_t inode_count = 4;
  const uint64_t inode_block_count = 1;
  const uint64_t superroot_block = 2;
  const uint64_t fpt_block = 3;
  const uint64_t uroot_block = 5;
  const uint64_t file_block = 6;
  uint64_t file_blocks = ceil_div_u64(stored_size, PFS_BLOCK_SIZE);
  if(file_blocks == 0) file_blocks = 1;
  uint64_t final_ndblock = file_block + file_blocks;
  time_t now = time(NULL);
  unsigned char *header = calloc(1, (size_t)PFS_BLOCK_SIZE);
  unsigned char *inode_block = calloc(1, (size_t)PFS_BLOCK_SIZE);
  byte_buf_t superroot = {0};
  byte_buf_t root = {0};
  unsigned char fpt[8];
  int rc = -1;

  if(!header || !inode_block) {
    set_err(err, err_size, "out of memory");
    goto done;
  }
  if(ftruncate(fd, (off_t)(final_ndblock * PFS_BLOCK_SIZE)) != 0) {
    set_err(err, err_size, "truncate output: %s", strerror(errno));
    goto done;
  }

  write_header_blob(header, inode_count, inode_block_count, final_ndblock, now);
  write_inode(inode_block + 0 * PFS_INODE_SIZE,
              PFS_INODE_MODE_DIR | PFS_INODE_RWX_ALL, 1,
              PFS_INODE_FLAG_INTERNAL | PFS_INODE_FLAG_READONLY,
              PFS_BLOCK_SIZE, PFS_BLOCK_SIZE, 1, (int32_t)superroot_block, 0,
              now);
  write_inode(inode_block + 1 * PFS_INODE_SIZE,
              PFS_INODE_MODE_FILE | PFS_INODE_RWX_ALL, 1,
              PFS_INODE_FLAG_INTERNAL | PFS_INODE_FLAG_READONLY,
              sizeof(fpt), sizeof(fpt), 1, (int32_t)fpt_block, 1, now);
  write_inode(inode_block + 2 * PFS_INODE_SIZE,
              PFS_INODE_MODE_DIR | PFS_INODE_RWX_ALL, 3,
              PFS_INODE_FLAG_READONLY,
              PFS_BLOCK_SIZE, PFS_BLOCK_SIZE, 1, (int32_t)uroot_block, 1, now);
  write_inode(inode_block + 3 * PFS_INODE_SIZE,
              PFS_INODE_MODE_FILE | PFS_INODE_RWX_ALL, 1,
              PFS_INODE_FLAG_READONLY | PFS_INODE_FLAG_COMPRESSED,
              stored_size, nested_size, (uint32_t)file_blocks,
              (int32_t)file_block, 1, now);

  char nested_path[320];
  int path_len = snprintf(nested_path, sizeof(nested_path), "/%s",
                          nested_name && nested_name[0]
                            ? nested_name
                            : "pfs_image.dat");
  if(path_len < 0 || (size_t)path_len >= sizeof(nested_path)) {
    set_err(err, err_size, "nested image name too long");
    goto done;
  }
  uint32_t hash = pfs_hash_path(nested_path);
  le32(fpt + 0, hash);
  le32(fpt + 4, 3);

  if(append_dirent(&superroot, 1, PFS_DIRENT_TYPE_FILE,
                   "flat_path_table") != 0 ||
     append_dirent(&superroot, 2, PFS_DIRENT_TYPE_DIRECTORY,
                   "uroot") != 0 ||
     append_dirent(&root, 2, PFS_DIRENT_TYPE_DOT, ".") != 0 ||
     append_dirent(&root, 2, PFS_DIRENT_TYPE_DOTDOT, "..") != 0 ||
     append_dirent(&root, 3, PFS_DIRENT_TYPE_FILE,
                   nested_name && nested_name[0]
                     ? nested_name
                     : "pfs_image.dat") != 0) {
    set_err(err, err_size, "out of memory");
    goto done;
  }

  if(pwrite_all_local(fd, header, (size_t)PFS_BLOCK_SIZE, 0) != 0 ||
     pwrite_all_local(fd, inode_block, (size_t)PFS_BLOCK_SIZE,
                      (off_t)PFS_BLOCK_SIZE) != 0 ||
     write_blob_padded(fd, superroot_block, superroot.data, superroot.len,
                       err, err_size) != 0 ||
     write_blob_padded(fd, fpt_block, fpt, sizeof(fpt),
                       err, err_size) != 0 ||
     write_blob_padded(fd, uroot_block, root.data, root.len,
                       err, err_size) != 0) {
    if(!err[0]) set_err(err, err_size, "write PFS metadata: %s", strerror(errno));
    goto done;
  }
  rc = 0;

done:
  free(header);
  free(inode_block);
  free(superroot.data);
  free(root.data);
  return rc;
}

static int
read_file_limited(const char *path, char **out, size_t *out_size,
                  size_t max_size) {
  FILE *f = fopen(path, "rb");
  if(!f) return -1;
  char *buf = malloc(max_size + 1);
  if(!buf) {
    fclose(f);
    errno = ENOMEM;
    return -1;
  }
  size_t n = fread(buf, 1, max_size, f);
  int ferr = ferror(f);
  fclose(f);
  if(ferr) {
    free(buf);
    errno = EIO;
    return -1;
  }
  buf[n] = 0;
  *out = buf;
  *out_size = n;
  return 0;
}

static int
json_find_string_value(const char *json, const char *key,
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
read_title_id(const char *app_path, char *title, size_t title_size,
              char *err, size_t err_size) {
  char param[1024];
  char sce_sys[1024];
  char *json = NULL;
  size_t json_size = 0;
  if(join_abs(sce_sys, sizeof(sce_sys), app_path, "sce_sys") != 0 ||
     join_abs(param, sizeof(param), sce_sys, "param.json") != 0) {
    set_err(err, err_size, "param path too long");
    return -1;
  }
  if(read_file_limited(param, &json, &json_size, 128 * 1024) != 0) {
    set_err(err, err_size, "missing sce_sys/param.json");
    return -1;
  }
  (void)json_size;
  int ok = json_find_string_value(json, "titleId", title, title_size) ||
           json_find_string_value(json, "title_id", title, title_size);
  free(json);
  if(!ok || !title_id_safe(title)) {
    set_err(err, err_size, "param.json is missing a valid titleId");
    return -1;
  }
  return 0;
}

static int
has_root_boot_file(const char *app_path) {
  char p[1024];
  struct stat st;
  if(join_abs(p, sizeof(p), app_path, "eboot.bin") == 0 &&
     stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
    return 1;
  }
  if(join_abs(p, sizeof(p), app_path, "iboot.bin") == 0 &&
     stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
    return 1;
  }
  return 0;
}

int
pfs_app_probe(const char *path, pfs_app_info_t *info,
              char *err, size_t err_size) {
  char clean[1024], parent[1024], base[256];
  struct stat st;
  pfs_app_info_t local;
  if(!info) info = &local;
  memset(info, 0, sizeof(*info));

  if(normalize_app_path(path, clean, sizeof(clean)) != 0) {
    set_err(err, err_size, "bad path");
    return -1;
  }
  if(stat(clean, &st) != 0 || !S_ISDIR(st.st_mode)) {
    set_err(err, err_size, "not a folder");
    return -1;
  }
  if(path_parent_base(clean, parent, sizeof(parent), base, sizeof(base)) != 0) {
    set_err(err, err_size, "bad app path");
    return -1;
  }
  if(read_title_id(clean, info->title_id, sizeof(info->title_id),
                   err, err_size) != 0) {
    return -1;
  }
  if(!has_root_boot_file(clean)) {
    set_err(err, err_size, "missing root eboot.bin or iboot.bin");
    return -1;
  }

  snprintf(info->source_path, sizeof(info->source_path), "%s", clean);
  int n = snprintf(info->output_path, sizeof(info->output_path), "%s%s%s.ffpfsc",
                   parent, parent[1] ? "/" : "", info->title_id);
  if(n < 0 || (size_t)n >= sizeof(info->output_path)) {
    set_err(err, err_size, "output path too long");
    return -1;
  }
  info->output_exists = stat(info->output_path, &st) == 0;
  return 0;
}

int
pfs_compress_app_to_ffpfsc_opts(const char *path, int overwrite,
                                int workers, int format,
                                int delete_policy,
                                pfs_app_info_t *info,
                                char *err, size_t err_size) {
  pfs_app_info_t local_info;
  pfs_layout_t nested = {0};
  char tmp_path[1024];
  int fd = -1;
  int rc = -1;
  int delete_started = 0;
  uint64_t stored_size = 0;
  const uint64_t outer_file_start = 6 * PFS_BLOCK_SIZE;
  int worker_count = clamp_worker_count(workers);

  if(!info) info = &local_info;
  if(format != PFS_COMPRESS_FORMAT_PFS &&
     format != PFS_COMPRESS_FORMAT_EXFAT) {
    set_err(err, err_size, "unsupported compression format");
    errno = EINVAL;
    return -1;
  }
  if(delete_policy != PFS_DELETE_KEEP &&
     delete_policy != PFS_DELETE_AFTER &&
     delete_policy != PFS_DELETE_STREAM) {
    set_err(err, err_size, "unsupported delete policy");
    errno = EINVAL;
    return -1;
  }
  if(pfs_app_probe(path, info, err, err_size) != 0) return -1;
  info->format = format;
  info->delete_policy = delete_policy;
  snprintf(info->nested_name, sizeof(info->nested_name), "%s",
           format == PFS_COMPRESS_FORMAT_EXFAT ? info->title_id : "pfs_image.dat");
  if(format == PFS_COMPRESS_FORMAT_EXFAT) {
    size_t used = strlen(info->nested_name);
    if(used + strlen(".exfat") >= sizeof(info->nested_name)) {
      set_err(err, err_size, "nested image name too long");
      errno = ENAMETOOLONG;
      return -1;
    }
    snprintf(info->nested_name + used, sizeof(info->nested_name) - used,
             ".exfat");
  }
  if(info->output_exists && !overwrite) {
    set_err(err, err_size, "output exists");
    errno = EEXIST;
    return -2;
  }

  if(snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", info->output_path) >=
     (int)sizeof(tmp_path)) {
    set_err(err, err_size, "temporary output path too long");
    return -1;
  }

  job_set_current(format == PFS_COMPRESS_FORMAT_EXFAT
                    ? "Building nested exFAT layout"
                    : "Building nested PFS layout");
  if(format == PFS_COMPRESS_FORMAT_EXFAT) {
    if(build_exfat_layout_from_files(info->source_path, info->title_id,
                                     &nested, err, err_size) != 0) {
      goto done;
    }
  } else if(build_layout_from_files(info->source_path, &nested,
                                    err, err_size) != 0) {
    goto done;
  }

  fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if(fd < 0) {
    set_err(err, err_size, "open output: %s", strerror(errno));
    goto done;
  }
  if(ftruncate(fd, (off_t)outer_file_start) != 0) {
    set_err(err, err_size, "reserve output: %s", strerror(errno));
    goto done;
  }

  if(compress_nested_to_pfsc(fd, outer_file_start, &nested, worker_count,
                             delete_policy == PFS_DELETE_STREAM,
                             info->nested_name, info->source_path,
                             &delete_started, &stored_size,
                             err, err_size) != 0) {
    goto done;
  }

  if(job_cancelled()) {
    set_err(err, err_size, "cancelled");
    errno = EINTR;
    goto done;
  }

  job_set_current("Finalizing .ffpfsc");
  if(write_outer_pfs_metadata(fd, nested.image_size, stored_size,
                              info->nested_name, err, err_size) != 0) {
    goto done;
  }
  close(fd);
  fd = -1;

  if(rename(tmp_path, info->output_path) != 0) {
    set_err(err, err_size, "rename output: %s", strerror(errno));
    goto done;
  }
  info->output_exists = 1;

  if(delete_policy == PFS_DELETE_STREAM) {
    job_set_current("Removing source app folder");
    if(rmdir(info->source_path) != 0 && errno != ENOENT) {
      set_err(err, err_size, "remove source app folder: %s", strerror(errno));
      goto done;
    }
  } else if(delete_policy == PFS_DELETE_AFTER) {
    job_set_current("Removing source app folder");
    if(remove_tree_local(info->source_path) != 0) {
      set_err(err, err_size, "remove source app folder: %s", strerror(errno));
      goto done;
    }
  }
  rc = 0;

done:
  if(fd >= 0) close(fd);
  if(rc != 0 &&
     (delete_policy != PFS_DELETE_STREAM || !delete_started)) {
    unlink(tmp_path);
  }
  layout_free(&nested);
  return rc;
}

int
pfs_compress_app_to_ffpfsc_ex(const char *path, int overwrite,
                              int workers, int convert,
                              pfs_app_info_t *info,
                              char *err, size_t err_size) {
  return pfs_compress_app_to_ffpfsc_opts(path, overwrite, workers,
                                         PFS_COMPRESS_FORMAT_PFS,
                                         convert ? PFS_DELETE_STREAM
                                                 : PFS_DELETE_KEEP,
                                         info, err, err_size);
}

int
pfs_compress_app_to_ffpfsc(const char *path, int overwrite,
                           pfs_app_info_t *info,
                           char *err, size_t err_size) {
  return pfs_compress_app_to_ffpfsc_opts(path, overwrite,
                                         PFS_COMPRESS_DEFAULT_WORKERS,
                                         PFS_COMPRESS_FORMAT_PFS,
                                         PFS_DELETE_KEEP,
                                         info, err, err_size);
}
