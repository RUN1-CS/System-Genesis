#include "cpu_metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CORES 128

typedef struct {
  unsigned long long busy, total;
} CpuSample;

static int num_cores;
static CpuSample prev_cores[MAX_CORES];

// Parses a "cpuN user nice system idle iowait irq softirq steal ..." line.
static int parse_core_line(const char *line, CpuSample *out) {
  if (strncmp(line, "cpu", 3) != 0 || line[3] < '0' || line[3] > '9')
    return 0;

  char *end;
  strtol(line + 3, &end, 10);

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

void init_cpu_cores(void) {
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

int cpu_core_count(void) { return num_cores; }

void append_cpu_cores_json(JsonBuf *j) {
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
