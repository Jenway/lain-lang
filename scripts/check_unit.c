/* 把一份 LAINIR 文本装回 lainir_parse + lainir_verify，供闸门判断 Meta 的产物是否合格。
 * 编译命令见 docs/development.md。 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lain/ir/build.h"
#include "lain/ir/core.h"
#include "lain/text/parse.h"
#include "lain/ir/verify.h"

int main(int argc, char **argv) {
  FILE *f;
  long n;
  char *buf;
  L1Builder *builder;
  const L1Module *module;
  L1Diagnostic diag;
  if (argc < 2) { printf("usage: check_unit <file.lain>\n"); return 2; }
  f = fopen(argv[1], "rb");
  if (!f) { printf("open failed\n"); return 2; }
  fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
  buf = (char *)malloc((size_t)n + 1);
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) { printf("read failed\n"); return 2; }
  buf[n] = 0;
  fclose(f);
  builder = lainir_builder_new();
  memset(&diag, 0, sizeof(diag));
  module = lainir_parse(builder, buf, &diag);
  if (!module) {
    printf("PARSE FAILED code=%u line=%u col=%u msg=%s\n", diag.code, diag.line, diag.column, diag.message);
    return 1;
  }
  if (lainir_verify(module, &diag) != 0) {
    printf("VERIFY FAILED code=%u line=%u col=%u msg=%s\n", diag.code, diag.line, diag.column, diag.message);
    return 1;
  }
  printf("PARSE+VERIFY OK %s\n", argv[1]);
  return 0;
}
