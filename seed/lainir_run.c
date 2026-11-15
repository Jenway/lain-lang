/* LAINIR 执行：把若干份 canonical LAINIR 文本当成**一个模块**解析、验证、装载进
 * Lain-VM，然后从指定入口真跑一遍，打印返回值。
 *
 * 用它是为了给降级产物做真值测试：校验器只回答"这份 IR 结构上合法"，回答不了
 * "它算出来的东西对不对"。真跑一遍才知道。
 *
 * 用法：lainir-run [--entry <过程名>] [--arg <整数>]... [--fuel <n>] <文件> [<文件> ...]
 * 退出码：0 = 跑完并有结果；1 = 解析/验证/装载/启动失败；
 *         3 = Trap；4 = 跑完但没有结果；5 = 燃料耗尽；2 = 用法或 IO 失败。
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lain/ir/build.h"
#include "lain/ir/core.h"
#include "lain/ir/infer.h"
#include "lain/ir/verify.h"
#include "lain/text/parse.h"
#include "lain/vm/caps.h"
#include "lain/vm/engine.h"
#include "lain/vm/quota.h"
#include "lain/vm/space.h"
#include "lain/vm/tcb.h"

#define RUN_ARG_MAX 16
#define RUN_STACK_BYTES (4u << 20)
#define RUN_MAX_DEPTH 64
#define RUN_SLICE_FUEL 2000000ull

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

/* 把全部文件按顺序接成一个缓冲，每份后面补一个换行。 */
static char *read_join(int first, int argc, char **argv) {
  size_t total = 0;
  for (int i = first; i < argc; ++i) {
    size_t len = 0;
    char *body = read_file(argv[i], &len);
    if (!body) { printf("读不到 %s\n", argv[i]); return NULL; }
    free(body);
    total += len + 1;
  }
  char *joined = (char *)malloc(total + 1);
  if (!joined) { puts("合并缓冲分配失败"); return NULL; }
  size_t at = 0;
  for (int i = first; i < argc; ++i) {
    size_t len = 0;
    char *body = read_file(argv[i], &len);
    if (!body) { free(joined); printf("读不到 %s\n", argv[i]); return NULL; }
    memcpy(joined + at, body, len);
    at += len;
    joined[at++] = '\n';
    free(body);
  }
  joined[at] = '\0';
  return joined;
}

static void usage(FILE *out) {
  fputs("用法: lainir-run [--entry <过程名>] [--arg <整数>]... [--fuel <n>] <文件> [<文件> ...]\n",
        out);
}

