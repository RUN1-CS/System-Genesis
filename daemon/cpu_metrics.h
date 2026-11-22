#ifndef CPU_METRICS_H
#define CPU_METRICS_H

#include "json_builder.h"

void init_cpu_cores(void);
int cpu_core_count(void);
void append_cpu_cores_json(JsonBuf *j);

#endif
