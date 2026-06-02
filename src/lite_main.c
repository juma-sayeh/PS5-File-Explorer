/*
 * BFpilot - minimal PS5 browser file manager payload.
 *
 * Runtime surface is intentionally small: one HTTP file-manager server,
 * startup notification, and no companion side services.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include "app_installer.h"
#include "notify.h"
#include "version.h"
#include "websrv.h"

#define BFPILOT_WEB_PORT 5905
#define BFPILOT_RELOAD_TOKEN "bs5fm-local-reload"

int sceNetCtlInit(void);
int sceUserServiceInitialize(void *);


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
} ready_state_t;


static void
init_ps5_services(void) {
  int user_prio = 256;
  int err = sceNetCtlInit();
  printf("  service: sceNetCtlInit 0x%08x\n", err);

  err = sceUserServiceInitialize(&user_prio);
  printf("  service: sceUserServiceInitialize 0x%08x\n", err);
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
  if(!strstr(response, "\"name\":\"BFpilot\"") &&
     !strstr(response, "\"name\":\"BS5FileManager\"")) {
    return 0;
  }

  long old_pid = json_long_field(response, "pid");
  if(old_pid <= 0 || old_pid == (long)getpid()) {
    return 0;
  }

  if(old_server_busy()) {
    bfpilot_notify("BFpilot reload skipped",
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
    bfpilot_notify("BFpilot reloaded", "Old listener stopped cleanly");
    return 1;
  }

  if(kill((pid_t)old_pid, SIGTERM) == 0 && wait_for_old_server_down()) {
    bfpilot_notify("BFpilot reloaded", "Old listener stopped");
    return 1;
  }

  kill((pid_t)old_pid, SIGKILL);
  if(wait_for_old_server_down()) {
    bfpilot_notify("BFpilot reloaded", "Old listener was replaced");
    return 1;
  }

  bfpilot_notify("BFpilot reload failed",
                 "Could not stop old listener on port 5905");
  return -1;
}


static void
on_web_ready(unsigned short port, void *arg) {
  ready_state_t *state = arg;
  char url[128];

  snprintf(url, sizeof(url), "Open http://%s:%u/",
           state->ip, (unsigned int)port);

  printf("  web ui ready: http://%s:%u/\n", state->ip, (unsigned int)port);

  if(!state->notified) {
    bfpilot_notify("BFpilot started", url);
    state->notified = 1;
  }
}


int
main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  ready_state_t ready;
  memset(&ready, 0, sizeof(ready));
  detect_lan_ip(ready.ip, sizeof(ready.ip));

  puts(".----------------------------------------------.");
  puts("|  BFpilot                                     |");
  printf("|  %-18s  browser file manager        |\n", VERSION_TAG);
  puts("'----------------------------------------------'");
  puts("");
  puts("  active: standalone web file manager");
  puts("  scope: browse, upload, download, copy, move, delete, rename, mkdir");
  puts("  ps5 app: BFpilot opens http://127.0.0.1:5905/");
  printf("  web ui: http://%s:%u/\n", ready.ip, (unsigned int)BFPILOT_WEB_PORT);
  puts("  inject/deploy port: 9021");
  puts("");

  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);

  init_ps5_services();

  int app_install_status = bfpilot_install_app_if_needed();
  if(app_install_status >= 0) {
    puts("  ps5 app: ready");
  } else {
    puts("  ps5 app: install failed, continuing web server");
  }

  int handoff_status = handoff_existing_server();
  if(handoff_status < 0) {
    puts("  reload: old listener still active; exiting this injection");
    return 0;
  }

  while(1) {
    int rc = websrv_listen(BFPILOT_WEB_PORT, on_web_ready, &ready);
    if(websrv_exit_requested()) {
      puts("  web ui: shutdown requested");
      break;
    }
    if(!ready.notified) {
      char msg[128];
      snprintf(msg, sizeof(msg), "port %u error %d, retrying",
               (unsigned int)BFPILOT_WEB_PORT, -rc);
      bfpilot_notify("BFpilot could not start", msg);
      ready.notified = 1;
    }
    sleep(rc == -EADDRINUSE || rc == -EACCES ? 5 : 2);
  }

  return 0;
}
