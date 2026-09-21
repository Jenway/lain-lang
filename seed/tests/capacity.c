/* 容量回归：**上限只能来自模块自己**。
 *
 * 这里生成一个大模块（几百个过程 + 一个几千条指令的过程），把
 * parse -> verify -> load -> run 和 backend 全走一遍。
 *
 * 它守的是这样一组曾经存在的定长上限：
 *   解析器   data[64] / subs[64]（硬编码）      -> 200 个过程编不了
 *   验证器   bindings[1024]                     -> 3000 个局部量被拒
 *   装载器   regions[128] insts[2048] ...       -> 200 个过程装不下
 *   后端     regions[128] widths[512]           -> 槽数越界时**静默**按 64 位算
 *   能力表   entries[64]                        -> 旧宿主登记了 92 项
 * 每一个都是「内核结构不做动态扩容」被误读成「编译期常量」的结果。 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lainir/build.h"
#include "lainir/parse.h"
#include "lainir/verify.h"
#include "lainbackend/emit.h"
#include "lainvm/engine.h"

#define PROC_COUNT 400u
#define INST_COUNT 3000u

static int failures = 0;

static void report_fail(const char *what, const char *why) {
  printf("FAIL %-24s %s\n", what, why);
  failures++;
}

/* --- 把两份文本拼起来 ------------------------------------------------------ */

typedef struct {
  char *data;
  size_t used;
  size_t cap;
} Buffer;

static void buf_init(Buffer *b) {
  b->cap = 1u << 20;
  b->data = (char *)malloc(b->cap);
  b->used = 0;
  if (b->data) b->data[0] = '\0';
}

static void buf_addf(Buffer *b, const char *fmt, ...) {
  va_list args;
  int n;
  if (!b->data) return;
  va_start(args, fmt);
  n = vsnprintf(b->data + b->used, b->cap - b->used, fmt, args);
  va_end(args);
  if (n > 0) b->used += (size_t)n;
}

/* --- 输出的空 sink -------------------------------------------------------- */

static void sink_write(void *user, const char *bytes, uint32_t size) {
  uint32_t *total = (uint32_t *)user;
  (void)bytes;
  if (total) *total += size;
}

static void sink_symbol(void *user, const char *symbol, bool is_extern) {
  (void)user;
  (void)symbol;
  (void)is_extern;
}

static uint32_t emitted_bytes = 0;

static void make_target(LainTarget *target) {
  static const uint32_t ints[4] = {8, 16, 32, 64};
  static const uint32_t floats[2] = {32, 64};
  target->name = "c";
  target->address_bits = 64;
  target->int_widths = ints;
  target->int_width_count = 4;
  target->float_formats = floats;
  target->float_format_count = 2;
  target->max_alignment = 16;
  target->unaligned_ok = true;
  target->little_endian = true;
  target->capability = LAINBC_CAP_SYMBOL;
}

