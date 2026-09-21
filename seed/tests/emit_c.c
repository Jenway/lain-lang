/* 差分测试：同一批 LAINIR 程序，一边走 VM（engine），一边编译成 C。
 *
 * 这个程序负责：搭模块 -> 装载 -> 用 VM 跑每个用例并打印 -> 同时把 C 后端
 * 的产物写进文件。C 那一侧的答案由 cbackend_driver.c 给出，两边输出对齐了
 * 才算通过。
 */
#include <stdio.h>
#include <string.h>

#include "host_functions.h"
#include "lainir/build.h"
#include "lainir/verify.h"
#include "lainbackend/emit.h"
#include "lainvm/engine.h"

/* 子过程顺序——驱动那一侧硬编码同样的序号。 */
enum {
  SUB_ANSWER = 0,
  SUB_MAX = 1,
  SUB_FACT = 2,
  SUB_SUM_BYTES = 3,
  SUB_FIRST_BYTE = 4,
  SUB_INC = 5,
  SUB_APPLY = 6,
  SUB_VIA_PTR = 7,
  SUB_SLT32 = 8,
  SUB_ALLOCA_STORE = 9,
  SUB_HOST_ADD_100 = 10,
  SUB_HOST_CALLER = 11,
  SUB_HOST_SUM_BYTES = 12,
  SUB_HOST_SUM = 13,
  SUB_HOST_REFUSE = 14,
  SUB_HOST_REFUSED = 15,
  SUB_HOST_MISSING = 16,
  SUB_HOST_MISSING_CALLER = 17,
  SUB_CLASSIFY = 18,
  SUB_COUNT = 19
};

static FILE *g_out;

static void sink_write(void *user, const char *bytes, uint32_t size) {
  (void)user;
  fwrite(bytes, 1, size, g_out);
}

static void sink_symbol(void *user, const char *symbol, bool is_extern) {
  (void)user;
  printf("# symbol %s%s\n", is_extern ? "extern " : "", symbol);
}

typedef struct {
  const char *name;
  uint32_t sub;
  uint64_t args[2];
  uint32_t nargs;
} Case;

