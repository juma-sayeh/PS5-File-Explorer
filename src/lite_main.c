/*
 * File Explorer - minimal PS5 browser file manager payload.
 *
 * Runtime surface is intentionally small: one HTTP file-manager server,
 * startup notification, and no companion side services.
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include "diag.h"
#include "notify.h"
#include "version.h"
#include "websrv.h"

#if BFPILOT_ENABLE_LAUNCHER
#include "app_installer.h"
#include "sce_resolve.h"
#endif

#define BFPILOT_WEB_PORT 5905
#define BFPILOT_RELOAD_TOKEN "bs5fm-local-reload"

#if BFPILOT_ENABLE_LAUNCHER
typedef int (*sce_netctl_init_fn)(void);
typedef int (*sce_user_service_initialize_fn)(void *);

#define BFPILOT_NETCTL_MODULE "libSceNetCtl.sprx"
#define BFPILOT_USER_SERVICE_MODULE "libSceUserService.sprx"
#endif


static void
detect_lan_ip(char *out, size_t out_size) {
  struct ifaddrs *ifaddr = NULL;

  snprintf(out, out_size, "<PS5_IP>");
  if(getifaddrs(&ifaddr) != 0) return;

  for(struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if(!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
    if(ifa->ifa_flags & IFF_LOOPBACK) continue;

    struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
    const char *ip = inet_ntop(AF_INET, &sa->sin_addr, out, out_size);
    if(ip && strncmp(out, "169.254.", 8) != 0) {
      freeifaddrs(ifaddr);
      return;
    }
  }

  freeifaddrs(ifaddr);
  snprintf(out, out_size, "<PS5_IP>");
}


typedef struct ready_state {
  char ip[64];
  int notified;
  int launcher_disabled;
  int launcher_started;
} ready_state_t;


#if BFPILOT_ENABLE_LAUNCHER
static void
init_ps5_services(void) {
  int user_prio = 256;
  int netctl_rc = BFPILOT_DIAG_SKIPPED;
  int user_service_rc = BFPILOT_DIAG_SKIPPED;
  sce_netctl_init_fn sce_netctl_init = NULL;
  sce_user_service_initialize_fn sce_user_service_initialize = NULL;

  int resolve_rc = sce_resolve_symbol(BFPILOT_NETCTL_MODULE,
                                      "sceNetCtlInit",
                                      (void **)&sce_netctl_init);
  bfpilot_log("sce resolve %s:sceNetCtlInit %s rc=0x%08x",
              BFPILOT_NETCTL_MODULE,
              sce_netctl_init ? "ok" : "missing", resolve_rc);
  if(sce_netctl_init) {
    netctl_rc = sce_netctl_init();
    bfpilot_log("sceNetCtlInit rc=0x%08x", netctl_rc);
  } else {
    bfpilot_log("sceNetCtlInit rc=skipped");
  }

  resolve_rc = sce_resolve_symbol(BFPILOT_USER_SERVICE_MODULE,
                                  "sceUserServiceInitialize",
                                  (void **)&sce_user_service_initialize);
  bfpilot_log("sce resolve %s:sceUserServiceInitialize %s rc=0x%08x",
              BFPILOT_USER_SERVICE_MODULE,
              sce_user_service_initialize ? "ok" : "missing", resolve_rc);
  if(sce_user_service_initialize) {
    user_service_rc = sce_user_service_initialize(&user_prio);
    bfpilot_log("sceUserServiceInitialize rc=0x%08x", user_service_rc);
  } else {
    bfpilot_log("sceUserServiceInitialize rc=skipped");
  }

  bfpilot_diag_set_service_rcs(netctl_rc, user_service_rc);
}
#endif


#if BFPILOT_ENABLE_LAUNCHER
typedef struct launcher_thread_arg {
  char ip[64];
} launcher_thread_arg_t;


static void *
launcher_thread(void *arg) {
  launcher_thread_arg_t *state = arg;
  char fallback[128];

  snprintf(fallback, sizeof(fallback), "Use http://%s:%u/",
           state && state->ip[0] ? state->ip : "<PS5_IP>",
           (unsigned int)BFPILOT_WEB_PORT);

  bfpilot_checkpoint("launcher started");
  bfpilot_diag_set_launcher_status("started", 0);
  bfpilot_log("launcher started after web server");
  init_ps5_services();

  int app_install_status = bfpilot_install_app_if_needed();
  if(app_install_status >= 0) {
    bfpilot_diag_set_launcher_status("ready", app_install_status);
    bfpilot_log("launcher ready rc=%d", app_install_status);
    puts("  ps5 app: ready");
  } else {
    bfpilot_diag_set_launcher_status("failed_nonfatal", app_install_status);
    bfpilot_log("launcher skipped rc=%d, web server remains available",
                app_install_status);
    bfpilot_notify("File Explorer launcher skipped", fallback);
    puts("  ps5 app: launcher skipped, web server remains available");
  }

  free(state);
  return NULL;
}


static void
start_launcher_thread(const char *ip) {
  launcher_thread_arg_t *state = calloc(1, sizeof(*state));
  if(!state) {
    bfpilot_diag_set_launcher_status("failed_nonfatal", -ENOMEM);
    bfpilot_log("launcher skipped: failed allocating thread state");
    bfpilot_notify("File Explorer launcher skipped", "Use http://<PS5_IP>:5905/");
    return;
  }
  snprintf(state->ip, sizeof(state->ip), "%s", ip ? ip : "<PS5_IP>");

  pthread_t thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int rc = pthread_create(&thread, &attr, launcher_thread, state);
  pthread_attr_destroy(&attr);
  if(rc != 0) {
    bfpilot_diag_set_launcher_status("failed_nonfatal", -rc);
    bfpilot_log("launcher skipped: pthread_create rc=%d", rc);
    bfpilot_notify("File Explorer launcher skipped", "Use http://<PS5_IP>:5905/");
    free(state);
  }
}
#endif


static int
launcher_disabled_requested(int argc, char **argv) {
#if !BFPILOT_ENABLE_LAUNCHER
  (void)argc;
  (void)argv;
  return 1;
#else
  const char *env_value = getenv("BFPILOT_NO_LAUNCHER");
  int env_enabled = env_value &&
                    (!strcmp(env_value, "1") ||
                     !strcasecmp(env_value, "true") ||
                     !strcasecmp(env_value, "yes") ||
                     !strcasecmp(env_value, "on"));

  if(env_enabled) {
    return 1;
  }
  for(int i = 1; i < argc; i++) {
    if(argv[i] && !strcmp(argv[i], "--no-launcher")) {
      return 1;
    }
  }
  return 0;
#endif
}


static int
local_http_get(const char *path, char *out, size_t out_size) {
  if(out_size == 0) return -1;
  out[0] = 0;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd < 0) return -1;

  struct timeval timeout;
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(BFPILOT_WEB_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }

  char request[256];
  int n = snprintf(request, sizeof(request),
                   "GET %s HTTP/1.1\r\n"
                   "Host: 127.0.0.1:%u\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   path, (unsigned int)BFPILOT_WEB_PORT);
  if(n < 0 || (size_t)n >= sizeof(request) ||
     websrv_write_all(fd, request, (size_t)n) != 0) {
    close(fd);
    return -1;
  }

  size_t used = 0;
  while(used + 1 < out_size) {
    ssize_t r = recv(fd, out + used, out_size - used - 1, 0);
    if(r < 0) {
      if(errno == EINTR) continue;
      break;
    }
    if(r == 0) break;
    used += (size_t)r;
  }
  out[used] = 0;
  close(fd);

  return used > 0 ? 0 : -1;
}


static long
json_long_field(const char *json, const char *field) {
  char needle[64];
  snprintf(needle, sizeof(needle), "\"%s\":", field);

  const char *p = strstr(json ? json : "", needle);
  if(!p) return -1;
  p += strlen(needle);
  while(*p == ' ' || *p == '\t') p++;

  char *end = NULL;
  long value = strtol(p, &end, 10);
  return end && end != p ? value : -1;
}


static int
old_server_busy(void) {
  char response[2048];
  if(local_http_get("/api/fs/job/status", response, sizeof(response)) != 0) {
    return 0;
  }
  return strstr(response, "\"busy\":true") != NULL;
}


static int
wait_for_old_server_down(void) {
  char response[512];
  for(int i = 0; i < 30; i++) {
    if(local_http_get("/api/status", response, sizeof(response)) != 0) {
      return 1;
    }
    usleep(100000);
  }
  return 0;
}


static int
handoff_existing_server(void) {
  char response[2048];
  if(local_http_get("/api/status", response, sizeof(response)) != 0) {
    return 0;
  }
  if(!strstr(response, "\"name\":\"File Explorer\"") &&
     !strstr(response, "\"name\":\"BFpilot\"") &&
     !strstr(response, "\"name\":\"BS5FileManager\"")) {
    bfpilot_log("handoff: port %u belongs to a different service; leaving it "
                "running", (unsigned int)BFPILOT_WEB_PORT);
    bfpilot_notify("File Explorer reload skipped",
                   "Port 5905 is used by another service");
    return -1;
  }

  long old_pid = json_long_field(response, "pid");
  bfpilot_log("handoff: existing File Explorer-compatible server pid=%ld",
              old_pid);
  if(old_pid == (long)getpid()) {
    return 0;
  }

  if(old_server_busy()) {
    bfpilot_notify("File Explorer reload skipped",
                   "Old file operation is still running");
    return -1;
  }

  snprintf(response, sizeof(response),
           "/api/control/shutdown?token=%s", BFPILOT_RELOAD_TOKEN);
  char shutdown_response[512];
  if(local_http_get(response, shutdown_response,
                    sizeof(shutdown_response)) == 0 &&
     strstr(shutdown_response, "200 OK") &&
     strstr(shutdown_response, "\"ok\":true") &&
     wait_for_old_server_down()) {
    bfpilot_notify("File Explorer reloaded", "Old listener stopped cleanly");
    return 1;
  }

  bfpilot_notify("File Explorer reload failed",
                 "Could not stop old listener on port 5905");
  bfpilot_log("handoff: old listener did not accept clean shutdown; not "
              "sending signals");
  return -1;
}


static void
on_web_ready(unsigned short port, void *arg) {
  ready_state_t *state = arg;
  char url[128];

  snprintf(url, sizeof(url), "Open http://%s:%u/",
           state->ip, (unsigned int)port);

  bfpilot_checkpoint("web server ready");
  bfpilot_log("web ui ready http://%s:%u/", state->ip, (unsigned int)port);

  if(!state->notified) {
    bfpilot_notify("File Explorer started", url);
    state->notified = 1;
  }

#if BFPILOT_ENABLE_LAUNCHER
  if(!state->launcher_disabled && !state->launcher_started) {
    state->launcher_started = 1;
    start_launcher_thread(state->ip);
  }
#endif
}


int
main(int argc, char **argv) {
  bfpilot_diag_init();
  bfpilot_diag_install_signal_handlers();
  bfpilot_checkpoint("process start");
  bfpilot_log("build version tag=%s build=%s mode=%s",
              VERSION_TAG, BUILD_VERSION, BFPILOT_BUILD_MODE);
  bfpilot_log("PID=%ld", (long)getpid());
  bfpilot_log("argv argc=%d", argc);
  for(int i = 0; i < argc; i++) {
    bfpilot_log("argv[%d]=%s", i, argv && argv[i] ? argv[i] : "(null)");
  }
  bfpilot_log("PS5 SDK build flags sdk_path=%s enable_launcher=%d "
              "disable_launcher=%d",
              BFPILOT_SDK_PATH, BFPILOT_ENABLE_LAUNCHER,
              BFPILOT_DISABLE_LAUNCHER);
  bfpilot_log("diagnostic paths log=/data/FileExplorer/log.txt crash=/data/FileExplorer/"
              "crash.log");

  ready_state_t ready;
  memset(&ready, 0, sizeof(ready));
  detect_lan_ip(ready.ip, sizeof(ready.ip));
  int launcher_disabled = launcher_disabled_requested(argc, argv);
  ready.launcher_disabled = launcher_disabled;
  websrv_set_runtime_diag(launcher_disabled);
  bfpilot_log("runtime launcher_disabled=%d", launcher_disabled);

  puts(".----------------------------------------------.");
  puts("|  File Explorer                               |");
  printf("|  %-18s  browser file manager        |\n", VERSION_TAG);
  puts("'----------------------------------------------'");
  puts("");
  puts("  active: standalone web file manager");
  printf("  mode: %s\n", BFPILOT_BUILD_MODE);
  puts("  scope: browse, upload, download, copy, move, delete, rename, mkdir");
  if(launcher_disabled) {
    puts("  ps5 app: launcher disabled");
  } else {
    puts("  ps5 app: File Explorer opens http://127.0.0.1:5905/");
  }
  printf("  web ui: http://%s:%u/\n", ready.ip, (unsigned int)BFPILOT_WEB_PORT);
  puts("  inject/deploy port: 9021");
  puts("");

  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);

  int notification_rc = bfpilot_notify_test();
  bfpilot_diag_set_notification_rc(notification_rc);
  bfpilot_log("notification test rc=0x%08x", notification_rc);

#if BFPILOT_ENABLE_LAUNCHER
  if(!launcher_disabled) {
    bfpilot_diag_set_launcher_status("deferred", BFPILOT_DIAG_SKIPPED);
    bfpilot_log("launcher deferred until web server is listening");
    puts("  ps5 app: launcher will run after web server starts");
  } else {
    bfpilot_diag_set_service_rcs(BFPILOT_DIAG_SKIPPED, BFPILOT_DIAG_SKIPPED);
    bfpilot_diag_set_launcher_status("skipped", BFPILOT_DIAG_SKIPPED);
    bfpilot_log("sceNetCtlInit rc=skipped");
    bfpilot_log("sceUserServiceInitialize rc=skipped");
    bfpilot_log("launcher skipped: runtime flag");
    puts("  ps5 app: skipped by runtime flag");
  }
#else
  bfpilot_diag_set_service_rcs(BFPILOT_DIAG_SKIPPED, BFPILOT_DIAG_SKIPPED);
  bfpilot_diag_set_launcher_status("not_compiled", BFPILOT_DIAG_SKIPPED);
  bfpilot_log("sceNetCtlInit rc=skipped");
  bfpilot_log("sceUserServiceInitialize rc=skipped");
  bfpilot_log("launcher skipped: not compiled");
  puts("  ps5 app: not compiled in core mode");
#endif

  bfpilot_checkpoint("handoff check");
  int handoff_status = handoff_existing_server();
  bfpilot_log("handoff_existing_server rc=%d", handoff_status);
  if(handoff_status < 0) {
    bfpilot_checkpoint("handoff blocked");
    puts("  reload: old listener still active; exiting this injection");
    return 0;
  }

  while(1) {
    bfpilot_checkpoint("web listen starting");
    int rc = websrv_listen(BFPILOT_WEB_PORT, on_web_ready, &ready);
    bfpilot_log("websrv_listen returned rc=%d", rc);
    if(websrv_exit_requested()) {
      bfpilot_checkpoint("web shutdown requested");
      puts("  web ui: shutdown requested");
      break;
    }
    if(!ready.notified) {
      char msg[128];
      snprintf(msg, sizeof(msg), "port %u error %d, retrying",
               (unsigned int)BFPILOT_WEB_PORT, -rc);
      bfpilot_notify("File Explorer could not start", msg);
      ready.notified = 1;
    }
    sleep(rc == -EADDRINUSE || rc == -EACCES ? 5 : 2);
  }

  return 0;
}
