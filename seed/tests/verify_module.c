/* 切片 3 的验收：验证器。
 *
 * 文档 §11 的 12 条规则，每条一个负例必须被拒；另外补上后端/引擎实际依赖
 * 的几条（宽度可搬运、alloca 常量、符号存在、过程体不声明结果……）。
 *
 * 第 9 条「操作数不得是嵌套操作」在数据模型里**不可能违反**：`L1Operand`
 * 只能是值或字面量，没有嵌套这个形状。所以它没有负例，由模型本身保证。
 */
#include <stdio.h>
#include <string.h>

#include "lainir/build.h"
#include "lainir/verify.h"

static int failures = 0;

static void expect(const char *name, const L1Module *module, int want_code) {
  L1Diagnostic diag;
  int rc;
  diag.code = 0;
  rc = lainir_verify(module, &diag);
  if (want_code == 0) {
    if (rc == 0) {
      printf("ok    %-28s 通过\n", name);
    } else {
      printf("FAIL  %-28s 期望通过，得到 %d %s\n", name, diag.code, diag.message);
      failures++;
    }
    return;
  }
  if (rc == 0) {
    printf("FAIL  %-28s 期望被拒（%d），却通过了\n", name, want_code);
    failures++;
    return;
  }
  if (diag.code != want_code) {
    printf("FAIL  %-28s 期望 %d，得到 %d %s\n", name, want_code, diag.code,
           diag.message);
    failures++;
    return;
  }
  printf("ok    %-28s 被拒 %d：%s\n", name, diag.code, diag.message);
}

/* --- 小工具 --------------------------------------------------------------- */

typedef struct {
  L1Builder *b;
  const L1Type *u64;
  const L1Type *u32;
  const L1Type *u8;
  const L1Type *u12;
  const L1Type *addr;
} Fx;

static const L1Module *module_of(Fx *fx, const L1Region *body,
                                 const L1Param *params, uint32_t nparams,
                                 const L1Type *const *results,
                                 uint32_t nresults) {
  L1Subroutine subs[4];
  uint32_t n = 0;
  const L1Subroutine *sub;
  if (results) {
    sub = lainir_subroutine(fx->b, "main", params, nparams, results, nresults,
                            body);
  } else {
    sub = lainir_subroutine(fx->b, "main", params, nparams, NULL, 0, body);
  }
  subs[n++] = *sub;
  return lainir_module(fx->b, "probe", NULL, 0, subs, n);
}

/* 一个 (u64) -> u64 的过程，体是给定指令。 */
static const L1Module *simple(Fx *fx, const L1Inst *const *insts, uint32_t count) {
  L1Param p[1];
  const L1Type *r[1];
  p[0] = lainir_proc_param("%n", fx->u64);
  r[0] = fx->u64;
  return module_of(fx, lainir_region(fx->b, NULL, 0, NULL, 0, insts, count), p, 1,
                   r, 1);
}

/* --- 正例 ----------------------------------------------------------------- */

