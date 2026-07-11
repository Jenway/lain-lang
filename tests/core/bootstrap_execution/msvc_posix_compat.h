#ifndef LAIN_TEST_MSVC_POSIX_COMPAT_H
#define LAIN_TEST_MSVC_POSIX_COMPAT_H

#if defined(_WIN32)
#include <stdlib.h>
#include <string.h>

static char *lain_test_strndup(const char *source, size_t length) {
  char *copy = (char *)malloc(length + 1);
  if (!copy)
    return NULL;
  memcpy(copy, source, length);
  copy[length] = '\0';
  return copy;
}

#define strndup lain_test_strndup
#endif

#endif
