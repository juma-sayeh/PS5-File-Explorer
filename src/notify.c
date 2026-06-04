/*
 * File Explorer - PS5 notification helpers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "diag.h"
#include "notify.h"

typedef struct notify_request {
  char unused[45];
  char message[3075];
} notify_request_t;


int sceKernelSendNotificationRequest(int, notify_request_t *req, size_t size,
                                     int flags) __attribute__((weak));


static int
notify_debug(const char *message, const char *submessage) {
  notify_request_t req;

  if(!sceKernelSendNotificationRequest) return -2;

  memset(&req, 0, sizeof(req));

  if(submessage && submessage[0]) {
    snprintf(req.message, sizeof(req.message), "%s\n%s", message, submessage);
  } else {
    snprintf(req.message, sizeof(req.message), "%s", message);
  }

  return sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}


int
bfpilot_notify_send(const char *message, const char *submessage) {
  const char *msg = message ? message : "File Explorer";
  const char *sub = submessage ? submessage : "";
  const char *disabled = getenv("BFPILOT_NO_NOTIFY");

  if(disabled && (!strcmp(disabled, "1") || !strcasecmp(disabled, "true") ||
                  !strcasecmp(disabled, "yes"))) {
    return BFPILOT_DIAG_SKIPPED;
  }
  return notify_debug(msg, sub);
}


int
bfpilot_notify_test(void) {
  return bfpilot_notify_send("File Explorer diagnostics", "notification test");
}


void
bfpilot_notify(const char *message, const char *submessage) {
  (void)bfpilot_notify_send(message, submessage);
}
