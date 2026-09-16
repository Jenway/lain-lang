/* 从文本读 LAINIR 程序，解析 -> 验证 -> 装载 -> 在 VM 里跑。
 *
 * 输入是 seed/tests/programs.l1（手写的文本），不再用 C 结构体手搭模块：
 * 文本路径是承重的，测试就该走它。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lainir/build.h"
#include "lainir/parse.h"
#include "lainir/print.h"
#include "lainir/verify.h"
#include "lainvm/engine.h"

static int failures = 0;

static void report_fail(const char *what, const char *why) {
  printf("FAIL %-14s %s\n", what, why);
  failures++;
}

static void expect_bits(LainVmTcb *tcb, const char *entry,
                        const L1Value *args, uint32_t arg_count, uint64_t want,
                        const char *what) {
  L1Diagnostic diag;
  LainVmSliceResult result;

  diag.code = 0;
  if (lainvm_tcb_start(tcb, entry, args, arg_count, &diag) != 0) {
    char buffer[192];
    snprintf(buffer, sizeof(buffer), "start: code=%d %s", diag.code,
             diag.message);
    report_fail(what, buffer);
    return;
  }
  result = lainvm_engine_run(tcb, 1000000);
  if (result != LAINVM_SLICE_DONE) {
    char buffer[192];
    snprintf(buffer, sizeof(buffer),
             "slice=%d trap kind=%d status=%d region=%u pos=%u steps=%llu",
             (int)result, (int)tcb->trap.kind, tcb->trap.status,
             tcb->trap.region, tcb->trap.position,
             (unsigned long long)tcb->steps);
    report_fail(what, buffer);
    return;
  }
  if (!tcb->has_result || tcb->result.as.bits != want) {
    char buffer[192];
    snprintf(buffer, sizeof(buffer), "got %llu want %llu",
             (unsigned long long)tcb->result.as.bits, (unsigned long long)want);
    report_fail(what, buffer);
    return;
  }
  printf("ok   %-14s = %llu  (%llu steps)\n", what,
         (unsigned long long)tcb->result.as.bits,
         (unsigned long long)tcb->steps);
}

static void expect_trap(LainVmTcb *tcb, const char *entry,
                        const L1Value *args, uint32_t arg_count,
                        const char *what) {
  L1Diagnostic diag;
  LainVmSliceResult result;

  diag.code = 0;
  if (lainvm_tcb_start(tcb, entry, args, arg_count, &diag) != 0) {
    report_fail(what, "start failed");
    return;
  }
  result = lainvm_engine_run(tcb, 1000000);
  if (result != LAINVM_SLICE_TRAPPED) {
    report_fail(what, "expected a trap");
    return;
  }
  printf("ok   %-14s trapped kind=%d status=%d\n", what, (int)tcb->trap.kind,
         tcb->trap.status);
}

static char *read_text_file(const char *path) {
  FILE *file = fopen(path, "rb");
  long size;
  char *text;
  if (!file) return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  size = ftell(file);
  if (size < 0) {
    fclose(file);
    return NULL;
  }
  rewind(file);
  text = (char *)malloc((size_t)size + 1);
  if (!text) {
    fclose(file);
    return NULL;
  }
  if (fread(text, 1, (size_t)size, file) != (size_t)size) {
    free(text);
    fclose(file);
    return NULL;
  }
  text[size] = '\0';
  fclose(file);
  return text;
}

int main(void) {
  static const char *k_paths[] = {"seed/tests/programs.l1", "programs.l1",
                                  "../seed/tests/programs.l1",
                                  "./seed/tests/programs.l1"};
  L1Builder *b = lainir_builder_new();
  L1Diagnostic diag;
  const L1Module *module;
  LainVmSpace space;
  LainVmImage *image;
  LainVmTcb *tcb;
  char *text = NULL;
  char *canonical;
  uint32_t i;

  setvbuf(stdout, NULL, _IONBF, 0);
  diag.code = 0;

  for (i = 0; i < sizeof(k_paths) / sizeof(k_paths[0]); i++) {
    text = read_text_file(k_paths[i]);
    if (text) break;
  }
  if (!text) {
    printf("cannot read the fixture (tried seed/tests/programs.l1)\n");
    lainir_builder_free(b);
    return 1;
  }

  module = lainir_parse(b, text, &diag);
  if (!module) {
    printf("parse failed: code=%d line=%u %s\n", diag.code, diag.line,
           diag.message);
    free(text);
    lainir_builder_free(b);
    return 1;
  }
  free(text);
  printf("parsed: %u subroutine(s), %u data object(s)\n",
         module->subroutine_count, module->data_count);

  if (lainir_verify(module, &diag) != 0) {
    printf("verify failed: code=%d %s\n", diag.code, diag.message);
    lainir_builder_free(b);
    return 1;
  }
  printf("verified\n");

  canonical = lainir_print_to_string(module);
  if (canonical) {
    FILE *dump = fopen("build/tmp-probe/programs_canonical.l1", "wb");
    if (dump) {
      fwrite(canonical, 1, strlen(canonical), dump);
      fclose(dump);
    }
    free(canonical);
  }

  lainvm_space_init(&space);
  image = lainvm_image_load(module, &space, &diag);
  if (!image) {
    printf("load failed: code=%d %s\n", diag.code, diag.message);
    lainir_builder_free(b);
    return 1;
  }
  printf("image: %u region(s), %u inst meta, %u operand ref, %u result slot\n",
         image->region_count, image->inst_count, image->operand_count,
         image->result_count);
  printf("bounds: max_region_depth=%u max_slots=%u\n", image->max_region_depth,
         image->max_slots);

  tcb = lainvm_tcb_new(image, &space, 1, 1, 64, 4096);
  if (!tcb) {
    printf("admit failed\n");
    lainvm_image_free(image);
    lainir_builder_free(b);
    return 1;
  }
  printf("tcb: frame_cap=%u slot_cap=%u stack_region=%d\n", tcb->frame_cap,
         tcb->slot_cap, tcb->stack_region);
  printf("---\n");

  expect_bits(tcb, "answer", NULL, 0, 42, "answer");

  {
    L1Value args[2];
    memset(args, 0, sizeof(args));
    args[0] = (L1Value){L1_VALUE_BITS, 64, {.bits = 3}};
    args[1] = (L1Value){L1_VALUE_BITS, 64, {.bits = 9}};
    expect_bits(tcb, "max", args, 2, 9, "max(3,9)");
    args[0].as.bits = 9;
    args[1].as.bits = 3;
    expect_bits(tcb, "max", args, 2, 9, "max(9,3)");
  }

  {
    L1Value args[1];
    memset(args, 0, sizeof(args));
    args[0] = (L1Value){L1_VALUE_BITS, 64, {.bits = 5}};
    expect_bits(tcb, "fact", args, 1, 120, "fact(5)");
    args[0].as.bits = 0;
    expect_bits(tcb, "fact", args, 1, 1, "fact(0)");
  }

  {
    L1Value args[2];
    memset(args, 0, sizeof(args));
    args[0] =
        (L1Value){L1_VALUE_ADDR, 0, {.addr = (void *)image->symbols[0].addr}};
    args[1] = (L1Value){L1_VALUE_BITS, 64, {.bits = 3}};
    expect_bits(tcb, "sum_bytes", args, 2, 24, "sum_bytes(3)");
    args[1].as.bits = 0;
    expect_bits(tcb, "sum_bytes", args, 2, 0, "sum_bytes(0)");
  }

  expect_bits(tcb, "first_byte", NULL, 0, 7, "first_byte");
  expect_bits(tcb, "alloca_store", NULL, 0, 5, "alloca_store");
  expect_bits(tcb, "slt32", NULL, 0, 1, "slt32(-1<1)");
  expect_trap(tcb, "bad_load", NULL, 0, "bad_load");
  expect_trap(tcb, "write_ro", NULL, 0, "write_ro");
  expect_bits(tcb, "via_ptr", NULL, 0, 42, "via_ptr");
  expect_trap(tcb, "bad_call", NULL, 0, "bad_call");

  printf("---\n");
  printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");

  lainvm_tcb_free(tcb);
  lainvm_image_free(image);
  lainir_builder_free(b);
  return failures == 0 ? 0 : 1;
}