static const L1Module *good_module(Fx *fx) {
  const L1Inst *insts[4];
  L1Param p[1];
  const L1Type *r[1];
  const L1Inst *then_i[1];
  const L1Inst *else_i[2];
  const L1Region *then_r;
  const L1Region *else_r;
  L1Operand cmp[2], cond[1], ret[1], ya[1], yb[1], sub_ops[2];
  L1Subroutine subs[2];
  const L1Subroutine *f;
  L1Type *dummy = NULL;

  (void)dummy;
  p[0] = lainir_proc_param("%n", fx->u64);
  r[0] = fx->u64;

  /* #if 的两条分支都交值 */
  cmp[0] = lainir_ref("%n");
  cmp[1] = lainir_int(1);
  insts[0] = lainir_inst(fx->b, INST_SGT, "%c", fx->u64, cmp, 2);
  ya[0] = lainir_ref("%n");
  then_i[0] = lainir_inst(fx->b, INST_YIELD, NULL, NULL, ya, 1);
  sub_ops[0] = lainir_ref("%n");
  sub_ops[1] = lainir_int(1);
  else_i[0] = lainir_inst(fx->b, INST_SUB, "%m", fx->u64, sub_ops, 2);
  yb[0] = lainir_ref("%m");
  else_i[1] = lainir_inst(fx->b, INST_YIELD, NULL, NULL, yb, 1);
  then_r = lainir_region(fx->b, NULL, 0, r, 1, then_i, 1);
  else_r = lainir_region(fx->b, NULL, 0, r, 1, else_i, 2);
  cond[0] = lainir_ref("%c");
  insts[1] = lainir_inst_if(fx->b, "%r", cond[0], then_r, else_r);
  ret[0] = lainir_ref("%r");
  insts[2] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  f = lainir_subroutine(fx->b, "dec", p, 1, r, 1,
                        lainir_region(fx->b, NULL, 0, NULL, 0, insts, 3));
  subs[0] = *f;

  /* 调用它 */
  {
    const L1Inst *caller_insts[2];
    L1Operand call_ops[1], cret[1];
    call_ops[0] = lainir_ref("%n");
    caller_insts[0] = lainir_inst_call(fx->b, "%v", "dec", call_ops, 1);
    cret[0] = lainir_ref("%v");
    caller_insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, cret, 1);
    subs[1] = *lainir_subroutine(fx->b, "caller", p, 1, r, 1,
                                 lainir_region(fx->b, NULL, 0, NULL, 0,
                                               caller_insts, 2));
  }
  return lainir_module(fx->b, "good", NULL, 0, subs, 2);
}

/* --- 负例 ----------------------------------------------------------------- */

static const L1Module *case_duplicate_binding(Fx *fx) {
  const L1Inst *insts[3];
  L1Operand a[2], ret[1];
  a[0] = lainir_int(1);
  a[1] = lainir_int(2);
  insts[0] = lainir_inst(fx->b, INST_ADD, "%r", fx->u64, a, 2);
  insts[1] = lainir_inst(fx->b, INST_ADD, "%r", fx->u64, a, 2);
  ret[0] = lainir_ref("%r");
  insts[2] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  return simple(fx, insts, 3);
}

static const L1Module *case_undefined_value(Fx *fx) {
  const L1Inst *insts[2];
  L1Operand a[2], ret[1];
  a[0] = lainir_ref("%n");
  a[1] = lainir_ref("%missing");
  insts[0] = lainir_inst(fx->b, INST_ADD, "%r", fx->u64, a, 2);
  ret[0] = lainir_ref("%r");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  return simple(fx, insts, 2);
}

static const L1Module *case_after_terminator(Fx *fx) {
  const L1Inst *insts[3];
  L1Operand a[2], ret[1];
  a[0] = lainir_ref("%n");
  a[1] = lainir_int(1);
  insts[0] = lainir_inst(fx->b, INST_ADD, "%r", fx->u64, a, 2);
  ret[0] = lainir_ref("%r");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  insts[2] = lainir_inst(fx->b, INST_ADD, "%q", fx->u64, a, 2);
  return simple(fx, insts, 3);
}

static const L1Module *case_literal_without_type(Fx *fx) {
  const L1Inst *insts[2];
  L1Operand a[2], ret[1];
  a[0] = lainir_int(1);
  a[1] = lainir_int(2);
  insts[0] = lainir_inst(fx->b, INST_ADD, "%r", NULL, a, 2); /* 没有类型实参 */
  ret[0] = lainir_ref("%r");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  return simple(fx, insts, 2);
}

