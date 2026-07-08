#include "mongoose-src/mongoose.h"
#include <mntent.h>
#include <stdio.h> // Core functions
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#define REMOTE_WS_PORT                                                         \
  "ws://localhost:3000"  // Remote WebSocket host address and port
#define INTERVAL_MS 5000 // Log interval
#define MAX_CORES 128    // Max CPU cores

// Struct for CPU Stats
typedef struct {
  long double u, n, s, i; // user, nice, system, idle
} CPUStats;

static struct mg_mgr mgr;                        // Mongoose manager
static struct mg_connection *client_conn = NULL; // Tracking
static int ws_connecting = 0;
static int num_cores = 0;
static CPUStats prev_cores[MAX_CORES];

// Detects core count and initial state
void init_cpu_cores() {
  // Read /proc/stat to determine the number of CPU cores and their initial
  // stats
  FILE *fp = fopen("/proc/stat", "r");
  if (!fp)
    return;
  char line[256];
  // Read lines until we find CPU core lines
  while (fgets(line, sizeof(line), fp)) {
    if (strncmp(line, "cpu", 3) == 0 && line[3] != ' ') {
      int core_id;
      sscanf(line, "cpu%d %Lf %Lf %Lf %Lf", &core_id, &prev_cores[num_cores].u,
             &prev_cores[num_cores].n, &prev_cores[num_cores].s,
             &prev_cores[num_cores].i);
      num_cores++;
      if (num_cores >= MAX_CORES)
        break;
    }
  }
  fclose(fp);
}

// Appends CPU core usage percentages to the JSON buffer
int append_cpu_cores_json(char *buffer, size_t max_len, int offset) {
  // Read /proc/stat again to get current CPU stats and calculate usage
  // percentages
  FILE *fp = fopen("/proc/stat", "r");
  if (!fp)
    return offset;

  // Append CPU core usage percentages to the JSON buffer
  offset += snprintf(buffer + offset, max_len - offset, ", 'cpu_cores': [");
  char line[256];
  int current_core = 0;

  // Read lines until we find CPU core lines
  while (fgets(line, sizeof(line), fp) && current_core < num_cores) {
    if (strncmp(line, "cpu", 3) == 0 && line[3] != ' ') {
      long double u, n, s, i;
      int core_id;
      sscanf(line, "cpu%d %Lf %Lf %Lf %Lf", &core_id, &u, &n, &s, &i);

      // Calculate CPU usage percentage based on the difference from the
      // previous reading
      long double total_prev =
          prev_cores[current_core].u + prev_cores[current_core].n +
          prev_cores[current_core].s + prev_cores[current_core].i;
      long double total_now = u + n + s + i;
      long double idled = i - prev_cores[current_core].i;
      long double totald = total_now - total_prev;

      double pct = (totald == 0) ? 0.0 : (totald - idled) / totald * 100.0;

      // Cache current state for the NEXT timer invocation block
      prev_cores[current_core].u = u;
      prev_cores[current_core].n = n;
      prev_cores[current_core].s = s;
      prev_cores[current_core].i = i;

      offset += snprintf(buffer + offset, max_len - offset, "%s%.1f",
                         (current_core > 0) ? ", " : "", pct);
      current_core++;
    }
  }
  fclose(fp);
  // Append closing bracket for the CPU cores array
  offset += snprintf(buffer + offset, max_len - offset, "]");
  return offset;
}

