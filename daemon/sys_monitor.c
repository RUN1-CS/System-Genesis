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
#include <dirent.h>
#include <fcntl.h>
#include <mntent.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_WS_URL "ws://localhost:3000"
#define DEFAULT_INTERVAL_MS 5000
#define MIN_INTERVAL_MS 500
#define CONNECT_TIMEOUT_MS 10000
#define MAX_SEND_BACKLOG (64 * 1024) // skip a sample if this much is unsent
#define MAX_CORES 128
#define MAX_DISKS 64
#define MAX_TEMPERATURES 64
#define REPORT_BUF_SIZE 16384
#define GIB (1024.0 * 1024.0 * 1024.0)

#ifndef MG_TIMER_RUN_NOW
#define MG_TIMER_RUN_NOW 0
#endif

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
  unsigned long long busy, total;
} CpuSample;

static struct mg_mgr mgr;
static struct mg_connection *conn = NULL; // current (possibly connecting) link
static int ws_open = 0;                   // handshake finished?
static uint64_t connect_started = 0;
static const char *ws_url = DEFAULT_WS_URL;
static int foreground = 0;
static volatile sig_atomic_t running = 1;

static int num_cores = 0;
static CpuSample prev_cores[MAX_CORES];

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
/* Bounds-checked JSON builder                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
  char *buf;
  size_t cap, len;
  int truncated;
} JsonBuf;

static void jb_printf(JsonBuf *j, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void jb_printf(JsonBuf *j, const char *fmt, ...) {
  if (j->truncated)
    return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(j->buf + j->len, j->cap - j->len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t) n >= j->cap - j->len) {
    j->truncated = 1;
    j->buf[j->len] = '\0';
    return;
  }
  j->len += (size_t) n;
}

// Appends s as a quoted, escaped JSON string.
static void jb_string(JsonBuf *j, const char *s) {
  jb_printf(j, "\"");
  for (; *s; s++) {
    unsigned char c = (unsigned char) *s;
    switch (c) {
    case '"':
      jb_printf(j, "\\\"");
      break;
    case '\\':
      jb_printf(j, "\\\\");
      break;
    case '\n':
      jb_printf(j, "\\n");
      break;
    case '\r':
      jb_printf(j, "\\r");
      break;
    case '\t':
      jb_printf(j, "\\t");
      break;
    default:
      if (c < 0x20)
        jb_printf(j, "\\u%04x", c);
      else
        jb_printf(j, "%c", c);
    }
  }
  jb_printf(j, "\"");
}

/* ------------------------------------------------------------------------ */
/* CPU                                                                      */
/* ------------------------------------------------------------------------ */

// Parses a "cpuN user nice system idle iowait irq softirq steal ..." line.
// Returns 1 for a per-core line (and fills *out), 0 otherwise.
static int parse_core_line(const char *line, CpuSample *out) {
  if (strncmp(line, "cpu", 3) != 0 || line[3] < '0' || line[3] > '9')
    return 0;

  char *end;
  strtol(line + 3, &end, 10); // skip the core id

  unsigned long long v[8] = {0};
  int n = sscanf(end, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1],
                 &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
  if (n < 4)
    return 0;

  unsigned long long total = 0;
  for (int i = 0; i < 8; i++)
    total += v[i]; // guest times are already included in user/nice
  unsigned long long idle = v[3] + v[4]; // idle + iowait
  out->total = total;
  out->busy = total - idle;
  return 1;
}

static void init_cpu_cores(void) {
  FILE *fp = fopen("/proc/stat", "r");
  if (!fp)
    return;
  char line[512];
  while (num_cores < MAX_CORES && fgets(line, sizeof line, fp)) {
    if (parse_core_line(line, &prev_cores[num_cores]))
      num_cores++;
  }
  fclose(fp);
}

// Appends "cpu_cores":[pct, pct, ...] using the delta since the last call.
static void append_cpu_cores_json(JsonBuf *j) {
  jb_printf(j, ",\"cpu_cores\":[");
  FILE *fp = fopen("/proc/stat", "r");
  if (fp) {
    char line[512];
    int core = 0;
    while (core < num_cores && fgets(line, sizeof line, fp)) {
      CpuSample now;
      if (!parse_core_line(line, &now))
        continue;
      unsigned long long dtotal = now.total - prev_cores[core].total;
      unsigned long long dbusy = now.busy - prev_cores[core].busy;
      double pct = dtotal ? (double) dbusy * 100.0 / (double) dtotal : 0.0;
      if (pct < 0.0)
        pct = 0.0;
      if (pct > 100.0)
        pct = 100.0;
      prev_cores[core] = now;
      jb_printf(j, "%s%.1f", core ? "," : "", pct);
      core++;
    }
    fclose(fp);
  }
  jb_printf(j, "]");
}

