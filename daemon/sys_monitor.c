/*
 * sys_monitor.c - lightweight system metrics agent.
 *
 * Collects RAM / swap / per-core CPU / disk usage and pushes it as a JSON text
 * frame over a WebSocket (Mongoose) at a fixed interval. Reconnects on its own.
 *
 * Usage: sys_monitor [-f] [-u ws://host:port[/?token=...]] [-i interval_ms]
 *   -f  stay in the foreground and log to stderr (default: daemonize + syslog)
 *   -u  server URL        (env SYSMON_URL, default ws://localhost:3000)
 *   -i  interval in ms    (env SYSMON_INTERVAL_MS, default 5000, min 500)
 */
#define _GNU_SOURCE
#include "mongoose-src/mongoose.h"
#include "cpu_metrics.h"
#include "disk_metrics.h"
#include "json_builder.h"
#include "memory_metrics.h"
#include "temperature_metrics.h"

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_WS_URL "ws://localhost:3000"
#define DEFAULT_INTERVAL_MS 5000
#define MIN_INTERVAL_MS 500
#define CONNECT_TIMEOUT_MS 10000
#define MAX_SEND_BACKLOG (64 * 1024) // skip a sample if this much is unsent
#define REPORT_BUF_SIZE 16384

#ifndef MG_TIMER_RUN_NOW
#define MG_TIMER_RUN_NOW 0
#endif

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

static struct mg_mgr mgr;
static struct mg_connection *conn = NULL; // current (possibly connecting) link
static int ws_open = 0;                   // handshake finished?
static uint64_t connect_started = 0;
static const char *ws_url = DEFAULT_WS_URL;
static int foreground = 0;
static volatile sig_atomic_t running = 1;

/* ------------------------------------------------------------------------ */
/* Logging (stderr in foreground, syslog when daemonized)                   */
/* ------------------------------------------------------------------------ */

static void log_msg(int prio, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void log_msg(int prio, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (foreground) {
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
  } else {
    vsyslog(prio, fmt, ap);
  }
  va_end(ap);
}

/* ------------------------------------------------------------------------ */
/* Report                                                                   */
/* ------------------------------------------------------------------------ */

// Builds the JSON report. Returns its length, or 0 if it didn't fit.
static size_t generate_json_report(char *buffer, size_t cap) {
  JsonBuf j = {buffer, cap, 0, 0};

  char hostname[256] = "unknown";
  if (gethostname(hostname, sizeof hostname - 1) != 0)
    snprintf(hostname, sizeof hostname, "unknown");
  hostname[sizeof hostname - 1] = '\0';

  char ts[32];
  time_t now = time(NULL);
  struct tm tm;
  gmtime_r(&now, &tm);
  strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm); // ISO 8601, UTC

  double ram_total, ram_free, swap_total, swap_free;
  read_memory(&ram_total, &ram_free, &swap_total, &swap_free);

  jb_printf(&j, "{\"hostname\":");
  jb_string(&j, hostname);
  jb_printf(&j, ",\"time\":\"%s\"", ts);
  jb_printf(&j, ",\"ram\":{\"free\":%.2f,\"total\":%.2f}", ram_free, ram_total);
  jb_printf(&j, ",\"swap\":{\"free\":%.2f,\"total\":%.2f}", swap_free,
            swap_total);
  append_cpu_cores_json(&j);
  jb_printf(&j, ",\"disk_metrics\":");
  append_disks_json(&j);
  append_temperatures_json(&j);
  jb_printf(&j, "}");

  return j.truncated ? 0 : j.len;
}

/* ------------------------------------------------------------------------ */
/* WebSocket client                                                         */
/* ------------------------------------------------------------------------ */

static void ws_handler(struct mg_connection *c, int ev, void *ev_data) {
  switch (ev) {
  case MG_EV_WS_OPEN:
    if (c == conn) {
      ws_open = 1;
      log_msg(LOG_INFO, "connected to %s", ws_url);
    }
    break;
  case MG_EV_ERROR:
    log_msg(LOG_WARNING, "connection error: %s", (const char *) ev_data);
    break;
  case MG_EV_CLOSE:
    if (c == conn) {
      log_msg(LOG_INFO, "connection closed, will retry");
      conn = NULL;
      ws_open = 0;
    }
    break;
  default:
    break;
  }
}