int main(void) {
  Buffer text;
  L1Builder *builder = lainir_builder_new();
  L1Diagnostic diag;
  const L1Module *module;
  LainVmSpace space;
  LainVmImage *image;
  LainVmTcb *tcb;
  uint32_t i;

  setvbuf(stdout, NULL, _IONBF, 0);
  buf_init(&text);
  if (!text.data) {
    printf("cannot allocate the module text\n");
    return 1;
  }

  /* 一个过程 3000 条指令：链式相加，终点值 = INST_COUNT。 */
  buf_addf(&text, "#proc chain() -> #bits<64> {\n");
  buf_addf(&text, "  %%v0 = #add[#bits<64>](0, 1)\n");
  for (i = 1; i < INST_COUNT; i++)
    buf_addf(&text, "  %%v%u = #add[#bits<64>](%%v%u, 1)\n", i, i - 1);
  buf_addf(&text, "  #return %%v%u\n", INST_COUNT - 1);
  buf_addf(&text, "}\n\n");

  /* PROC_COUNT 个过程：链式调用，f0 返回 7。 */
  buf_addf(&text, "#proc f0() -> #bits<64> {\n  #return 7\n}\n\n");
  for (i = 1; i < PROC_COUNT; i++) {
    buf_addf(&text, "#proc f%u() -> #bits<64> {\n", i);
    buf_addf(&text, "  %%r = #call f%u()\n", i - 1);
    buf_addf(&text, "  #return %%r\n}\n\n");
  }

  printf("generated: %u procedures, one with %u instructions (%u bytes)\n",
         PROC_COUNT, INST_COUNT, (unsigned)text.used);

  diag.code = 0;
  module = lainir_parse(builder, text.data, &diag);
  if (!module) {
    char buffer[192];
    snprintf(buffer, sizeof(buffer), "parse: %d %s", diag.code, diag.message);
    report_fail("parse", buffer);
    return 1;
  }
  printf("ok   parse    %u subroutines\n", module->subroutine_count);

  if (lainir_verify(module, &diag) != 0) {
    char buffer[192];
    snprintf(buffer, sizeof(buffer), "verify: %d %s", diag.code, diag.message);
    report_fail("verify", buffer);
    return 1;
  }
  printf("ok   verify   %u subroutines\n", module->subroutine_count);

  lainvm_space_init(&space);
  image = lainvm_image_load(module, &space, &diag);
  if (!image) {
    char buffer[192];
    snprintf(buffer, sizeof(buffer), "load: %d %s", diag.code, diag.message);
    report_fail("load", buffer);
    return 1;
  }
  printf("ok   load     %u regions, %u insts, %u operands, %u results\n",
         image->region_count, image->inst_count, image->operand_count,
         image->result_count);

  tcb = lainvm_tcb_new(image, &space, 1, 1, 64, lainvm_stack_no_lease(), NULL);
  if (!tcb) {
    report_fail("admit", "failed");
    return 1;
  }

  diag.code = 0;
  if (lainvm_tcb_start(tcb, "chain", NULL, 0, &diag) != 0) {
    report_fail("run chain", "start failed");
  } else if (lainvm_engine_run(tcb, 1000000) != LAINVM_SLICE_DONE) {
    report_fail("run chain", "trapped");
  } else if (tcb->result.as.bits != INST_COUNT) {
    char buffer[192];
    snprintf(buffer, sizeof(buffer), "got %llu want %u",
             (unsigned long long)tcb->result.as.bits, INST_COUNT);
    report_fail("run chain", buffer);
  } else {
    printf("ok   run      chain() = %llu\n",
           (unsigned long long)tcb->result.as.bits);
  }

  /* 400 个过程一条链要 400 层递归，admit 只有 64 —— 那是调用方给的预算，
   * 不是上限。这里只跑链尾那一段，验证过程数本身不构成障碍。 */
  diag.code = 0;
  if (lainvm_tcb_start(tcb, "f63", NULL, 0, &diag) != 0) {
    report_fail("run f63", "start failed");
  } else if (lainvm_engine_run(tcb, 1000000) != LAINVM_SLICE_DONE) {
    report_fail("run f63", "trapped");
  } else if (tcb->result.as.bits != 7) {
    report_fail("run f63", "expected 7");
  } else {
    printf("ok   run      f63() = %llu (64 levels of %u procedures)\n",
           (unsigned long long)tcb->result.as.bits, PROC_COUNT);
  }

  /* 后端：槽号是每过程重排的，所以这里同时压了「单过程 3000 槽」。
   * 越界曾经是静默按 64 位算，现在应当要么正确要么明确报错。 */
  {
    LainTarget target;
    LainBackendSink sink;
    LainBackend *backend;
    make_target(&target);
    sink.write = sink_write;
    sink.symbol = sink_symbol;
    sink.user = &emitted_bytes;
    diag.code = 0;
    backend = lainbackend_new(&target, &sink, &diag);
    if (!backend) {
      report_fail("backend", "cannot create");
    } else if (lainbackend_emit(backend, module) != 0) {
      char buffer[192];
      snprintf(buffer, sizeof(buffer), "emit: %d %s", diag.code, diag.message);
      report_fail("backend", buffer);
    } else {
      printf("ok   backend  emitted %u bytes\n", emitted_bytes);
    }
    lainbackend_free(backend);
  }

  printf("---\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES");

  lainvm_tcb_free(tcb);
  lainvm_image_free(image);
  lainir_builder_free(builder);
  free(text.data);
  return failures == 0 ? 0 : 1;
}
