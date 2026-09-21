/* 切片 2 的验收：#eval 折叠。
 *
 * 判据：
 *   1. 折叠前后**解释执行结果完全相同**（折叠不改变语义）；
 *   2. 折掉的调用数正确，且折叠后的模块里没有 is_eval 残留；
 *   3. 折叠后的模块能被 C 后端编译（后端的「碰到 #eval 拒绝」不再触发）；
 *   4. 实参不是编译期已知的 → 拒绝（相位检查）。
 */
#include <stdio.h>
#include <string.h>

#include "host_functions.h"
#include "lainfold/fold.h"
#include "lainir/verify.h"
#include "lainbackend/emit.h"
#include "lainvm/engine.h"
#include "lainvm/image.h"

static int failures = 0;

static void check(bool ok, const char *what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

/* 在一个自己的地址空间里跑一次。 */
static int run_bits(const L1Module *module, const char *entry, uint64_t arg,
                    uint32_t nargs, LainVmCaps *caps, uint64_t *out) {
  LainVmSpace space;
  LainVmImage *image;
  LainVmTcb *tcb;
  L1Diagnostic diag;
  L1Value args[1];
  LainVmSliceResult result;
  int rc = 1;

  diag.code = 0;
  lainvm_space_init(&space);
  image = lainvm_image_load(module, &space, &diag);
  if (!image) {
    printf("     load failed: %d %s\n", diag.code, diag.message);
    return 1;
  }
  tcb = lainvm_tcb_new(image, &space, 1, 1, 64, 4096, 1);
  if (!tcb) {
    lainvm_image_free(image);
    return 1;
  }
  if (caps && lainvm_tcb_set_caps(tcb, caps, &diag) != 0) {
    printf("     caps failed: %d %s\n", diag.code, diag.message);
  } else {
    args[0] = (L1Value){L1_VALUE_BITS, 64, {.bits = arg}};
    if (lainvm_tcb_start(tcb, entry, nargs ? args : NULL, nargs, &diag) != 0) {
      printf("     start %s failed: %d %s\n", entry, diag.code, diag.message);
    } else {
      result = lainvm_engine_run(tcb, 1000000);
      if (result == LAINVM_SLICE_DONE && tcb->has_result) {
        *out = tcb->result.kind == L1_VALUE_ADDR
                   ? (uint64_t)(uintptr_t)tcb->result.as.addr
                   : tcb->result.as.bits;
        rc = 0;
      } else {
        printf("     run %s: slice=%d trap=%d\n", entry, (int)result,
               tcb->trap.status);
      }
    }
  }
  lainvm_tcb_free(tcb);
  lainvm_image_free(image);
  return rc;
}

/* 递归数一数还有多少 is_eval。 */
static uint32_t count_eval_region(const L1Region *region) {
  uint32_t count = 0;
  uint32_t i;
  if (!region) return 0;
  for (i = 0; i < region->inst_count; i++) {
    const L1Inst *inst = &region->insts[i];
    if (inst->is_eval) count++;
    count += count_eval_region(inst->body);
    count += count_eval_region(inst->else_body);
  }
  return count;
}

static uint32_t count_eval(const L1Module *module) {
  uint32_t count = 0;
  uint32_t i;
  for (i = 0; i < module->subroutine_count; i++) {
    if (module->subroutines[i].flags & SUBROUTINE_EXTERN) continue;
    count += count_eval_region(module->subroutines[i].body);
  }
  return count;
}

static FILE *g_out;
static uint32_t g_bytes;
static void sink_write(void *user, const char *bytes, uint32_t size) {
  (void)user;
  g_bytes += size;
  if (g_out) fwrite(bytes, 1, size, g_out);
}
static void sink_symbol(void *user, const char *symbol, bool is_extern) {
  (void)user;
  (void)symbol;
  (void)is_extern;
}

/* 造一个可折叠的模块：table_size 是纯计算，host_eval 走宿主能力。 */
static const L1Module *build_foldable(L1Builder *b) {
  const L1Type *u64 = lainir_type(b, TY_BITS, 64);
  L1Subroutine subs[4];
  L1Param p1[1];
  const L1Type *r1[1];
  uint32_t n = 0;

  /* 0: table_size() = 60 + 4 */
  {
    L1Operand add[2], ret[1];
    const L1Inst *insts[2];
    add[0] = lainir_int(60);
    add[1] = lainir_int(4);
    insts[0] = lainir_inst(b, INST_ADD, "%r", u64, add, 2);
    ret[0] = lainir_ref("%r");
    insts[1] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    r1[0] = u64;
    subs[n++] = *lainir_subroutine(b, "table_size", NULL, 0, r1, 1,
                                   lainir_region(b, NULL, 0, NULL, 0, insts, 2));
  }

  /* 1: use_table(%n) = %n + #eval table_size() */
  {
    L1Operand eval_ops[1];
    L1Operand add[2], ret[1];
    const L1Inst *insts[3];
    p1[0] = lainir_proc_param("%n", u64);
    eval_ops[0] = lainir_int(0); /* 占位，用不到：table_size 没有参数 */
    insts[0] = lainir_inst_call(b, "%s", "table_size", NULL, 0);
    ((L1Inst *)insts[0])->is_eval = true;
    add[0] = lainir_ref("%n");
    add[1] = lainir_ref("%s");
    insts[1] = lainir_inst(b, INST_ADD, "%r", u64, add, 2);
    ret[0] = lainir_ref("%r");
    insts[2] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    r1[0] = u64;
    subs[n++] = *lainir_subroutine(b, "use_table", p1, 1, r1, 1,
                                   lainir_region(b, NULL, 0, NULL, 0, insts, 3));
  }

  /* 2: 宿主能力 */
  {
    L1Param hp[1];
    hp[0] = lainir_proc_param("%x", u64);
    r1[0] = u64;
    subs[n++] = *lainir_subroutine_extern(b, "host_add_100", "host_add_100",
                                          hp, 1, r1, 1);
  }

  /* 3: host_eval() = #eval host_add_100(5) */
  {
    L1Operand call[1], ret[1];
    const L1Inst *insts[2];
    call[0] = lainir_int(5);
    insts[0] = lainir_inst_call(b, "%r", "host_add_100", call, 1);
    ((L1Inst *)insts[0])->is_eval = true;
    ret[0] = lainir_ref("%r");
    insts[1] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    r1[0] = u64;
    subs[n++] = *lainir_subroutine(b, "host_eval", NULL, 0, r1, 1,
                                   lainir_region(b, NULL, 0, NULL, 0, insts, 2));
  }

  return lainir_module(b, "foldable", NULL, 0, subs, n);
}

/* 不可折叠：eval 的实参是运行期的。 */
static const L1Module *build_phase_error(L1Builder *b) {
  const L1Type *u64 = lainir_type(b, TY_BITS, 64);
  L1Subroutine subs[2];
  L1Param p1[1];
  const L1Type *r1[1];
  L1Operand ret[1];

  p1[0] = lainir_proc_param("%x", u64);
  r1[0] = u64;
  {
    L1Operand add[2];
    const L1Inst *insts[2];
    add[0] = lainir_ref("%x");
    add[1] = lainir_int(1);
    insts[0] = lainir_inst(b, INST_ADD, "%r", u64, add, 2);
    ret[0] = lainir_ref("%r");
    insts[1] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    subs[0] = *lainir_subroutine(b, "g2", p1, 1, r1, 1,
                                 lainir_region(b, NULL, 0, NULL, 0, insts, 2));
  }
  {
    L1Operand call[1];
    const L1Inst *insts[2];
    call[0] = lainir_ref("%x");
    insts[0] = lainir_inst_call(b, "%r", "g2", call, 1);
    ((L1Inst *)insts[0])->is_eval = true;
    ret[0] = lainir_ref("%r");
    insts[1] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    subs[1] = *lainir_subroutine(b, "g", p1, 1, r1, 1,
                                 lainir_region(b, NULL, 0, NULL, 0, insts, 2));
  }
  return lainir_module(b, "phase_error", NULL, 0, subs, 2);
}

int main(void) {
  L1Builder *in = lainir_builder_new();
  L1Builder *out = lainir_builder_new();
  LainVmCaps *caps = lainvm_caps_new();
  LainFold *fold;
  const L1Module *before;
  const L1Module *after;
  L1Diagnostic diag;
  uint64_t v = 0;

  setvbuf(stdout, NULL, _IONBF, 0);
  lainvm_caps_add(caps, "host_add_100", LAINVM_CAP_FUNCTION, host_add_100);

  before = build_foldable(in);
  check(lainir_verify(before, &diag) == 0, "折叠前模块通过验证");
  check(count_eval(before) == 2, "折叠前有 2 处 #eval");

  check(run_bits(before, "use_table", 1, 1, caps, &v) == 0 && v == 65,
        "折叠前 use_table(1) = 65");
  check(run_bits(before, "host_eval", 0, 0, caps, &v) == 0 && v == 105,
        "折叠前 host_eval() = 105");

  fold = lainfold_new(caps, 64, 4096, 1000000);
  after = lainfold_module(fold, out, before, &diag);
  check(after != NULL, "折叠成功");
  if (!after) {
    printf("     diag: %d %s\n", diag.code, diag.message);
    return 1;
  }
  check(lainfold_folded_count(fold) == 2, "折掉 2 次");
  check(count_eval(after) == 0, "折叠后没有 #eval 残留");
  {
    /* 折叠不是「换个常量定义」，是把调用删掉、结果替进用处。 */
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < after->subroutine_count; i++) {
      const L1Subroutine *sub = &after->subroutines[i];
      if (sub->name && strcmp(sub->name, "use_table") == 0 && sub->body)
        n = sub->body->inst_count;
    }
    check(n == 2, "折叠后 use_table 只剩 2 条指令（调用被删掉）");
  }

  v = 0;
  check(run_bits(after, "use_table", 1, 1, caps, &v) == 0 && v == 65,
        "折叠后 use_table(1) = 65（语义不变）");
  v = 0;
  check(run_bits(after, "host_eval", 0, 0, caps, &v) == 0 && v == 105,
        "折叠后 host_eval() = 105（语义不变）");

  /* 折叠后的模块必须能被后端编译。 */
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
    g_out = fopen("build/tmp-probe/folded.c", "wb");
    g_bytes = 0;
    backend = lainbackend_new(&target, &sink, &diag);
    check(lainbackend_emit(backend, after) == 0 && g_bytes > 0,
          "折叠后的模块能被 C 后端编译");
    if (diag.code) printf("     diag: %d %s\n", diag.code, diag.message);
    lainbackend_free(backend);
    if (g_out) fclose(g_out);
    g_out = NULL;
  }

  /* 相位检查：实参不是编译期已知的，必须拒绝。 */
  {
    L1Builder *in2 = lainir_builder_new();
    L1Builder *out2 = lainir_builder_new();
    LainFold *fold2 = lainfold_new(caps, 64, 4096, 1000000);
    const L1Module *bad = build_phase_error(in2);
    const L1Module *r = lainfold_module(fold2, out2, bad, &diag);
    check(r == NULL && diag.code == 9304, "运行期实参的 #eval 被拒绝（9304）");
    if (r != NULL) printf("     unexpected success\n");
    lainfold_free(fold2);
    lainir_builder_free(out2);
    lainir_builder_free(in2);
  }

  lainfold_free(fold);
  lainvm_caps_free(caps);
  lainir_builder_free(out);
  lainir_builder_free(in);
  printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
