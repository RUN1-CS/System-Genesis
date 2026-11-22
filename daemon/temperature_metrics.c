#define _GNU_SOURCE
#include "temperature_metrics.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TEMPERATURES 64

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

    char directory[512], name_path[1024], name[128] = "";
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

      char input_path[1024], label_path[1024], label[128] = "";
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
        snprintf(sample->label, sizeof sample->label, "%.55s: %.68s", name,
                 label);
      else if (label[0])
        snprintf(sample->label, sizeof sample->label, "%.127s", label);
      else if (name[0])
        snprintf(sample->label, sizeof sample->label, "%.80s temperature %ld",
                 name, sensor_id);
      else
        snprintf(sample->label, sizeof sample->label, "%.80s temperature %ld",
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

    char directory[512], temp_path[1024], type_path[1024];
    snprintf(directory, sizeof directory, "/sys/class/thermal/%s",
             zone->d_name);
    snprintf(temp_path, sizeof temp_path, "%s/temp", directory);
    snprintf(type_path, sizeof type_path, "%s/type", directory);
    double celsius;
    if (!read_temperature_file(temp_path, &celsius))
      continue;

    TemperatureSample *sample = &samples[count++];
    if (!read_text_file(type_path, sample->label, sizeof sample->label))
      snprintf(sample->label, sizeof sample->label, "%.127s", zone->d_name);
    sample->celsius = celsius;
  }
  closedir(zones);
  return count;
}

void append_temperatures_json(JsonBuf *j) {
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