// Generates a JSON report of system metrics and writes it to the provided
// buffer
void generate_json_report(char *buffer, size_t max_len) {
  // Gather system metrics
  struct sysinfo mem_info;
  struct statfs disk_info;
  struct mntent *ent;
  FILE *mounts_fp;

  // Gather RAM and Swap metrics
  double total_ram_gb = 0.0, free_ram_gb = 0.0;
  double total_swap_gb = 0.0, free_swap_gb = 0.0;

  // Use sysinfo to get memory information
  if (sysinfo(&mem_info) == 0) {
    long long mem_unit = mem_info.mem_unit;
    long long total_ram = mem_info.totalram * mem_unit;
    long long free_ram = mem_info.freeram * mem_unit;
    long long total_swap = mem_info.totalswap * mem_unit;
    long long free_swap = mem_info.freeswap * mem_unit;

    // Convert bytes to GiB using double precision division by 1024^3
    total_ram_gb = (double)total_ram / (1024 * 1024 * 1024);
    free_ram_gb = (double)free_ram / (1024 * 1024 * 1024);

    // Convert swap values to GiB only if total_swap is greater than 0 to avoid
    // division by zero
    if (total_swap > 0) {
      total_swap_gb = (double)total_swap / (1024 * 1024 * 1024);
      free_swap_gb = (double)free_swap / (1024 * 1024 * 1024);
    }
  }

  // Gather timestamp and hostname
  time_t now = time(NULL);
  char *timestamp = ctime(&now);
  timestamp[strlen(timestamp) - 1] = '\0';
  char hostname[1024];
  hostname[1023] = '\0';
  gethostname(hostname, 1023);

  // Base JSON elements
  int offset = snprintf(
      buffer, max_len,
      "{ 'hostname': '%s', 'time':'%s', 'ram': {'free': %.2f, 'total': %.2f}, "
      "'swap': {'free': %.2f, 'total': %.2f}",
      hostname, timestamp, free_ram_gb, total_ram_gb, free_swap_gb,
      total_swap_gb);

  // Append CPU metrics per core
  offset = append_cpu_cores_json(buffer, max_len, offset);

  offset += snprintf(buffer + offset, max_len - offset, ", 'disk_metrics': {");

  // Dynamic disk configuration scans
  // TODO: btrfs support
  mounts_fp = setmntent("/proc/mounts", "r");
  if (mounts_fp) {
    int first_disk = 1;
    while ((ent = getmntent(mounts_fp)) != NULL) {
      if (strncmp(ent->mnt_fsname, "/dev/", 5) == 0) {
        if (statfs(ent->mnt_dir, &disk_info) == 0) {
          unsigned long long total_blocks = disk_info.f_blocks;
          unsigned long long free_blocks = disk_info.f_bfree;
          // f_bavail is the blocks actually available to a non-root user
          unsigned long long avail_blocks = disk_info.f_bavail;

          // Calculate GiB values using double precision division by 1024^3
          double disk_free_gib =
              (double)(free_blocks * disk_info.f_bsize) / (1024 * 1024 * 1024);
          double disk_total_gib =
              (double)(total_blocks * disk_info.f_bsize) / (1024 * 1024 * 1024);

          // Calculate used space based on user-accessible blocks
          unsigned long long usable_total =
              total_blocks - (free_blocks - avail_blocks);
          unsigned long long used_blocks = usable_total - avail_blocks;

          char disk_chunk[256];
          // Added 'label' to the snprintf layout matching ent->mnt_dir
          snprintf(disk_chunk, sizeof(disk_chunk),
                   "'%s': {'label': '%s', 'free': %.2f, 'total': %.2f }",
                   ent->mnt_fsname, ent->mnt_dir, disk_free_gib,
                   disk_total_gib);

          if (offset + strlen(disk_chunk) + 2 < max_len - 5) {
            const char *sep = first_disk ? "" : ", ";
            offset += snprintf(buffer + offset, max_len - offset, "%s%s", sep,
                               disk_chunk);
            first_disk = 0;
          }
        }
      }
    }
    // Close the mounts file pointer after processing
    endmntent(mounts_fp);
  }
  // Append closing brackets for disk_metrics and the main JSON object
  strcat(buffer, " } }\n");
}

// Event handler for WebSocket connection events
static void fn(struct mg_connection *c, int ev, void *ev_data) {
  // Ignore the ev_data for now, as we don't need it in this context
  (void)ev_data;

  // Handle WebSocket events
  if (ev == MG_EV_WS_OPEN) {
    printf("WebSocket Handshake Complete! Channel open.\n");
    client_conn = c;
  } else if (ev == MG_EV_CLOSE ||
             ev == MG_EV_ERROR) { // Handle connection closure or error
    if (c == client_conn) {
      printf("Active connection dropped or failed.\n");
      client_conn = NULL;
    }
  }
}

// Timer function that checks the WebSocket connection and sends metrics
static void timer_fn(void *param) {
  // Check if we have a real, fully upgraded WebSocket client link
  if (client_conn == NULL || !client_conn->is_websocket) {
    // Safe check: clear tracking if it's dead or stuck in partial HTTP state
    client_conn = NULL;

    printf("No active pipeline. Connecting to %s...\n", REMOTE_WS_PORT);

    // Let Mongoose allocate the outbound upgrade tracking structure
    // We do NOT assign client_conn here; we wait for MG_EV_WS_OPEN
    mg_ws_connect(&mgr, REMOTE_WS_PORT, fn, NULL, NULL);
  } else {
    // Connection is live and authenticated! Send the metric frame.
    char json_buffer[4096] = {0};
    generate_json_report(json_buffer, sizeof(json_buffer));

    printf("Sending metric update packet...\n");
    mg_ws_send(client_conn, json_buffer, strlen(json_buffer),
               WEBSOCKET_OP_TEXT);
  }
  (void)param;
}

// Main function: daemonizes the process, initializes Mongoose, and enters the
// event loop
int main() {
  init_cpu_cores(); // Seed initial values to parse differentials accurately

  // Daemonize the process
  pid_t pid = fork();
  if (pid < 0)
    exit(EXIT_FAILURE);
  if (pid > 0)
    exit(EXIT_SUCCESS);
  if (setsid() < 0)
    exit(EXIT_FAILURE);
  if ((chdir("/")) < 0)
    exit(EXIT_FAILURE);
  umask(0);
  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);

  mg_mgr_init(&mgr);

  mg_timer_add(&mgr, INTERVAL_MS, MG_TIMER_REPEAT, timer_fn, NULL);

  // Enter the event loop, polling for events every 100 ms
  while (1) {
    mg_mgr_poll(&mgr, 100);
  }

  mg_mgr_free(&mgr);
  return 0;
}