static void timer_fn(void *arg) {
  (void) arg;

  if (conn == NULL) { // (re)connect; ws_handler tracks the outcome
    ws_open = 0;
    connect_started = mg_millis();
    conn = mg_ws_connect(&mgr, ws_url, ws_handler, NULL, NULL);
    if (conn == NULL)
      log_msg(LOG_WARNING, "could not start connection to %s", ws_url);
    return;
  }

  if (!ws_open) { // handshake still pending: don't open a second connection
    if (mg_millis() - connect_started > CONNECT_TIMEOUT_MS) {
      log_msg(LOG_WARNING, "connect timeout, dropping attempt");
      conn->is_closing = 1;
    }
    return;
  }

  if (conn->send.len > MAX_SEND_BACKLOG) { // server not keeping up
    log_msg(LOG_WARNING, "send backlog too large, skipping sample");
    return;
  }

  char json[REPORT_BUF_SIZE];
  size_t len = generate_json_report(json, sizeof json);
  if (len == 0) {
    log_msg(LOG_ERR, "report did not fit in buffer, skipping");
    return;
  }
  mg_ws_send(conn, json, len, WEBSOCKET_OP_TEXT);
}

/* ------------------------------------------------------------------------ */
/* Process plumbing                                                         */
/* ------------------------------------------------------------------------ */

static void on_signal(int sig) {
  (void) sig;
  running = 0;
}

static void daemonize(void) {
  pid_t pid = fork();
  if (pid < 0)
    exit(EXIT_FAILURE);
  if (pid > 0)
    _exit(EXIT_SUCCESS);
  if (setsid() < 0)
    exit(EXIT_FAILURE);
  if (chdir("/") < 0)
    exit(EXIT_FAILURE);
  umask(027);

  // Point stdio at /dev/null so later open() calls can't land on fds 0-2.
  int fd = open("/dev/null", O_RDWR);
  if (fd >= 0) {
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
      close(fd);
  }
}

static void usage(const char *prog) {
  fprintf(stderr, "Usage: %s [-f] [-u ws://host:port] [-i interval_ms]\n",
          prog);
}

int main(int argc, char **argv) {
  int interval_ms = DEFAULT_INTERVAL_MS;

  const char *env_url = getenv("SYSMON_URL");
  const char *env_int = getenv("SYSMON_INTERVAL_MS");
  if (env_url && *env_url)
    ws_url = env_url;
  if (env_int && atoi(env_int) > 0)
    interval_ms = atoi(env_int);

  int opt;
  while ((opt = getopt(argc, argv, "fu:i:h")) != -1) {
    switch (opt) {
    case 'f':
      foreground = 1;
      break;
    case 'u':
      ws_url = optarg;
      break;
    case 'i':
      interval_ms = atoi(optarg);
      break;
    default:
      usage(argv[0]);
      return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
    }
  }
  if (interval_ms < MIN_INTERVAL_MS)
    interval_ms = MIN_INTERVAL_MS;

  if (!foreground) {
    openlog("sys_monitor", LOG_PID, LOG_DAEMON);
    daemonize();
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);

  init_cpu_cores(); // baseline so the first report has a valid delta

  mg_mgr_init(&mgr);
  mg_timer_add(&mgr, (uint64_t) interval_ms,
               MG_TIMER_REPEAT | MG_TIMER_RUN_NOW, timer_fn, NULL);

  log_msg(LOG_INFO, "started: url=%s interval=%dms cores=%d", ws_url,
          interval_ms, cpu_core_count());

  while (running)
    mg_mgr_poll(&mgr, 100);

  log_msg(LOG_INFO, "shutting down");
  mg_mgr_free(&mgr);
  return EXIT_SUCCESS;
}