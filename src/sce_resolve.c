/*
 * File Explorer - optional Sony module symbol resolution.
 */

#include <stddef.h>
#include <stdint.h>

#include <ps5/kernel.h>

#include "sce_resolve.h"


int
sce_resolve_symbol(const char *module, const char *symbol, void **out) {
  uint32_t handle = 0;
  void *addr = NULL;

  if(out) *out = NULL;
  if(!module || !symbol || !out) return -1;

  int rc = kernel_dynlib_handle(-1, module, &handle);
  if(rc != 0) return rc;

  addr = (void *)kernel_dynlib_resolve(-1, handle, symbol);
  if(!addr) {
    addr = (void *)kernel_dynlib_dlsym(-1, handle, symbol);
  }
  if(!addr) return -2;

  *out = addr;
  return 0;
}
