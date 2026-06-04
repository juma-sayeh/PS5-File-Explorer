/*
 * BFpilot - RAR request internals.
 */

#pragma once

#include "websrv.h"

#define RAR_UPLOAD_MAGIC "BFPRAR01"
#define RAR_PASSWORD_MAX 512
#define RAR_VOLUME_MAX 128
#define RAR_DICT_LIMIT (64ULL * 1024ULL * 1024ULL)
#define RAR_LOG_PATH "/data/BFpilot/rar.log"

int extract_archive_handler(const http_request_t *req);
