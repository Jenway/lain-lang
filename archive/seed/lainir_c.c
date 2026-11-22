/* lainir-c：LAINIR -> C99 命令行编译器。
 *
 * 用法：
 *   lainir-c [--entry <proc>] [--print-result] [-o <out.c>] <unit.l1> ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lain/backend/c99.h"
#include "lain/ir/build.h"
#include "lain/ir/verify.h"
#include "lain/text/parse.h"

static char *read_and_join(int count, char **paths) {
  size_t total = 0;
  char *buf = (char *)malloc(1);
  if (!buf) return NULL;
  buf[0] = '\0';
  for (int i = 0; i < count; ++i) {
    FILE *f = fopen(paths[i], "rb");
    if (!f) {
      fprintf(stderr, "lainir-c: cannot open %s\n", paths[i]);
      free(buf);
      return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
      fclose(f);
      free(buf);
      return NULL;
    }
    char *nb = (char *)realloc(buf, total + (size_t)sz + 2);
    if (!nb) {
      fclose(f);
      free(buf);
      return NULL;
    }
    buf = nb;
    size_t nread = fread(buf + total, 1, (size_t)sz, f);
    fclose(f);
    total += nread;
    buf[total++] = '\n';
    buf[total] = '\0';
  }
  return buf;
}

int main(int argc, char **argv) {
  const char *entry = NULL;
  const char *out_path = NULL;
  bool print_result = false;
  char *inputs[256];
  int input_count = 0;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--entry") == 0 && i + 1 < argc) {
      entry = argv[++i];
    } else if (strcmp(argv[i], "--print-result") == 0) {
      print_result = true;
    } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      out_path = argv[++i];
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "lainir-c: unknown option %s\n", argv[i]);
      return 2;
    } else {
      if (input_count >= 256) {
        fprintf(stderr, "lainir-c: too many input files\n");
        return 2;
      }
      inputs[input_count++] = argv[i];
    }
  }

  if (input_count == 0) {
    fprintf(stderr,
            "usage: lainir-c [--entry <proc>] [--print-result] [-o <out.c>] "
            "<file.l1> ...\n");
    return 2;
  }

  char *source = read_and_join(input_count, inputs);
  if (!source) return 1;

  L1Builder *b = lainir_builder_new();
  if (!b) {
    free(source);
    return 1;
  }

  L1Diagnostic diag = {0};
  const L1Module *mod = lainir_parse(b, source, &diag);
  if (!mod) {
    fprintf(stderr, "lainir-c parse error (%d) at %u:%u: %s\n", diag.code,
            diag.line, diag.column, diag.message);
    lainir_builder_free(b);
    free(source);
    return 1;
  }

  if (lainir_verify(mod, &diag) != 0) {
    fprintf(stderr, "lainir-c verify error (%d) at %u:%u: %s\n", diag.code,
            diag.line, diag.column, diag.message);
    lainir_builder_free(b);
    free(source);
    return 1;
  }

  FILE *out = stdout;
  if (out_path) {
    out = fopen(out_path, "wb");
    if (!out) {
      fprintf(stderr, "lainir-c: cannot write %s\n", out_path);
      lainir_builder_free(b);
      free(source);
      return 1;
    }
  }

  LainC99Options opts = {
      .entry = entry,
      .print_result = print_result,
  };
  int rc = lainir_emit_c99(mod, out, &opts, &diag);
  if (out_path) fclose(out);
  if (rc != 0) {
    fprintf(stderr, "lainir-c emit error (%d) at %u:%u: %s\n", diag.code,
            diag.line, diag.column, diag.message);
  }

  lainir_builder_free(b);
  free(source);
  return rc == 0 ? 0 : 1;
}
