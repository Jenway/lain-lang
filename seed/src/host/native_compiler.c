/*
 * Native host for the LAIN-IR-written compiler.
 *
 * This file owns process I/O and allocation only. Parsing, verification, and
 * C emission remain in src/lainir/compiler.l1 and in the generated C compiled
 * beside this host.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *source_bytes;
static int64_t source_size;
static const char *artifact_path;
static FILE *artifact_file;

int32_t lainir_compile(void);
int32_t lainir_compile_module(void);

int64_t bootstrap_dot_source_dash_count(void) { return 1; }

uint8_t *bootstrap_dot_source_dash_data(int64_t index) {
  return index == 0 ? source_bytes : NULL;
}

int64_t bootstrap_dot_source_dash_length(int64_t index) {
  return index == 0 ? source_size : 0;
}

uint8_t *bootstrap_dot_allocate_dash_pages(int64_t size) {
  return calloc(size > 0 ? (size_t)size : 1, 1);
}

void bootstrap_dot_release_dash_pages(uint8_t *memory) { free(memory); }

void bootstrap_dot_artifact_dash_begin(void) {
  if (artifact_file)
    return;
  artifact_file = fopen(artifact_path, "wb");
}

void bootstrap_dot_artifact_dash_write_dash_byte(int8_t byte) {
  if (artifact_file)
    fputc((unsigned char)byte, artifact_file);
}

void bootstrap_dot_artifact_dash_write_dash_literal(uint8_t *text) {
  if (artifact_file && text)
    fputs((const char *)text, artifact_file);
}

void bootstrap_dot_artifact_dash_finish(void) {
  if (!artifact_file)
    return;
  if (fclose(artifact_file) != 0)
    remove(artifact_path);
  artifact_file = NULL;
}

void bootstrap_dot_write_dash_diagnostic(uint8_t *text, int64_t length) {
  if (text && length > 0)
    fwrite(text, 1, (size_t)length, stderr);
  fputc('\n', stderr);
}

static uint8_t *read_source(const char *path, int64_t *length_out) {
  FILE *file = fopen(path, "rb");
  long length;
  uint8_t *bytes;
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
  bytes = malloc(length ? (size_t)length : 1);
  if (!bytes || (length && fread(bytes, 1, (size_t)length, file) != (size_t)length)) {
    free(bytes);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *length_out = (int64_t)length;
  return bytes;
}

int main(int argc, char **argv) {
  int32_t status;
  int module_mode = argc == 4 && strcmp(argv[1], "--module") == 0;
  const char *output;
  const char *input;
  if ((!module_mode && argc != 3) || (module_mode && argc != 4)) {
    fprintf(stderr, "usage: lainir-c [--module] <output.c> <input.l1>\n");
    return 2;
  }
  output = module_mode ? argv[2] : argv[1];
  input = module_mode ? argv[3] : argv[2];
  artifact_path = output;
  source_bytes = read_source(input, &source_size);
  if (!source_bytes) {
    fprintf(stderr, "lainir-c: cannot read %s\n", input);
    return 2;
  }
  status = module_mode ? lainir_compile_module() : lainir_compile();
  if (artifact_file) {
    fclose(artifact_file);
    artifact_file = NULL;
  }
  if (status != 0)
    remove(artifact_path);
  free(source_bytes);
  source_bytes = NULL;
  return status;
}
