#define _GNU_SOURCE
#include "disk_metrics.h"

#include <mntent.h>
#include <stdio.h>
#include <string.h>
#include <sys/vfs.h>

#define MAX_DISKS 64
#define GIB (1024.0 * 1024.0 * 1024.0)

// Report each block device once, even when it has multiple mounts.
void append_disks_json(JsonBuf *j) {
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