static const L1Module *case_bad_operand_type(Fx *fx) {
  const L1Inst *insts[2];
  L1Operand a[2], ret[1];
  L1Param p[1];
  a[0] = lainir_ref("%p");
  a[1] = lainir_int(1);
  insts[0] = lainir_inst(fx->b, INST_ADD, "%r", fx->u64, a, 2);
  ret[0] = lainir_ref("%r");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  p[0] = lainir_proc_param("%p", fx->addr);
  {
    const L1Type *r[1];
    r[0] = fx->u64;
    return module_of(fx, lainir_region(fx->b, NULL, 0, NULL, 0, insts, 2), p, 1,
                     r, 1);
  }
}

static const L1Module *case_missing_yield(Fx *fx) {
  const L1Inst *insts[3];
  const L1Inst *then_i[1];
  L1Operand cmp[2], cond[1], ret[1], inner[2];
  const L1Type *r[1];
  const L1Region *then_r_late;

  r[0] = fx->u64;
  cmp[0] = lainir_ref("%n");
  cmp[1] = lainir_int(0);
  insts[0] = lainir_inst(fx->b, INST_SGT, "%c", fx->u64, cmp, 2);
  /* then 区域声明了结果，但末尾不是 #yield */
  inner[0] = lainir_ref("%n");
  inner[1] = lainir_int(1);
  then_i[0] = lainir_inst(fx->b, INST_ADD, "%x", fx->u64, inner, 2);
  then_r_late = lainir_region(fx->b, NULL, 0, r, 1, then_i, 1);
  cond[0] = lainir_ref("%c");
  insts[1] = lainir_inst_if(fx->b, "%r", cond[0], then_r_late, then_r_late);
  ret[0] = lainir_ref("%r");
  insts[2] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  return simple(fx, insts, 3);
}

static const L1Module *case_bad_jump_arity(Fx *fx) {
  L1RegionParam lp[1];
  L1Operand cont[1], ret[1];
  const L1Inst *body[3];
  const L1Type *r[1];
  const L1Region *loop_body;
  const L1Inst *insts[2];
  L1Type *u64 = (L1Type *)fx->u64;

  (void)u64;
  r[0] = fx->u64;
  lp[0] = lainir_param("%i", fx->u64, lainir_int(0));
  body[0] = lainir_inst_jump(fx->b, INST_BREAK, "L", NULL, 0);
  body[1] = lainir_inst_jump(fx->b, INST_CONTINUE, "L", cont, 0);
  cont[0] = lainir_ref("%i");
  loop_body = lainir_region(fx->b, lp, 1, r, 1, body, 2);
  insts[0] = lainir_inst_loop(fx->b, "%v", "L", loop_body);
  ret[0] = lainir_ref("%v");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  return simple(fx, insts, 2);
}

static const L1Module *case_missing_type(Fx *fx) {
  const L1Inst *insts[2];
  L1Operand ld[1], ret[1];
  L1Param p[1];
  const L1Type *r[1];
  ld[0] = lainir_ref("%p");
  insts[0] = lainir_inst(fx->b, INST_LOAD, "%b", NULL, ld, 1); /* 缺类型实参 */
  ret[0] = lainir_ref("%b");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  p[0] = lainir_proc_param("%p", fx->addr);
  r[0] = fx->u64;
  return module_of(fx, lainir_region(fx->b, NULL, 0, NULL, 0, insts, 2), p, 1, r,
                   1);
}

static const L1Module *case_bad_condition(Fx *fx) {
  const L1Inst *insts[2];
  const L1Inst *then_i[1];
  L1Operand cond[1], ret[1], y[1];
  const L1Type *r[1];
  const L1Region *then_r_late;

  r[0] = fx->u64;
  cond[0] = lainir_ref("%n"); /* #bits<64>，不是 #bits<1> */
  y[0] = lainir_ref("%n");
  then_i[0] = lainir_inst(fx->b, INST_YIELD, NULL, NULL, y, 1);
  then_r_late = lainir_region(fx->b, NULL, 0, r, 1, then_i, 1);
  insts[0] = lainir_inst_if(fx->b, "%r", cond[0], then_r_late, then_r_late);
  ret[0] = lainir_ref("%r");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  return simple(fx, insts, 2);
}

