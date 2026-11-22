#include "memory_metrics.h"

#include <stdio.h>
#include <sys/sysinfo.h>

#define GIB (1024.0 * 1024.0 * 1024.0)

// MemAvailable includes reclaimable cache; MemFree alone understates usable RAM.
void read_memory(double *ram_total, double *ram_free, double *swap_total,
                 double *swap_free) {
  unsigned long long mt = 0, ma = 0, st = 0, sf = 0;
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
