/*
 * File Explorer - PS5 notification helpers.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "diag.h"
#include "notify.h"
#include "version.h"

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE

typedef struct notify_request {
  char unused[45];
  char message[3075];
} notify_request_t;


#if BFPILOT_ENABLE_LAUNCHER
int sceNotificationSend(int userId, bool isLogged, const char *payload);
#endif
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


static int
notify_toast(const char *message, const char *submessage) {
#if BFPILOT_ENABLE_LAUNCHER
  char payload[4096];

  snprintf(payload, sizeof(payload),
           "{"
           "\"rawData\":{"
           "\"viewTemplateType\":\"InteractiveToastTemplateB\","
           "\"channelType\":\"Downloads\","
           "\"useCaseId\":\"IDC\","
           "\"toastOverwriteType\":\"No\","
           "\"isImmediate\":true,"
           "\"priority\":100,"
           "\"viewData\":{"
           "\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
           "\"message\":{\"body\":\"%s\"},"
           "\"subMessage\":{\"body\":\"%s\"}"
           "},"
           "\"platformViews\":{"
           "\"previewDisabled\":{"
           "\"viewData\":{"
           "\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
           "\"message\":{\"body\":\"%s\"}"
           "}"
           "}"
           "}"
           "},"
           "\"localNotificationId\":\"5905\""
           "}",
           message, submessage, message);

  return sceNotificationSend(SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM, true,
                             payload);
#else
  (void)message;
  (void)submessage;
  return BFPILOT_DIAG_SKIPPED;
#endif
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

  int toast_rc = notify_toast(msg, sub);
  if(toast_rc == 0) return 0;

  int debug_rc = notify_debug(msg, sub);
  return toast_rc != BFPILOT_DIAG_SKIPPED ? toast_rc : debug_rc;
}


int
bfpilot_notify_test(void) {
  return notify_toast("File Explorer starting", "Preparing PS5 web UI");
}


void
bfpilot_notify(const char *message, const char *submessage) {
  (void)bfpilot_notify_send(message, submessage);
}
