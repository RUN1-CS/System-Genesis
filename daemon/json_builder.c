#include "json_builder.h"

#include <stdarg.h>
#include <stdio.h>

void jb_printf(JsonBuf *j, const char *fmt, ...) {
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

void jb_string(JsonBuf *j, const char *s) {
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