/* ------------------------------------------------------------------------ */
/* Memory                                                                   */
/* ------------------------------------------------------------------------ */

// "free" RAM is MemAvailable (what applications can actually use, including
// reclaimable cache), not the misleadingly small MemFree.
static void read_memory(double *ram_total, double *ram_free, double *swap_total,
                        double *swap_free) {
  unsigned long long mt = 0, ma = 0, st = 0, sf = 0; // all in kB
  int have_avail = 0;

  FILE *fp = fopen("/proc/meminfo", "r");
  if (fp) {
    char line[128];
    unsigned long long v;
    while (fgets(line, sizeof line, fp)) {
      if (sscanf(line, "MemTotal: %llu", &v) == 1)
        mt = v;
      else if (sscanf(line, "MemAvailable: %llu", &v) == 1) {
        ma = v;
        have_avail = 1;
      } else if (sscanf(line, "SwapTotal: %llu", &v) == 1)
        st = v;
      else if (sscanf(line, "SwapFree: %llu", &v) == 1)
        sf = v;
    }
    fclose(fp);
  }

  if (mt == 0 || !have_avail) { // very old kernel or no procfs: fall back
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
      unsigned long long unit = si.mem_unit ? si.mem_unit : 1;
      mt = (unsigned long long) si.totalram * unit / 1024;
      ma = ((unsigned long long) si.freeram + si.bufferram) * unit / 1024;
      st = (unsigned long long) si.totalswap * unit / 1024;
      sf = (unsigned long long) si.freeswap * unit / 1024;
    }
  }

  *ram_total = (double) mt * 1024.0 / GIB;
  *ram_free = (double) ma * 1024.0 / GIB;
  *swap_total = (double) st * 1024.0 / GIB;
  *swap_free = (double) sf * 1024.0 / GIB;
}

/* ------------------------------------------------------------------------ */
/* Disks                                                                    */
/* ------------------------------------------------------------------------ */

// One entry per block device (bind mounts and btrfs subvolumes of the same
// device are reported once). "free" is the space available to normal users.
static void append_disks_json(JsonBuf *j) {
  static char seen[MAX_DISKS][256];
  int nseen = 0, first = 1;

  jb_printf(j, "{");
  FILE *fp = setmntent("/proc/mounts", "r");
  if (fp) {
    struct mntent *ent;
    while (nseen < MAX_DISKS && (ent = getmntent(fp)) != NULL) {
      if (strncmp(ent->mnt_fsname, "/dev/", 5) != 0 ||
          strncmp(ent->mnt_fsname, "/dev/loop", 9) == 0)
        continue;

      int dup = 0;
      for (int i = 0; i < nseen && !dup; i++)
        dup = strncmp(seen[i], ent->mnt_fsname, sizeof seen[i] - 1) == 0;
      if (dup)
        continue;

      struct statfs st;
      if (statfs(ent->mnt_dir, &st) != 0 || st.f_blocks == 0)
        continue;

      snprintf(seen[nseen++], sizeof seen[0], "%s", ent->mnt_fsname);

      double unit = (double) (st.f_frsize ? st.f_frsize : st.f_bsize);
      double total = (double) st.f_blocks * unit / GIB;
      double avail = (double) st.f_bavail * unit / GIB;

      if (!first)
        jb_printf(j, ",");
      first = 0;
      jb_string(j, ent->mnt_fsname);
      jb_printf(j, ":{\"label\":");
      jb_string(j, ent->mnt_dir);
      jb_printf(j, ",\"free\":%.2f,\"total\":%.2f}", avail, total);
    }
    endmntent(fp);
  }
  jb_printf(j, "}");
}