int main(int argc, char **argv) {
  const char *entry = "main";
  uint64_t fuel = 40000000ull;
  uint64_t args[RUN_ARG_MAX];
  uint32_t arg_count = 0;
  int first_file = 1;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--entry")) {
      if (i + 1 >= argc) { usage(stderr); return 2; }
      entry = argv[++i];
      first_file = i + 1;
      continue;
    }
    if (!strcmp(argv[i], "--fuel")) {
      if (i + 1 >= argc) { usage(stderr); return 2; }
      fuel = strtoull(argv[++i], NULL, 0);
      first_file = i + 1;
      continue;
    }
    if (!strcmp(argv[i], "--arg")) {
      if (i + 1 >= argc || arg_count == RUN_ARG_MAX) { usage(stderr); return 2; }
      args[arg_count++] = strtoull(argv[++i], NULL, 0);
      first_file = i + 1;
      continue;
    }
    if (argv[i][0] == '-' && argv[i][1]) { usage(stderr); return 2; }
    first_file = i;
    break;
  }
  if (first_file >= argc) { usage(stderr); return 2; }

  char *joined = read_join(first_file, argc, argv);
  if (!joined) return 2;

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

  /* 装载与执行。这里不注册任何能力：外部符号调不到就 Trap 1110，
   * 那是纯计算样例应当暴露的事实，不该被宿主能力掩盖。 */
  LainVmSpace space;
  lainvm_space_init(&space);
  LainVmImage *image = lainvm_image_load(module, &space, &diag);
  if (!image) {
    printf("IMAGE FAILED code=%d line=%u\n", diag.code, diag.line);
    lainir_builder_free(builder);
    return 1;
  }
  LainVmQuota quota;
  lainvm_quota_init(&quota, 0);
  LainVmRegionHandle stack = lainvm_space_alloc_stack(&space, RUN_STACK_BYTES, 1, &quota);
  if (lainvm_space_handle_none(stack)) {
    puts("没有栈区");
    lainvm_image_free(image);
    lainir_builder_free(builder);
    return 1;
  }
  LainVmStackLease lease;
  lease.space = &space;
  lease.region = stack;
  LainVmTcb *tcb = lainvm_tcb_new(image, &space, 1, 1, RUN_MAX_DEPTH, lease, &quota);
  if (!tcb) {
    puts("tcb_new 失败");
    lainvm_image_free(image);
    lainir_builder_free(builder);
    return 1;
  }

  /* 实参宽度取入口过程声明的参数宽度：宽度对不上，返回值宽度就会随实参漂移，
   * 差分测试会拿这种漂移当成编译器差异。 */
  const L1Subroutine *entry_sub = lainir_find_subroutine(module, entry);
  L1Value argv_bits[RUN_ARG_MAX];
  for (uint32_t i = 0; i < arg_count; ++i) {
    uint32_t width = 64;
    if (entry_sub && i < entry_sub->param_count && entry_sub->params[i].ty) {
      const L1Type *ty = entry_sub->params[i].ty;
      if (ty->kind == TY_ADDR) {
        printf("入口参数 %s 的类型是 #addr，lainir-run 只能传位串\n",
               entry_sub->params[i].name);
        lainvm_tcb_free(tcb);
        lainvm_image_free(image);
        lainir_builder_free(builder);
        return 2;
      }
      if (ty->kind == TY_BITS && ty->width) width = ty->width;
    }
    argv_bits[i].kind = L1_VALUE_BITS;
    argv_bits[i].bit_width = width;
    argv_bits[i].as.bits = args[i];
  }
  memset(&diag, 0, sizeof(diag));
  int started = lainvm_tcb_start(tcb, entry, argv_bits, arg_count, &diag);
  if (started != 0) {
    printf("START FAILED code=%d line=%u\n", diag.code, diag.line);
    lainvm_tcb_free(tcb);
    lainvm_image_free(image);
    lainir_builder_free(builder);
    return 1;
  }

  LainVmSliceResult slice = LAINVM_SLICE_RUNNABLE;
  uint64_t used = 0;
  while (used < fuel) {
    uint64_t want = fuel - used;
    if (want > RUN_SLICE_FUEL) want = RUN_SLICE_FUEL;
    uint64_t before = lainvm_tcb_steps(tcb);
    slice = lainvm_engine_run(tcb, want);
    uint64_t advanced = lainvm_tcb_steps(tcb) - before;
    used += advanced;
    if (slice != LAINVM_SLICE_RUNNABLE) break;
    if (advanced == 0) break;
  }
  printf("run slice=%d steps=%" PRIu64 "\n", (int)slice, lainvm_tcb_steps(tcb));

  if (slice == LAINVM_SLICE_RUNNABLE) {
    printf("RUN INCOMPLETE fuel=%" PRIu64 " used=%" PRIu64 "\n", fuel, used);
    lainvm_tcb_free(tcb);
    lainvm_image_free(image);
    lainir_builder_free(builder);
    return 5;
  }
  const LainVmTrap *trap = lainvm_tcb_trap(tcb);
  if (trap && trap->active) {
    printf("trap kind=%d status=%d line=%u column=%u\n", (int)trap->kind,
           trap->status, trap->line, trap->column);
    lainvm_tcb_free(tcb);
    lainvm_image_free(image);
    lainir_builder_free(builder);
    return 3;
  }
  if (!lainvm_tcb_has_result(tcb)) {
    puts("result none");
    lainvm_tcb_free(tcb);
    lainvm_image_free(image);
    lainir_builder_free(builder);
    return 4;
  }
  L1Value r = lainvm_tcb_result(tcb);
  if (r.kind == L1_VALUE_ADDR) {
    printf("result addr=0x%" PRIxPTR "\n", (uintptr_t)r.as.addr);
  } else {
    /* 位串按声明的宽度取值：宽度 < 64 时高位是脏的，先截断再给出有符号读数。 */
    uint32_t width = r.bit_width;
    uint64_t raw = r.as.bits;
    if (width == 0) width = 64;
    if (width < 64) raw &= ((1ull << width) - 1ull);
    int64_t signed_value;
    if (width >= 64) {
      signed_value = (int64_t)raw;
    } else {
      uint64_t sign_bit = 1ull << (width - 1);
      signed_value = (raw & sign_bit) ? (int64_t)(raw | ~((1ull << width) - 1ull))
                                      : (int64_t)raw;
    }
    printf("result value=%" PRIu64 " signed=%" PRId64 " width=%u\n", raw,
           signed_value, width);
  }

  lainvm_tcb_free(tcb);
  lainvm_image_free(image);
  lainir_builder_free(builder);
  return 0;
}
