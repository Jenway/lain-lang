/* LAINIR 校验：把若干份 canonical LAINIR 文本当成**一个模块**解析，再跑验证器。
 *
 * 用它是为了校验 Meta 产出的单元：一份单元跨模块引用别的单元里的过程与数据块，
 * 单独拿出来必然报未知符号；把同一程序全部单元拼在一起才是完整模块。
 *
 * 用法：lainir-verify [--print] <文件> [<文件> ...]
 * --print：校验通过后把 canonical 文本打到标准输出（不打印摘要行）。
 * 退出码：0 = 解析与验证都过；1 = 失败；2 = 用法或 IO 失败。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lain/ir/build.h"
#include "lain/ir/core.h"
#include "lain/ir/verify.h"
#include "lain/text/parse.h"
#include "lain/text/print.h"

static char *read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long size = ftell(f);
  if (size < 0) { fclose(f); return NULL; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
  char *buf = (char *)malloc((size_t)size + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t got = fread(buf, 1, (size_t)size, f);
  fclose(f);
  if (got != (size_t)size) { free(buf); return NULL; }
  buf[got] = '\0';
  *out_len = got;
  return buf;
}

int main(int argc, char **argv) {
  int first = 1;
  int print_mode = 0;
  if (first < argc && strcmp(argv[first], "--print") == 0) {
    print_mode = 1;
    first++;
  }
  if (first >= argc) {
    fputs("用法: lainir-verify [--print] <文件> [<文件> ...]\n", stderr);
    return 2;
  }
  int file_count = argc - first;
  size_t total = 0;
  for (int i = first; i < argc; ++i) {
    size_t len = 0;
    char *body = read_file(argv[i], &len);
    if (!body) { printf("读不到 %s\n", argv[i]); return 2; }
    free(body);
    total += len + 1;
  }
  char *joined = (char *)malloc(total + 1);
  if (!joined) { puts("合并缓冲分配失败"); return 1; }
  size_t at = 0;
  for (int i = first; i < argc; ++i) {
    size_t len = 0;
    char *body = read_file(argv[i], &len);
    if (!body) { free(joined); printf("读不到 %s\n", argv[i]); return 2; }
    memcpy(joined + at, body, len);
    at += len;
    joined[at++] = '\n';
    free(body);
  }
  joined[at] = '\0';

  L1Diagnostic diag;
  memset(&diag, 0, sizeof(diag));
  L1Builder *builder = lainir_builder_new();
  if (!builder) { free(joined); puts("builder 分配失败"); return 1; }
  const L1Module *module = lainir_parse(builder, joined, &diag);
  free(joined);
  if (!module) {
    printf("PARSE FAILED code=%d line=%u\n", diag.code, diag.line);
    lainir_builder_free(builder);
    return 1;
  }
  if (lainir_verify(module, &diag) != 0) {
    printf("VERIFY FAILED code=%d line=%u\n", diag.code, diag.line);
    lainir_builder_free(builder);
    return 1;
  }
  if (print_mode) {
    char *text = lainir_print_to_string(module);
    if (!text) { lainir_builder_free(builder); puts("打印缓冲分配失败"); return 1; }
    fputs(text, stdout);
    free(text);
    lainir_builder_free(builder);
    return 0;
  }
  printf("verify ok subroutines=%u data=%u files=%d\n", module->subroutine_count,
         module->data_count, file_count);
  lainir_builder_free(builder);
  return 0;
}
