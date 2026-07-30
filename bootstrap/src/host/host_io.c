#include "host_io.h"

#include <stdlib.h>

unsigned char *lainir_host_read_file(
    const char *path,
    size_t *length_out) {
  FILE *file;
  unsigned char *data;
  long length;
  size_t actual;

  if (!path || !length_out)
    return NULL;
  file = fopen(path, "rb");
  if (!file)
    return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  length = ftell(file);
  if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  data = malloc((size_t)length + 1);
  if (!data) {
    fclose(file);
    return NULL;
  }
  actual = fread(data, 1, (size_t)length, file);
  fclose(file);
  if (actual != (size_t)length) {
    free(data);
    return NULL;
  }
  data[actual] = 0;
  *length_out = actual;
  return data;
}

static int write_file(
    void *context,
    const char *data,
    size_t length) {
  FILE *file = context;
  return file && fwrite(data, 1, length, file) == length;
}

LainirWriter lainir_host_file_writer(FILE *file) {
  LainirWriter writer = {
      .context = file,
      .write = write_file,
  };
  return writer;
}