static const L1Module *case_duplicate_case(Fx *fx) {
  static L1SwitchCase cases[2];
  const L1Inst *insts[2];
  L1Operand sel[1], ret[1];
  const L1Inst *arm[1];
  const L1Type *r[1];
  L1Region *arm_r;
  L1Type *u64 = (L1Type *)fx->u64;

  r[0] = fx->u64;
  arm[0] = lainir_inst(fx->b, INST_YIELD, NULL, NULL, (L1Operand[]){lainir_int(1)}, 1);
  arm_r = (L1Region *)lainir_region(fx->b, NULL, 0, r, 1, arm, 1);
  cases[0].value = 7;
  cases[0].body = arm_r;
  cases[1].value = 7;
  cases[1].body = arm_r;
  sel[0] = lainir_ref("%n");
  insts[0] = lainir_inst(fx->b, INST_SWITCH, "%r", u64, sel, 1);
  ((L1Inst *)insts[0])->cases = cases;
  ((L1Inst *)insts[0])->case_count = 2;
  ret[0] = lainir_ref("%r");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  return simple(fx, insts, 2);
}

static const L1Module *case_bad_call_arity(Fx *fx) {
  const L1Inst *insts[3];
  L1Operand call[2], ret[1];
  L1Subroutine subs[2];
  L1Param p[1];
  const L1Type *r[1];

  p[0] = lainir_proc_param("%x", fx->u64);
  r[0] = fx->u64;
  {
    const L1Inst *one[1];
    one[0] = lainir_inst(fx->b, INST_RETURN, NULL, NULL,
                         (L1Operand[]){lainir_ref("%x")}, 1);
    subs[0] = *lainir_subroutine(fx->b, "one", p, 1, r, 1,
                                 lainir_region(fx->b, NULL, 0, NULL, 0, one, 1));
  }
  call[0] = lainir_ref("%x");
  call[1] = lainir_int(1);
  insts[0] = lainir_inst_call(fx->b, "%v", "one", call, 2); /* 多给一个 */
  ret[0] = lainir_ref("%v");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  subs[1] = *lainir_subroutine(fx->b, "main", p, 1, r, 1,
                               lainir_region(fx->b, NULL, 0, NULL, 0, insts, 2));
  return lainir_module(fx->b, "arity", NULL, 0, subs, 2);
}

static const L1Module *case_eval_not_const(Fx *fx) {
  const L1Inst *insts[2];
  L1Operand call[1], ret[1];
  L1Subroutine subs[2];
  L1Param p[1];
  const L1Type *r[1];

  p[0] = lainir_proc_param("%x", fx->u64);
  r[0] = fx->u64;
  {
    const L1Inst *one[1];
    one[0] = lainir_inst(fx->b, INST_RETURN, NULL, NULL,
                         (L1Operand[]){lainir_ref("%x")}, 1);
    subs[0] = *lainir_subroutine(fx->b, "id", p, 1, r, 1,
                                 lainir_region(fx->b, NULL, 0, NULL, 0, one, 1));
  }
  call[0] = lainir_ref("%x"); /* 运行期的实参 */
  insts[0] = lainir_inst_call(fx->b, "%v", "id", call, 1);
  ((L1Inst *)insts[0])->is_eval = true;
  ret[0] = lainir_ref("%v");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  subs[1] = *lainir_subroutine(fx->b, "main", p, 1, r, 1,
                               lainir_region(fx->b, NULL, 0, NULL, 0, insts, 2));
  return lainir_module(fx->b, "phase", NULL, 0, subs, 2);
}

