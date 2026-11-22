#ifndef JSON_BUILDER_H
#define JSON_BUILDER_H

#include <stddef.h>

typedef struct {
  char *buf;
  size_t cap, len;
  int truncated;
} JsonBuf;

void jb_printf(JsonBuf *j, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void jb_string(JsonBuf *j, const char *s);

#endif