/* ------------------------------------------------------------------------ */
/* Temperature                                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
  char label[128];
  double celsius;
} TemperatureSample;

static int read_text_file(const char *path, char *value, size_t cap) {
  FILE *fp = fopen(path, "r");
  if (!fp)
    return 0;
  int ok = fgets(value, (int) cap, fp) != NULL;
  fclose(fp);
  if (!ok)
    return 0;
  value[strcspn(value, "\r\n")] = '\0';
  return value[0] != '\0';
}

static int read_temperature_file(const char *path, double *celsius) {
  FILE *fp = fopen(path, "r");
  if (!fp)
    return 0;
  long long milli_celsius;
  int ok = fscanf(fp, "%lld", &milli_celsius) == 1;
  fclose(fp);
  if (!ok)
    return 0;
  *celsius = (double) milli_celsius / 1000.0;
  return 1;
}

static int read_hwmon_temperatures(TemperatureSample *samples) {
  DIR *hwmons = opendir("/sys/class/hwmon");
  if (!hwmons)
    return 0;

  int count = 0;
  struct dirent *hwmon;
  while (count < MAX_TEMPERATURES && (hwmon = readdir(hwmons)) != NULL) {
    if (strncmp(hwmon->d_name, "hwmon", 5) != 0)
      continue;

    char directory[512], name_path[512], name[128] = "";
    snprintf(directory, sizeof directory, "/sys/class/hwmon/%s", hwmon->d_name);
    snprintf(name_path, sizeof name_path, "%s/name", directory);
    read_text_file(name_path, name, sizeof name);

    DIR *sensor_dir = opendir(directory);
    if (!sensor_dir)
      continue;

    struct dirent *entry;
    while (count < MAX_TEMPERATURES &&
           (entry = readdir(sensor_dir)) != NULL) {
      if (strncmp(entry->d_name, "temp", 4) != 0)
        continue;
      char *end;
      long sensor_id = strtol(entry->d_name + 4, &end, 10);
      if (end == entry->d_name + 4 || sensor_id <= 0 ||
          strcmp(end, "_input") != 0)
        continue;

      char input_path[512], label_path[512], label[128] = "";
      snprintf(input_path, sizeof input_path, "%s/%s", directory,
               entry->d_name);
      snprintf(label_path, sizeof label_path, "%s/%.*s_label", directory,
               (int) (end - entry->d_name), entry->d_name);
      double celsius;
      if (!read_temperature_file(input_path, &celsius))
        continue;
      read_text_file(label_path, label, sizeof label);

      TemperatureSample *sample = &samples[count++];
      if (name[0] && label[0])
        snprintf(sample->label, sizeof sample->label, "%s: %s", name, label);
      else if (label[0])
        snprintf(sample->label, sizeof sample->label, "%s", label);
      else if (name[0])
        snprintf(sample->label, sizeof sample->label, "%s temperature %ld",
                 name, sensor_id);
      else
        snprintf(sample->label, sizeof sample->label, "%s temperature %ld",
                 hwmon->d_name, sensor_id);
      sample->celsius = celsius;
    }
    closedir(sensor_dir);
  }
  closedir(hwmons);
  return count;
}

static int read_thermal_zone_temperatures(TemperatureSample *samples) {
  DIR *zones = opendir("/sys/class/thermal");
  if (!zones)
    return 0;

  int count = 0;
  struct dirent *zone;
  while (count < MAX_TEMPERATURES && (zone = readdir(zones)) != NULL) {
    if (strncmp(zone->d_name, "thermal_zone", 12) != 0 ||
        zone->d_name[12] < '0' || zone->d_name[12] > '9')
      continue;

    char directory[512], temp_path[512], type_path[512];
    snprintf(directory, sizeof directory, "/sys/class/thermal/%s",
             zone->d_name);
    snprintf(temp_path, sizeof temp_path, "%s/temp", directory);
    snprintf(type_path, sizeof type_path, "%s/type", directory);
    double celsius;
    if (!read_temperature_file(temp_path, &celsius))
      continue;

    TemperatureSample *sample = &samples[count++];
    if (!read_text_file(type_path, sample->label, sizeof sample->label))
      snprintf(sample->label, sizeof sample->label, "%s", zone->d_name);
    sample->celsius = celsius;
  }
  closedir(zones);
  return count;
}

static void append_temperatures_json(JsonBuf *j) {
  TemperatureSample samples[MAX_TEMPERATURES];
  int count = read_hwmon_temperatures(samples);
  if (count == 0)
    count = read_thermal_zone_temperatures(samples);

  jb_printf(j, ",\"temperature_metrics\":[");
  for (int i = 0; i < count; i++) {
    if (i)
      jb_printf(j, ",");
    jb_printf(j, "{\"label\":");
    jb_string(j, samples[i].label);
    jb_printf(j, ",\"celsius\":%.1f}", samples[i].celsius);
  }
  jb_printf(j, "]");
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
          interval_ms, num_cores);

  while (running)
    mg_mgr_poll(&mgr, 100);

  log_msg(LOG_INFO, "shutting down");
  mg_mgr_free(&mgr);
  return EXIT_SUCCESS;
}