static const L1Module *case_bad_memory_width(Fx *fx) {
  const L1Inst *insts[2];
  L1Operand ld[1], ret[1];
  L1Param p[1];
  const L1Type *r[1];
  ld[0] = lainir_ref("%p");
  insts[0] = lainir_inst(fx->b, INST_LOAD, "%b", fx->u12, ld, 1); /* 12 位 */
  ret[0] = lainir_ref("%b");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  p[0] = lainir_proc_param("%p", fx->addr);
  r[0] = fx->u64;
  return module_of(fx, lainir_region(fx->b, NULL, 0, NULL, 0, insts, 2), p, 1, r,
                   1);
}

static const L1Module *case_alloca_not_const(Fx *fx) {
  const L1Inst *insts[2];
  L1Operand count[1], ret[1];
  count[0] = lainir_ref("%n");
  insts[0] = lainir_inst(fx->b, INST_ALLOCA, "%s", fx->u64, count, 1);
  ret[0] = lainir_ref("%n");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  return simple(fx, insts, 2);
}

static const L1Module *case_unknown_callee(Fx *fx) {
  const L1Inst *insts[2];
  L1Operand ret[1];
  insts[0] = lainir_inst_call(fx->b, "%v", "nosuch", NULL, 0);
  ret[0] = lainir_ref("%v");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  return simple(fx, insts, 2);
}

static const L1Module *case_unknown_proc(Fx *fx) {
  const L1Inst *insts[2];
  L1Operand ret[1];
  ret[0] = lainir_int(0);
  insts[0] = lainir_inst_symbol(fx->b, INST_PROC_ADDR, "%f", "nosuch");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  return simple(fx, insts, 2);
}

static const L1Module *case_unknown_symbol(Fx *fx) {
  const L1Inst *insts[2];
  L1Operand ret[1];
  ret[0] = lainir_int(0);
  insts[0] = lainir_inst_symbol(fx->b, INST_DATA_ADDR, "%p", "nosuch");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  return simple(fx, insts, 2);
}

static const L1Module *case_unknown_loop(Fx *fx) {
  const L1Inst *insts[2];
  L1Operand ret[1];
  ret[0] = lainir_int(0);
  insts[0] = lainir_inst_jump(fx->b, INST_CONTINUE, "noloop", NULL, 0);
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  return simple(fx, insts, 2);
}

static const L1Module *case_bad_return(Fx *fx) {
  const L1Inst *insts[1];
  insts[0] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, NULL, 0); /* 没有值 */
  return simple(fx, insts, 1);
}

static const L1Module *case_bad_result_count(Fx *fx) {
  const L1Inst *insts[2];
  L1Operand a[2], ret[1];
  a[0] = lainir_ref("%n");
  a[1] = lainir_int(1);
  insts[0] = lainir_inst(fx->b, INST_ADD, NULL, fx->u64, a, 2); /* 没有结果 */
  ret[0] = lainir_ref("%n");
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  return simple(fx, insts, 2);
}

static const L1Module *case_body_declares_results(Fx *fx) {
  const L1Inst *insts[1];
  L1Param p[1];
  const L1Type *r[1];
  const L1Region *body;
  insts[0] = lainir_inst(fx->b, INST_RETURN, NULL, NULL,
                         (L1Operand[]){lainir_ref("%n")}, 1);
  r[0] = fx->u64;
  body = lainir_region(fx->b, NULL, 0, r, 1, insts, 1); /* 体声明了结果 */
  p[0] = lainir_proc_param("%n", fx->u64);
  {
    L1Subroutine subs[1];
    subs[0] = *lainir_subroutine(fx->b, "main", p, 1, r, 1, body);
    return lainir_module(fx->b, "bodyresult", NULL, 0, subs, 1);
  }
}

static const L1Module *case_switch_unsupported(Fx *fx) {
  const L1Inst *insts[2];
  L1Operand sel[1], ret[1];
  ret[0] = lainir_int(0);
  sel[0] = lainir_ref("%n");
  insts[0] = lainir_inst(fx->b, INST_SWITCH, "%r", (L1Type *)fx->u64, sel, 1);
  insts[1] = lainir_inst(fx->b, INST_RETURN, NULL, NULL, ret, 1);
  return simple(fx, insts, 2);
}

