/* 切片 5 的验收（1/2）：后端的域外检查 + 宽度规范。
 *
 * 判据：
 *   1. 除零、移位越界在 VM 里是 trap，在编译产物里是 abort —— 两边都拒绝；
 *   2. 合法输入下两边给出同一个值（差分对拍由 harness 完成）；
 *   3. 宽度超过一个机器字的程序被验证器拒掉（2026），而不是让后端悄悄截断。
 */
#include <stdio.h>
#include <string.h>

#include "lainir/build.h"
#include "lainir/verify.h"
#include "lainbackend/emit.h"
#include "lainvm/engine.h"
#include "lainvm/image.h"

static int failures = 0;

static void check(bool ok, const char *what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static FILE *g_out;
static void sink_write(void *user, const char *bytes, uint32_t size) {
  (void)user;
  if (g_out) fwrite(bytes, 1, size, g_out);
}
static void sink_symbol(void *user, const char *symbol, bool is_extern) {
  (void)user;
  (void)symbol;
  (void)is_extern;
}

/* div_by(a,b) = a / b ；shift_by(a,b) = a << b */
static const L1Module *build_checks(L1Builder *b) {
  const L1Type *u64 = lainir_type(b, TY_BITS, 64);
  L1Subroutine subs[2];
  L1Param p[2];
  const L1Type *r[1];
  L1Operand ops[2], ret[1];

  p[0] = lainir_proc_param("%a", u64);
  p[1] = lainir_proc_param("%b", u64);
  r[0] = u64;
  ops[0] = lainir_ref("%a");
  ops[1] = lainir_ref("%b");
  ret[0] = lainir_ref("%r");

  {
    const L1Inst *insts[2];
    insts[0] = lainir_inst(b, INST_SDIV, "%r", u64, ops, 2);
    insts[1] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    subs[0] = *lainir_subroutine(b, "div_by", p, 2, r, 1,
                                 lainir_region(b, NULL, 0, NULL, 0, insts, 2));
  }
  {
    const L1Inst *insts[2];
    insts[0] = lainir_inst(b, INST_SHL, "%r", u64, ops, 2);
    insts[1] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    subs[1] = *lainir_subroutine(b, "shift_by", p, 2, r, 1,
                                 lainir_region(b, NULL, 0, NULL, 0, insts, 2));
  }
  return lainir_module(b, "checks", NULL, 0, subs, 2);
}

/* 宽度超过一个机器字：验证器必须拒。 */
static const L1Module *build_wide(L1Builder *b) {
  const L1Type *u128 = lainir_type(b, TY_BITS, 128);
  L1Subroutine subs[1];
  const L1Type *r[1];
  L1Operand ops[2], ret[1];
  const L1Inst *insts[2];

  r[0] = u128;
  ops[0] = lainir_int(1);
  ops[1] = lainir_int(2);
  insts[0] = lainir_inst(b, INST_ADD, "%r", u128, ops, 2);
  ret[0] = lainir_ref("%r");
  insts[1] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
  subs[0] = *lainir_subroutine(b, "wide", NULL, 0, r, 1,
                               lainir_region(b, NULL, 0, NULL, 0, insts, 2));
  return lainir_module(b, "wide", NULL, 0, subs, 1);
}

static int run2(const L1Module *module, const char *entry, uint64_t a0,
                uint64_t a1, uint64_t *out, int32_t *trap_status) {
  LainVmSpace space;
  LainVmImage *image;
  LainVmTcb *tcb;
  L1Diagnostic diag;
  L1Value args[2];
  LainVmSliceResult result;
  int rc = 1;

  *trap_status = 0;
  diag.code = 0;
  lainvm_space_init(&space);
  image = lainvm_image_load(module, &space, &diag);
  if (!image) return 1;
  tcb = lainvm_tcb_new(image, &space, 1, 1, 64, 4096);
  if (!tcb) {
    lainvm_image_free(image);
    return 1;
  }
  memset(args, 0, sizeof(args));
  args[0] = (L1Value){L1_VALUE_BITS, 64, {.bits = a0}};
  args[1] = (L1Value){L1_VALUE_BITS, 64, {.bits = a1}};
  if (lainvm_tcb_start(tcb, entry, args, 2, &diag) == 0) {
    result = lainvm_engine_run(tcb, 1000000);
    if (result == LAINVM_SLICE_DONE && tcb->has_result) {
      *out = tcb->result.as.bits;
      rc = 0;
    } else if (result == LAINVM_SLICE_TRAPPED) {
      *trap_status = tcb->trap.status;
      rc = 2;
    }
  }
  lainvm_tcb_free(tcb);
  lainvm_image_free(image);
  return rc;
}

int main(void) {
  L1Builder *b = lainir_builder_new();
  L1Diagnostic diag;
  const L1Module *m;
  uint64_t v = 0;
  int32_t status = 0;
  int rc;

  setvbuf(stdout, NULL, _IONBF, 0);
  diag.code = 0;
  m = build_checks(b);
  check(lainir_verify(m, &diag) == 0, "检查用的模块通过验证");

  /* 合法输入：两边必须给同一个值（C 那一侧由 harness 对拍）。 */
  rc = run2(m, "div_by", 10, 2, &v, &status);
  printf("div_by(10,2) = %llu\n", (unsigned long long)v);
  check(rc == 0 && v == 5, "VM: div_by(10,2) = 5");
  rc = run2(m, "shift_by", 1, 3, &v, &status);
  printf("shift_by(1,3) = %llu\n", (unsigned long long)v);
  check(rc == 0 && v == 8, "VM: shift_by(1,3) = 8");

  /* 域外：VM 拒绝（编译产物那一侧是 abort，由 harness 看退出码）。 */
  rc = run2(m, "div_by", 10, 0, &v, &status);
  printf("# VM: div_by(10,0) -> trapped %d\n", status);
  check(rc == 2 && status == 1001, "VM: 除零被拒（1001）");
  rc = run2(m, "shift_by", 1, 64, &v, &status);
  printf("# VM: shift_by(1,64) -> trapped %d\n", status);
  check(rc == 2 && status == 1002, "VM: 移位越界被拒（1002）");

  /* 宽度超一个机器字 */
  {
    L1Builder *b2 = lainir_builder_new();
    L1Diagnostic d2;
    const L1Module *wide = build_wide(b2);
    d2.code = 0;
    check(lainir_verify(wide, &d2) != 0 && d2.code == L1V_WIDTH_TOO_WIDE,
          "宽度 128 被验证器拒（2026），不让后端悄悄截断");
    printf("# wide diag: %d %s\n", d2.code, d2.message);
    lainir_builder_free(b2);
  }

  /* 把产物写出去，给 harness 编译 */
  {
    static const uint32_t ints[4] = {8, 16, 32, 64};
    static const uint32_t floats[2] = {32, 64};
    LainTarget target;
    LainBackendSink sink;
    LainBackend *backend;
    target.name = "c";
    target.address_bits = 64;
    target.int_widths = ints;
    target.int_width_count = 4;
    target.float_formats = floats;
    target.float_format_count = 2;
    target.max_alignment = 16;
    target.unaligned_ok = true;
    target.little_endian = true;
    target.capability = LAINBC_CAP_SYMBOL;
    sink.write = sink_write;
    sink.symbol = sink_symbol;
    sink.user = NULL;
    g_out = fopen("build/tmp-probe/checks.c", "wb");
    backend = lainbackend_new(&target, &sink, &diag);
    check(lainbackend_emit(backend, m) == 0, "检查用的模块能编成 C");
    lainbackend_free(backend);
    if (g_out) fclose(g_out);
    g_out = NULL;
  }

  lainir_builder_free(b);
  printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
