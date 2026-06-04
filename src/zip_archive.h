/*
 * BFpilot - ZIP and bounded archive pipe internals.
 */

#pragma once

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "websrv.h"

#define ZIP_STREAM_BUF_SIZE (1024 * 1024)
#define ARCHIVE_PIPE_SIZE (8 * 1024 * 1024)
#define ZIP_PIPE_READ_SIZE (256 * 1024)
#define ZIP_NAME_MAX 768
#define ZIP_METHOD_STORE 0
#define ZIP_METHOD_DEFLATE 8
#define ZIP_FLAG_ENCRYPTED 0x0001
#define ZIP_FLAG_DATA_DESCRIPTOR 0x0008
#define ZIP_SIG_LOCAL_FILE 0x04034b50U
#define ZIP_SIG_CENTRAL_DIR 0x02014b50U
#define ZIP_SIG_END_CENTRAL_DIR 0x06054b50U
#define ZIP_SIG_ZIP64_END_CENTRAL_DIR 0x06064b50U
#define ZIP_SIG_ZIP64_LOCATOR 0x07064b50U
#define ZIP_SIG_DATA_DESCRIPTOR 0x08074b50U

typedef struct zip_pipe {
  pthread_mutex_t lock;
  pthread_cond_t  can_read;
  pthread_cond_t  can_write;
  unsigned char  *buf;
  size_t          cap;
  size_t          read_pos;
  size_t          write_pos;
  size_t          used;
  int             done;
  int             cancel;
  char            err[200];
} zip_pipe_t;

long zip_job_long(uint64_t value);

int zip_pipe_init(zip_pipe_t *pipe, size_t cap);
void zip_pipe_destroy(zip_pipe_t *pipe);
void zip_pipe_cancel(zip_pipe_t *pipe);
ssize_t zip_pipe_read(zip_pipe_t *pipe, unsigned char *out, size_t size);
void zip_pipe_copy_error(zip_pipe_t *pipe, char *out, size_t out_size);

void archive_upload_set_active(zip_pipe_t *pipe, int fd);
void archive_upload_clear_active(zip_pipe_t *pipe, int fd);
void archive_upload_cancel_active(void);

int zip_start_upload_producer(zip_pipe_t *pipe, int fd,
                              const char *initial_data, size_t initial_size,
                              size_t content_size, pthread_t *thread);
int zip_start_file_producer(zip_pipe_t *pipe, int fd, uint64_t size,
                            pthread_t *thread);

int extract_local_zip(const http_request_t *req, const char *archive_path,
                      const char *dest, const struct stat *st);