int main(void) {
  L1Builder *b = lainir_builder_new();
  const L1Type *u64 = lainir_type(b, TY_BITS, 64);
  const L1Type *u8 = lainir_type(b, TY_BITS, 8);
  const L1Type *u32 = lainir_type(b, TY_BITS, 32);
  const L1Type *addr = lainir_type(b, TY_ADDR, 0);
  static const uint8_t bytes[3] = {7, 8, 9};
  L1Data data[1];
  L1Subroutine subs[SUB_COUNT];
  L1Diagnostic diag;
  LainVmSpace space;
  LainVmImage *image;
  LainVmTcb *tcb;
  uint32_t nsubs = 0;

  /* --- answer() = 40 + 2 --- */
  {
    L1Operand add[2];
    L1Operand ret[1];
    const L1Inst *insts[2];
    const L1Type *rtypes[1];
    add[0] = lainir_int(40);
    add[1] = lainir_int(2);
    insts[0] = lainir_inst(b, INST_ADD, "%r", u64, add, 2);
    ret[0] = lainir_ref("%r");
    insts[1] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    rtypes[0] = u64;
    subs[nsubs] = *lainir_subroutine(b, "answer", NULL, 0, rtypes, 1,
                                     lainir_region(b, NULL, 0, NULL, 0, insts, 2));
    nsubs++;
  }

  /* --- max(a, b) --- */
  {
    L1Param params[2];
    L1Operand cmp[2], cond[1], ret[1], ya[1], yb[1];
    const L1Inst *insts[3], *then_i[1], *else_i[1];
    const L1Type *rtypes[1];
    const L1Region *then_body, *else_body;
    params[0] = lainir_proc_param("%a", u64);
    params[1] = lainir_proc_param("%b", u64);
    cmp[0] = lainir_ref("%a");
    cmp[1] = lainir_ref("%b");
    insts[0] = lainir_inst(b, INST_SGT, "%c", u64, cmp, 2);
    ya[0] = lainir_ref("%a");
    yb[0] = lainir_ref("%b");
    then_i[0] = lainir_inst(b, INST_YIELD, NULL, NULL, ya, 1);
    else_i[0] = lainir_inst(b, INST_YIELD, NULL, NULL, yb, 1);
    rtypes[0] = u64;
    then_body = lainir_region(b, NULL, 0, rtypes, 1, then_i, 1);
    else_body = lainir_region(b, NULL, 0, rtypes, 1, else_i, 1);
    cond[0] = lainir_ref("%c");
    insts[1] = lainir_inst_if(b, "%r", cond[0], then_body, else_body);
    ret[0] = lainir_ref("%r");
    insts[2] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    subs[nsubs] = *lainir_subroutine(b, "max", params, 2, rtypes, 1,
                                     lainir_region(b, NULL, 0, NULL, 0, insts, 3));
    nsubs++;
  }

  /* --- fact(n)：递归 + 区域产出值 --- */
  {
    L1Param params[1];
    L1Operand cmp[2], cond[1], ret[1], y1[1], yp[1], sub_ops[2], call_ops[1],
        mul_ops[2];
    const L1Inst *insts[3], *then_i[1], *else_i[4];
    const L1Type *rtypes[1];
    const L1Region *then_body, *else_body;
    params[0] = lainir_proc_param("%n", u64);
    cmp[0] = lainir_ref("%n");
    cmp[1] = lainir_int(1);
    insts[0] = lainir_inst(b, INST_SLE, "%c", u64, cmp, 2);
    y1[0] = lainir_int(1);
    then_i[0] = lainir_inst(b, INST_YIELD, NULL, NULL, y1, 1);
    sub_ops[0] = lainir_ref("%n");
    sub_ops[1] = lainir_int(1);
    else_i[0] = lainir_inst(b, INST_SUB, "%m", u64, sub_ops, 2);
    call_ops[0] = lainir_ref("%m");
    else_i[1] = lainir_inst_call(b, "%f", "fact", call_ops, 1);
    mul_ops[0] = lainir_ref("%n");
    mul_ops[1] = lainir_ref("%f");
    else_i[2] = lainir_inst(b, INST_MUL, "%p", u64, mul_ops, 2);
    yp[0] = lainir_ref("%p");
    else_i[3] = lainir_inst(b, INST_YIELD, NULL, NULL, yp, 1);
    rtypes[0] = u64;
    then_body = lainir_region(b, NULL, 0, rtypes, 1, then_i, 1);
    else_body = lainir_region(b, NULL, 0, rtypes, 1, else_i, 4);
    cond[0] = lainir_ref("%c");
    insts[1] = lainir_inst_if(b, "%r", cond[0], then_body, else_body);
    ret[0] = lainir_ref("%r");
    insts[2] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    subs[nsubs] = *lainir_subroutine(b, "fact", params, 1, rtypes, 1,
                                     lainir_region(b, NULL, 0, NULL, 0, insts, 3));
    nsubs++;
  }

  /* --- sum_bytes(p, n)：循环 + 内存 --- */
  {
    L1Param params[2];
    L1RegionParam loop_params[2];
    L1Operand ret[1], uge[2], cond[1], brk[1], lea[4], ld[1], zx[1], add1[2],
        add2[2], cont[2];
    const L1Inst *insts[2], *body[8], *then_i[1];
    const L1Type *rtypes[1];
    const L1Region *then_body, *loop_body;
    params[0] = lainir_proc_param("%p", addr);
    params[1] = lainir_proc_param("%n", u64);
    uge[0] = lainir_ref("%i");
    uge[1] = lainir_ref("%n");
    body[0] = lainir_inst(b, INST_UGE, "%c", u64, uge, 2);
    brk[0] = lainir_ref("%s");
    then_i[0] = lainir_inst_jump(b, INST_BREAK, "bytes", brk, 1);
    then_body = lainir_region(b, NULL, 0, NULL, 0, then_i, 1);
    cond[0] = lainir_ref("%c");
    body[1] = lainir_inst_if(b, NULL, cond[0], then_body, NULL);
    lea[0] = lainir_ref("%p");
    lea[1] = lainir_ref("%i");
    lea[2] = lainir_int(1);
    lea[3] = lainir_int(0);
    body[2] = lainir_inst(b, INST_LEA, "%a", NULL, lea, 4);
    ld[0] = lainir_ref("%a");
    body[3] = lainir_inst(b, INST_LOAD, "%b", u8, ld, 1);
    zx[0] = lainir_ref("%b");
    body[4] = lainir_inst(b, INST_ZEXT, "%w", u64, zx, 1);
    add1[0] = lainir_ref("%s");
    add1[1] = lainir_ref("%w");
    body[5] = lainir_inst(b, INST_ADD, "%s2", u64, add1, 2);
    add2[0] = lainir_ref("%i");
    add2[1] = lainir_int(1);
    body[6] = lainir_inst(b, INST_ADD, "%i2", u64, add2, 2);
    cont[0] = lainir_ref("%i2");
    cont[1] = lainir_ref("%s2");
    body[7] = lainir_inst_jump(b, INST_CONTINUE, "bytes", cont, 2);
    loop_params[0] = lainir_param("%i", u64, lainir_int(0));
    loop_params[1] = lainir_param("%s", u64, lainir_int(0));
    rtypes[0] = u64;
    loop_body = lainir_region(b, loop_params, 2, rtypes, 1, body, 8);
    insts[0] = lainir_inst_loop(b, "%sum", "bytes", loop_body);
    ret[0] = lainir_ref("%sum");
    insts[1] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    subs[nsubs] = *lainir_subroutine(b, "sum_bytes", params, 2, rtypes, 1,
                                     lainir_region(b, NULL, 0, NULL, 0, insts, 2));
    nsubs++;
  }

  /* --- first_byte()：模块数据 --- */
  {
    L1Operand ret[1], ld[1], zx[1];
    const L1Inst *insts[4];
    const L1Type *rtypes[1];
    insts[0] = lainir_inst_symbol(b, INST_DATA_ADDR, "%p", "bytes");
    ld[0] = lainir_ref("%p");
    insts[1] = lainir_inst(b, INST_LOAD, "%b", u8, ld, 1);
    zx[0] = lainir_ref("%b");
    insts[2] = lainir_inst(b, INST_ZEXT, "%w", u64, zx, 1);
    ret[0] = lainir_ref("%w");
    insts[3] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    rtypes[0] = u64;
    subs[nsubs] = *lainir_subroutine(b, "first_byte", NULL, 0, rtypes, 1,
                                     lainir_region(b, NULL, 0, NULL, 0, insts, 4));
    nsubs++;
  }

  /* --- inc(x) = x + 1 --- */
  {
    L1Param params[1];
    L1Operand add[2], ret[1];
    const L1Inst *insts[2];
    const L1Type *rtypes[1];
    params[0] = lainir_proc_param("%x", u64);
    add[0] = lainir_ref("%x");
    add[1] = lainir_int(1);
    insts[0] = lainir_inst(b, INST_ADD, "%r", u64, add, 2);
    ret[0] = lainir_ref("%r");
    insts[1] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    rtypes[0] = u64;
    subs[nsubs] = *lainir_subroutine(b, "inc", params, 1, rtypes, 1,
                                     lainir_region(b, NULL, 0, NULL, 0, insts, 2));
    nsubs++;
  }

  /* --- apply(f, x) = #call_indirect(f, x) --- */
  {
    L1Param params[2];
    L1Operand call_ops[2], ret[1];
    const L1Inst *insts[2];
    const L1Type *rtypes[1];
    params[0] = lainir_proc_param("%f", addr);
    params[1] = lainir_proc_param("%x", u64);
    call_ops[0] = lainir_ref("%f");
    call_ops[1] = lainir_ref("%x");
    insts[0] = lainir_inst(b, INST_CALL_INDIRECT, "%r", u64, call_ops, 2);
    ret[0] = lainir_ref("%r");
    insts[1] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    rtypes[0] = u64;
    subs[nsubs] = *lainir_subroutine(b, "apply", params, 2, rtypes, 1,
                                     lainir_region(b, NULL, 0, NULL, 0, insts, 2));
    nsubs++;
  }

  /* --- via_ptr() = apply(#proc_addr inc, 41) --- */
  {
    L1Operand ret[1], call_ops[2];
    const L1Inst *insts[3];
    const L1Type *rtypes[1];
    insts[0] = lainir_inst_symbol(b, INST_PROC_ADDR, "%f", "inc");
    call_ops[0] = lainir_ref("%f");
    call_ops[1] = lainir_int(41);
    insts[1] = lainir_inst_call(b, "%v", "apply", call_ops, 2);
    ret[0] = lainir_ref("%v");
    insts[2] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    rtypes[0] = u64;
    subs[nsubs] = *lainir_subroutine(b, "via_ptr", NULL, 0, rtypes, 1,
                                     lainir_region(b, NULL, 0, NULL, 0, insts, 3));
    nsubs++;
  }

  /* --- slt32()：32 位有符号比较，0xFFFFFFFF 是 -1 --- */
  {
    L1Operand cmp[2], ret[1], zx[1];
    const L1Inst *insts[3];
    const L1Type *rtypes[1];
    cmp[0] = lainir_int(0xFFFFFFFFu);
    cmp[1] = lainir_int(1);
    insts[0] = lainir_inst(b, INST_SLT, "%c", u32, cmp, 2);
    zx[0] = lainir_ref("%c");
    insts[1] = lainir_inst(b, INST_ZEXT, "%w", u64, zx, 1);
    ret[0] = lainir_ref("%w");
    insts[2] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    rtypes[0] = u64;
    subs[nsubs] = *lainir_subroutine(b, "slt32", NULL, 0, rtypes, 1,
                                     lainir_region(b, NULL, 0, NULL, 0, insts, 3));
    nsubs++;
  }

  /* --- alloca_store()：栈对象 + 写回 --- */
  {
    L1Operand ret[1], st[2], ld[1];
    const L1Inst *insts[4];
    const L1Type *rtypes[1];
    insts[0] = lainir_inst(b, INST_ALLOCA, "%slot", u64,
                           (L1Operand[]){lainir_int(1)}, 1);
    st[0] = lainir_int(5);
    st[1] = lainir_ref("%slot");
    insts[1] = lainir_inst_mem(b, INST_STORE, NULL, u64, ORDER_RELAXED, false, st, 2);
    ld[0] = lainir_ref("%slot");
    insts[2] = lainir_inst(b, INST_LOAD, "%v", u64, ld, 1);
    ret[0] = lainir_ref("%v");
    insts[3] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    rtypes[0] = u64;
    subs[nsubs] = *lainir_subroutine(
        b, "alloca_store", NULL, 0, rtypes, 1,
        lainir_region(b, NULL, 0, NULL, 0, insts, 4));
    nsubs++;
  }

  /* --- 能力：extern 子过程 + 调用方 --- */
  {
    L1Param p1[1];
    L1Param p2[2];
    L1Operand ret[1], call1[1], call2[2];
    const L1Inst *i2[2], *i3[3];
    const L1Type *r1[1];

    p1[0] = lainir_proc_param("%x", u64);
    r1[0] = u64;

    subs[nsubs] = *lainir_subroutine_extern(b, "host_add_100", "host_add_100",
                                            p1, 1, r1, 1);
    nsubs++;
    call1[0] = lainir_ref("%x");
    i2[0] = lainir_inst_call(b, "%r", "host_add_100", call1, 1);
    ret[0] = lainir_ref("%r");
    i2[1] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    subs[nsubs] = *lainir_subroutine(b, "host_caller", p1, 1, r1, 1,
                                     lainir_region(b, NULL, 0, NULL, 0, i2, 2));
    nsubs++;

    p2[0] = lainir_proc_param("%p", addr);
    p2[1] = lainir_proc_param("%n", u64);
    subs[nsubs] = *lainir_subroutine_extern(b, "host_sum_bytes",
                                            "host_sum_bytes", p2, 2, r1, 1);
    nsubs++;
    i3[0] = lainir_inst_symbol(b, INST_DATA_ADDR, "%p", "bytes");
    call2[0] = lainir_ref("%p");
    call2[1] = lainir_int(3);
    i3[1] = lainir_inst_call(b, "%r", "host_sum_bytes", call2, 2);
    ret[0] = lainir_ref("%r");
    i3[2] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    subs[nsubs] = *lainir_subroutine(b, "host_sum", NULL, 0, r1, 1,
                                     lainir_region(b, NULL, 0, NULL, 0, i3, 3));
    nsubs++;

    subs[nsubs] = *lainir_subroutine_extern(b, "host_refuse", "host_refuse",
                                            NULL, 0, r1, 1);
    nsubs++;
    call1[0] = lainir_int(0);
    i2[0] = lainir_inst_call(b, "%r", "host_refuse", NULL, 0);
    ret[0] = lainir_ref("%r");
    i2[1] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    subs[nsubs] = *lainir_subroutine(b, "host_refused", NULL, 0, r1, 1,
                                     lainir_region(b, NULL, 0, NULL, 0, i2, 2));
    nsubs++;

    /* 这个外部符号故意不登记能力：调用时应当被拒绝。 */
    subs[nsubs] = *lainir_subroutine_extern(b, "host_missing", "host_missing",
                                            NULL, 0, r1, 1);
    nsubs++;
    i2[0] = lainir_inst_call(b, "%r", "host_missing", NULL, 0);
    ret[0] = lainir_ref("%r");
    i2[1] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    subs[nsubs] = *lainir_subroutine(
        b, "host_missing_caller", NULL, 0, r1, 1,
        lainir_region(b, NULL, 0, NULL, 0, i2, 2));
    nsubs++;
  }

  /* --- classify(x)：#switch 也要和编译产物对拍 --- */
  {
    L1Param params[1];
    L1Operand sel[1], ret[1];
    const L1Inst *insts[2], *arm_i[1];
    const L1Type *rtypes[1];
    const L1Region *arm0, *arm1, *dflt;
    L1SwitchCase sw_cases[2];

    params[0] = lainir_proc_param("%x", u64);
    rtypes[0] = u64;

    arm_i[0] = lainir_inst(b, INST_YIELD, NULL, NULL,
                           (L1Operand[]){lainir_int(100)}, 1);
    arm0 = lainir_region(b, NULL, 0, rtypes, 1, arm_i, 1);
    arm_i[0] = lainir_inst(b, INST_YIELD, NULL, NULL,
                           (L1Operand[]){lainir_int(200)}, 1);
    arm1 = lainir_region(b, NULL, 0, rtypes, 1, arm_i, 1);
    arm_i[0] = lainir_inst(b, INST_YIELD, NULL, NULL,
                           (L1Operand[]){lainir_int(999)}, 1);
    dflt = lainir_region(b, NULL, 0, rtypes, 1, arm_i, 1);

    sw_cases[0].value = 0;
    sw_cases[0].body = arm0;
    sw_cases[1].value = 1;
    sw_cases[1].body = arm1;
    sel[0] = lainir_ref("%x");
    insts[0] = lainir_inst_switch(b, "%r", sel[0], u64, sw_cases, 2, dflt);
    ret[0] = lainir_ref("%r");
    insts[1] = lainir_inst(b, INST_RETURN, NULL, NULL, ret, 1);
    subs[nsubs] = *lainir_subroutine(b, "classify", params, 1, rtypes, 1,
                                     lainir_region(b, NULL, 0, NULL, 0, insts, 2));
    nsubs++;
  }

  data[0] = *lainir_data(b, "bytes", bytes, 3, false);
  /* --- 装载 + 跑 --- */
  lainvm_space_init(&space);
  {
    const L1Module *module = lainir_module(b, "diff", data, 1, subs, nsubs);
    /* 流水线：verify -> (fold) -> backend。先验，再装载。 */
    if (lainir_verify(module, &diag) != 0) {
      printf("verify failed: code=%d %s\n", diag.code, diag.message);
      return 1;
    }
    image = lainvm_image_load(module, &space, &diag);
    if (!image) {
      printf("load failed: %d %s\n", diag.code, diag.message);
      return 1;
    }

    /* 先把 C 后端的产物写出去 */
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
      g_out = fopen("build/tmp-probe/emitted.c", "wb");
      if (!g_out) {
        printf("cannot open emitted.c\n");
        return 1;
      }
      backend = lainbackend_new(&target, &sink, &diag);
      if (lainbackend_emit(backend, module) != 0) {
        printf("backend refused: code=%d %s\n", diag.code, diag.message);
        fclose(g_out);
        return 1;
      }
      lainbackend_free(backend);
      fclose(g_out);
    }
  }

  tcb = lainvm_tcb_new(image, &space, 1, 1, 64, 4096);
  if (!tcb) {
    printf("admit failed\n");
    return 1;
  }

  /* 能力：按外部符号名登记。host_missing 故意不登记。 */
  {
    LainVmCaps *caps = lainvm_caps_new();
    if (lainvm_caps_add(caps, "host_add_100", LAINVM_CAP_FUNCTION,
                        host_add_100) != 0 ||
        lainvm_caps_add(caps, "host_sum_bytes", LAINVM_CAP_FUNCTION,
                        host_sum_bytes) != 0 ||
        lainvm_caps_add(caps, "host_refuse", LAINVM_CAP_FUNCTION,
                        host_refuse) != 0) {
      printf("caps setup failed\n");
      return 1;
    }
    if (lainvm_tcb_set_caps(tcb, caps, &diag) != 0) {
      printf("caps resolve failed: %d %s\n", diag.code, diag.message);
      return 1;
    }
    printf("# caps=%u resolved=%u\n", lainvm_caps_count(caps),
           tcb->resolved_count);
  }

  {
    Case cases[24];
    uint32_t count = 0;
    uint32_t i;
    cases[count].name = "answer"; cases[count].sub = SUB_ANSWER; cases[count].nargs = 0; count++;
    cases[count].name = "max(3,9)"; cases[count].sub = SUB_MAX; cases[count].nargs = 2;
    cases[count].args[0] = 3; cases[count].args[1] = 9; count++;
    cases[count].name = "max(9,3)"; cases[count].sub = SUB_MAX; cases[count].nargs = 2;
    cases[count].args[0] = 9; cases[count].args[1] = 3; count++;
    cases[count].name = "fact(5)"; cases[count].sub = SUB_FACT; cases[count].nargs = 1;
    cases[count].args[0] = 5; count++;
    cases[count].name = "fact(0)"; cases[count].sub = SUB_FACT; cases[count].nargs = 1;
    cases[count].args[0] = 0; count++;
    cases[count].name = "sum_bytes(3)"; cases[count].sub = SUB_SUM_BYTES; cases[count].nargs = 2;
    cases[count].args[0] = (uint64_t)image->symbols[0].addr; cases[count].args[1] = 3; count++;
    cases[count].name = "sum_bytes(0)"; cases[count].sub = SUB_SUM_BYTES; cases[count].nargs = 2;
    cases[count].args[0] = (uint64_t)image->symbols[0].addr; cases[count].args[1] = 0; count++;
    cases[count].name = "first_byte"; cases[count].sub = SUB_FIRST_BYTE; cases[count].nargs = 0; count++;
    cases[count].name = "via_ptr"; cases[count].sub = SUB_VIA_PTR; cases[count].nargs = 0; count++;
    cases[count].name = "slt32"; cases[count].sub = SUB_SLT32; cases[count].nargs = 0; count++;
    cases[count].name = "alloca_store"; cases[count].sub = SUB_ALLOCA_STORE; cases[count].nargs = 0; count++;
    cases[count].name = "host_caller(5)"; cases[count].sub = SUB_HOST_CALLER; cases[count].nargs = 1;
    cases[count].args[0] = 5; count++;
    cases[count].name = "host_sum()"; cases[count].sub = SUB_HOST_SUM; cases[count].nargs = 0; count++;
    cases[count].name = "classify(0)"; cases[count].sub = SUB_CLASSIFY; cases[count].nargs = 1;
    cases[count].args[0] = 0; count++;
    cases[count].name = "classify(1)"; cases[count].sub = SUB_CLASSIFY; cases[count].nargs = 1;
    cases[count].args[0] = 1; count++;
    cases[count].name = "classify(9)"; cases[count].sub = SUB_CLASSIFY; cases[count].nargs = 1;
    cases[count].args[0] = 9; count++;

    printf("# VM\n");
    for (i = 0; i < count; i++) {
      L1Value args[2];
      L1Value value;
      LainVmSliceResult result;
      uint32_t k;
      memset(args, 0, sizeof(args));
      for (k = 0; k < cases[i].nargs; k++)
        args[k] = (L1Value){L1_VALUE_BITS, 64, {.bits = cases[i].args[k]}};
      if (lainvm_tcb_start(tcb, image->subs[cases[i].sub].sub->name, args,
                           cases[i].nargs, &diag) != 0) {
        printf("%s = start-failed(%d)\n", cases[i].name, diag.code);
        continue;
      }
      result = lainvm_engine_run(tcb, 1000000);
      value = tcb->result;
      if (result != LAINVM_SLICE_DONE) {
        printf("%s = trapped(%d)\n", cases[i].name, tcb->trap.status);
        continue;
      }
      printf("%s = %llu\n", cases[i].name, (unsigned long long)value.as.bits);
    }

    /* 只在 VM 这一侧测的负例：编译产物里没有能力异常这条路（待定项），
     * 宿主拒绝只能 abort，没法对拍。行首加 # 让对拍跳过。 */
    printf("# VM-only\n");
    {
      struct { const char *name; uint32_t sub; } negatives[2];
      uint32_t n;
      negatives[0].name = "host_refused"; negatives[0].sub = SUB_HOST_REFUSED;
      negatives[1].name = "host_missing_caller";
      negatives[1].sub = SUB_HOST_MISSING_CALLER;
      for (n = 0; n < 2; n++) {
        L1Diagnostic d2;
        LainVmSliceResult r2;
        d2.code = 0;
        if (lainvm_tcb_start(tcb, image->subs[negatives[n].sub].sub->name,
                             NULL, 0, &d2) != 0) {
          printf("# %s -> start-failed(%d)\n", negatives[n].name, d2.code);
          continue;
        }
        r2 = lainvm_engine_run(tcb, 1000000);
        if (r2 == LAINVM_SLICE_TRAPPED)
          printf("# %s -> trapped kind=%d status=%d\n", negatives[n].name,
                 (int)tcb->trap.kind, tcb->trap.status);
        else
          printf("# %s -> slice=%d (expected a trap)\n", negatives[n].name,
                 (int)r2);
      }
    }
  }

  lainvm_tcb_free(tcb);
  lainvm_image_free(image);
  lainir_builder_free(b);
  return 0;
}