typedef const L1Module *(*CaseFn)(Fx *fx);
typedef struct {
  const char *name;
  CaseFn build;
  int want_code;
} Case;

int main(void) {
  static const Case cases[] = {
      {"重复定义 §11.1", case_duplicate_binding, L1V_DUPLICATE_BINDING},
      {"未定义的值 §11.2", case_undefined_value, L1V_UNDEFINED_VALUE},
      {"终结子后有指令 §11.3", case_after_terminator, L1V_AFTER_TERMINATOR},
      {"字面量没有类型 §11.4", case_literal_without_type, L1V_NO_TYPE_FOR_LITERAL},
      {"操作数类型不符 §11.5", case_bad_operand_type, L1V_BAD_OPERAND_TYPE},
      {"区域没交值 §11.6", case_missing_yield, L1V_MISSING_YIELD},
      {"跳转元数不符 §11.7", case_bad_jump_arity, L1V_BAD_JUMP_ARITY},
      {"搬运缺类型 §11.8", case_missing_type, L1V_MISSING_TYPE},
      {"条件不是 bits<1> §11.10", case_bad_condition, L1V_BAD_CONDITION},
      {"case 值重复 §11.10", case_duplicate_case, L1V_DUPLICATE_CASE},
      {"调用元数不符 §11.11", case_bad_call_arity, L1V_BAD_CALL_ARITY},
      {"eval 实参非常量 §11.12", case_eval_not_const, L1V_EVAL_ARG_NOT_CONST},
      {"内存宽度不可搬运", case_bad_memory_width, L1V_BAD_MEMORY_WIDTH},
      {"alloca 数量非常量", case_alloca_not_const, L1V_ALLOCA_NOT_CONST},
      {"调用不存在的子过程", case_unknown_callee, L1V_UNKNOWN_CALLEE},
      {"proc_addr 符号不存在", case_unknown_proc, L1V_UNKNOWN_PROC},
      {"data_addr 符号不存在", case_unknown_symbol, L1V_UNKNOWN_SYMBOL},
      {"跳转到不存在的循环", case_unknown_loop, L1V_UNKNOWN_LOOP},
      {"返回与签名不符", case_bad_return, L1V_BAD_RETURN},
      {"结果个数不对", case_bad_result_count, L1V_BAD_RESULT_COUNT},
      {"过程体声明了区域结果", case_body_declares_results,
       L1V_BODY_DECLARES_RESULTS},
      {"switch 还不支持", case_switch_unsupported, L1V_SWITCH_UNSUPPORTED},
  };
  Fx fx;
  uint32_t i;

  setvbuf(stdout, NULL, _IONBF, 0);
  memset(&fx, 0, sizeof(fx));
  fx.b = lainir_builder_new();
  fx.u64 = lainir_type(fx.b, TY_BITS, 64);
  fx.u32 = lainir_type(fx.b, TY_BITS, 32);
  fx.u8 = lainir_type(fx.b, TY_BITS, 8);
  fx.u12 = lainir_type(fx.b, TY_BITS, 12);
  fx.addr = lainir_type(fx.b, TY_ADDR, 0);

  expect("正例：if/call 都合法", good_module(&fx), 0);
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    L1Builder *b = lainir_builder_new();
    Fx local;
    memset(&local, 0, sizeof(local));
    local.b = b;
    local.u64 = lainir_type(b, TY_BITS, 64);
    local.u32 = lainir_type(b, TY_BITS, 32);
    local.u8 = lainir_type(b, TY_BITS, 8);
    local.u12 = lainir_type(b, TY_BITS, 12);
    local.addr = lainir_type(b, TY_ADDR, 0);
    expect(cases[i].name, cases[i].build(&local), cases[i].want_code);
    lainir_builder_free(b);
  }

  printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
  lainir_builder_free(fx.b);
  return failures == 0 ? 0 : 1;
}
