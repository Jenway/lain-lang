/* lainfold/fold.h 的实现。 */
#include "lainfold/fold.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lainvm/engine.h"
#include "lainvm/image.h"

#define FOLD_MAX_VALUES 256u
#define FOLD_MAX_ARGS 8u
/* 一个模块里能被 lowering 成临时根过程的 `#eval` 块数上限。这是**折叠阶段**的
 * 容量，不是语言限制（「每模块块数」那条语言级预算见计划 S6）。 */
#define FOLD_MAX_BLOCKS 64u

typedef struct {
  const char *name;
  uint64_t bits;
  uint32_t width;
} FoldedValue;

/* 一个 `#eval` 块 lowering 出来的临时根过程。名字是 malloc 的：image 持有指向
 * 这些子过程的指针，所以它们必须活到 image 释放为止。 */
typedef struct {
  const L1Inst *block;
  const L1Subroutine *sub;
  char *name;
} FoldedBlock;

struct LainFold {
  LainVmCaps *caps;
  uint32_t max_call_depth;
  uint64_t stack_bytes;
  uint64_t fuel;
  /* 这次编译期执行的**分配账户**：一次最外层执行一个账户，嵌套调用共享它。
   * limit = 0 = 不限额（默认，行为跟从前一样）。栈按整块容量在 admit 时预扣，
   * 归还发生在 TCB 销毁时。 */
  LainVmQuota quota;

  /* 一次 lainfold_module 期间的状态 */
  L1Builder *builder;
  const L1Module *module;
  L1Diagnostic *diag;
  LainVmSpace space;
  LainVmImage *image;
  LainVmTcb *tcb;
  FoldedValue values[FOLD_MAX_VALUES];
  uint32_t value_count;
  uint32_t folded;
  bool failed;

  /* `#eval` 块 lowering 的结果（见 lower_blocks）。run_module 是**装载 image 用的**
   * 那一份模块：它比输出多那几个临时根过程。输出永远是原模块的子过程集合。 */
  FoldedBlock blocks[FOLD_MAX_BLOCKS];
  uint32_t block_count;
  const L1Module *run_module;
  L1Subroutine *run_subs;
  uint32_t run_sub_count;
  /* 第一遍（折调用形态）的产物：第二遍的输入，也是 image 那份模块的来源。
   * 不是 builder 分配的，所以要自己放（release_blocks 负责）。 */
  L1Subroutine *phase1_subs;
};

static bool fold_fail(LainFold *f, int code, const char *message) {
  if (f->failed) return false;
  f->failed = true;
  if (f->diag) {
    f->diag->code = code;
    f->diag->line = 0;
    f->diag->column = 0;
    snprintf(f->diag->message, sizeof(f->diag->message), "%s", message);
  }
  return false;
}

LainFold *lainfold_new(LainVmCaps *caps, uint32_t max_call_depth,
                       uint64_t stack_bytes, uint64_t fuel) {
  LainFold *fold = (LainFold *)calloc(1, sizeof(LainFold));
  if (!fold) return NULL;
  fold->caps = caps;
  fold->max_call_depth = max_call_depth ? max_call_depth : 64;
  fold->stack_bytes = stack_bytes;
  fold->fuel = fuel ? fuel : 1000000;
  lainvm_quota_init(&fold->quota, 0); /* 默认不限额 */
  return fold;
}

/* 设这次编译期执行的分配预算（字节）。0 = 不限额。要在第一次
 * lainfold_module 之前调用：账户是**执行**级的，不随模块重置。 */
void lainfold_set_quota_limit(LainFold *fold, uint64_t limit_bytes) {
  if (fold) lainvm_quota_init(&fold->quota, limit_bytes);
}

/* 账户只读快照，给驱动与验收用。 */
void lainfold_quota(const LainFold *fold, LainVmQuota *out) {
  if (!fold || !out) return;
  *out = fold->quota;
}

void lainfold_free(LainFold *fold) { free(fold); }

uint32_t lainfold_folded_count(const LainFold *fold) {
  return fold ? fold->folded : 0;
}

/* --- 已折叠的值 ----------------------------------------------------------- */

static void remember(LainFold *f, const char *name, uint64_t bits,
                     uint32_t width) {
  if (!name || f->value_count >= FOLD_MAX_VALUES) return;
  f->values[f->value_count].name = name;
  f->values[f->value_count].bits = bits;
  f->values[f->value_count].width = width;
  f->value_count++;
}

static bool lookup(const LainFold *f, const char *name, uint64_t *bits_out) {
  uint32_t i;
  if (!name) return false;
  for (i = 0; i < f->value_count; i++) {
    if (strcmp(f->values[i].name, name) == 0) {
      *bits_out = f->values[i].bits;
      return true;
    }
  }
  return false;
}

/* 这个名字是块**自己**绑的吗（区域参数，或某条指令的结果）？ */
static bool bound_in_region(const L1Region *region, const char *name) {
  uint32_t i, k;
  if (!region || !name) return false;
  for (i = 0; i < region->param_count; i++)
    if (region->params[i].param.name &&
        strcmp(region->params[i].param.name, name) == 0)
      return true;
  for (i = 0; i < region->inst_count; i++) {
    const L1Inst *inst = &region->insts[i];
    for (k = 0; k < inst->result_count; k++)
      if (inst->results[k] && strcmp(inst->results[k], name) == 0) return true;
    if (inst->kind == INST_SWITCH) {
      for (k = 0; k < inst->case_count; k++)
        if (bound_in_region(inst->cases[k].body, name)) return true;
      if (bound_in_region(inst->default_case, name)) return true;
      continue;
    }
    if (bound_in_region(inst->body, name)) return true;
    if (bound_in_region(inst->else_body, name)) return true;
  }
  return false;
}

/* 换一个操作数：编译期已知的值换成字面量。
 *
 * `block_root` 非 NULL = 正在一个 `#eval` **块体**里（含它的嵌套区域）：块自己绑的名字
 * 不动（块内优先，外面的同名值不许串进来）。
 * `reject_unknown` = 正在做按值捕获，期间遇到没有已知值的外围名字就拒 **9323**：
 * 块必须在 backend 之前消失，读不到运行期的值；而且块是以临时根过程执行的，
 * 跨帧去读调用者那一帧会读到根本不存在的 frame。 */
static L1Operand rewrite_operand(LainFold *f, const L1Region *block_root,
                                 bool reject_unknown, L1Operand op,
                                 bool *changed) {
  uint64_t bits = 0;
  if (op.kind != OPERAND_VALUE || !op.name) return op;
  if (block_root && bound_in_region(block_root, op.name)) return op;
  if (lookup(f, op.name, &bits)) {
    *changed = true;
    return lainir_int(bits);
  }
  if (reject_unknown) {
    char message[160];
    snprintf(message, sizeof(message),
             "fold: the #eval block reads `%s`, which is not compile-time known",
             op.name);
    fold_fail(f, 9323, message);
  }
  return op;
}

static const L1Operand *rewrite_operands(LainFold *f, const L1Inst *inst,
                                         const L1Region *block_root,
                                         bool reject_unknown, bool *changed) {
  L1Operand *out;
  uint32_t i;
  if (inst->operand_count == 0) return NULL;
  out = (L1Operand *)malloc(sizeof(L1Operand) * inst->operand_count);
  if (!out) {
    fold_fail(f, 9300, "fold: out of memory");
    return NULL;
  }
  for (i = 0; i < inst->operand_count; i++)
    out[i] = rewrite_operand(f, block_root, reject_unknown, inst->operands[i],
                             changed);
  return out;
}

/* --- 跑一次编译期调用 ----------------------------------------------------- */

static const L1Subroutine *find_sub(const LainVmImage *image, const char *name,
                                    uint32_t *index_out) {
  uint32_t i;
  for (i = 0; i < image->sub_count; i++) {
    if (image->subs[i].sub->name && name &&
        strcmp(image->subs[i].sub->name, name) == 0) {
      if (index_out) *index_out = i;
      return image->subs[i].sub;
    }
  }
  return NULL;
}

static bool run_eval(LainFold *f, const L1Inst *inst,
                     const L1Operand *operands, uint64_t *bits_out,
                     uint32_t *width_out) {
  const L1Subroutine *callee;
  uint32_t sub_index = 0;
  L1Value args[FOLD_MAX_ARGS];
  L1Value value;
  LainVmSliceResult result;
  uint32_t i;

  if (!inst->symbol) return fold_fail(f, 9301, "fold: #eval without a callee");
  callee = find_sub(f->image, inst->symbol, &sub_index);
  if (!callee) return fold_fail(f, 9302, "fold: #eval callee is not in the module");
  if (inst->operand_count > FOLD_MAX_ARGS)
    return fold_fail(f, 9303, "fold: too many arguments in #eval");

  for (i = 0; i < inst->operand_count; i++) {
    if (operands[i].kind == OPERAND_VALUE)
      return fold_fail(f, 9304,
                       "fold: #eval argument is not compile-time known");
    args[i] = (L1Value){L1_VALUE_BITS, 64, {.bits = operands[i].bits}};
  }

  if (callee->result_count > 0 && callee->results[0] &&
      callee->results[0]->kind == TY_ADDR)
    return fold_fail(f, 9308,
                     "fold: a compile-time result that is an address must be "
                     "materialized by the host, not by the IR");

  memset(&value, 0, sizeof(value));
  if (callee->flags & SUBROUTINE_EXTERN) {
    /* 没有体区域可以起 activation：直接把这一项能力调掉。 */
    result = lainvm_vm_call_host(f->tcb, sub_index, args, inst->operand_count,
                                 inst->result_count > 0 ? &value : NULL);
    if (result != LAINVM_SLICE_RUNNABLE)
      return fold_fail(f, 9305, "fold: the compile-time host call was refused");
  } else {
    if (lainvm_tcb_start(f->tcb, inst->symbol, args, inst->operand_count,
                         f->diag) != 0)
      return fold_fail(f, 9305,
                       f->diag && f->diag->message[0]
                           ? f->diag->message
                           : "fold: cannot start the compile-time call");
    result = lainvm_engine_run(f->tcb, f->fuel);
    if (result != LAINVM_SLICE_DONE)
      return fold_fail(f, 9306, "fold: compile-time call did not finish");
    if (!f->tcb->has_result && inst->result_count > 0)
      return fold_fail(f, 9307, "fold: compile-time call produced no value");
    value = f->tcb->result;
  }

  if (inst->result_count == 0) {
    *bits_out = 0;
    *width_out = 64;
    return true;
  }
  *bits_out = value.kind == L1_VALUE_ADDR ? (uint64_t)(uintptr_t)value.as.addr
                                          : value.as.bits;
  *width_out = value.bit_width ? value.bit_width : 64;
  return true;
}

/* --- `#eval` 块 lowering --------------------------------------------------- */

/* 区域重写（定义在下面）：块的 lowering 要用它做按值捕获替换。 */
static const L1Region *rewrite_region(LainFold *f, const L1Region *region,
                                      bool fold_blocks,
                                      const L1Region *block_root,
                                      bool reject_unknown);

/* 遍历整个模块，把 `#eval` 块收集起来。块里再嵌一个块今天执行不了（引擎见到
 * INST_EVAL 就是 trap 1045），所以那一种直接拒，而不是留下一个跑不了的块。 */
static bool collect_blocks(LainFold *f, const L1Region *region,
                           bool inside_block) {
  uint32_t i;
  if (!region) return true;
  for (i = 0; i < region->inst_count; i++) {
    const L1Inst *inst = &region->insts[i];
    if (inst->kind == INST_EVAL) {
      if (inside_block)
        return fold_fail(f, 9320,
                         "fold: an #eval block nested in another #eval block is "
                         "not supported yet");
      if (f->block_count >= FOLD_MAX_BLOCKS)
        return fold_fail(f, 9319, "fold: too many #eval blocks in one module");
      if (!collect_blocks(f, inst->body, true)) return false;
      f->blocks[f->block_count].block = inst;
      f->blocks[f->block_count].sub = NULL;
      f->blocks[f->block_count].name = NULL;
      f->block_count++;
      continue;
    }
    if (!collect_blocks(f, inst->body, inside_block)) return false;
    if (inst->kind == INST_SWITCH) {
      uint32_t k;
      for (k = 0; k < inst->case_count; k++)
        if (!collect_blocks(f, inst->cases[k].body, inside_block)) return false;
      if (!collect_blocks(f, inst->default_case, inside_block)) return false;
      continue;
    }
    if (!collect_blocks(f, inst->else_body, inside_block)) return false;
  }
  return true;
}

static bool collect_module_blocks(LainFold *f, const L1Module *module) {
  uint32_t i;
  for (i = 0; i < module->subroutine_count; i++) {
    const L1Subroutine *sub = &module->subroutines[i];
    if ((sub->flags & SUBROUTINE_EXTERN) || !sub->body) continue;
    if (!collect_blocks(f, sub->body, false)) return false;
  }
  return true;
}

static void release_blocks(LainFold *f) {
  uint32_t i;
  for (i = 0; i < f->block_count; i++) {
    free(f->blocks[i].name);
    f->blocks[i].name = NULL;
    f->blocks[i].sub = NULL;
    f->blocks[i].block = NULL;
  }
  free(f->run_subs);
  f->run_subs = NULL;
  f->run_sub_count = 0;
  free(f->phase1_subs);
  f->phase1_subs = NULL;
  f->block_count = 0;
  f->run_module = NULL;
}

/* 把每个块 lowering 成临时根过程，并造一份**含这些过程**的模块给 image 用。
 * 装载之后 image 按名字找入口，所以合成必须在装载之前完成。
 *
 * 「自由变量 → params、按值传入」今天还没有来源：块区域今天解析出来就是零参数
 * （`parse.c` 的 `#eval` 分支传的是 `parse_block(p, NULL, 0, ...)`）。真出现带参数
 * 的块时**拒绝**而不是猜一个值——静默错值比拒绝严重得多。 */
static bool lower_blocks(LainFold *f, const L1Module *module) {
  uint32_t i;
  char buffer[64];

  f->run_module = module;
  if (f->block_count == 0) return true;

  for (i = 0; i < f->block_count; i++) {
    const L1Region *body = f->blocks[i].block->body;
    const L1Subroutine *sub;
    if (!body) return fold_fail(f, 9319, "fold: an #eval block has no body");
    if (body->param_count != 0)
      return fold_fail(f, 9321,
                       "fold: an #eval block with declared parameters is not "
                       "supported yet");
    /* 按值捕获外围的名字：编译期已知的换成字面量，不知道的拒 9323。
     * 换完之后块体不引用任何外部名字，才能当独立根过程装载并执行。 */
    body = rewrite_region(f, body, false, body, true);
    if (f->failed) return false;
    snprintf(buffer, sizeof(buffer), "__eval_block_%u", i);
    f->blocks[i].name = (char *)malloc(strlen(buffer) + 1);
    if (!f->blocks[i].name) return fold_fail(f, 9316, "fold: out of memory");
    memcpy(f->blocks[i].name, buffer, strlen(buffer) + 1);
    sub = lainir_subroutine(f->builder, f->blocks[i].name, NULL, 0,
                            body->results, body->result_count, body);
    if (!sub)
      return fold_fail(f, 9319,
                       "fold: cannot lower an #eval block into a temporary "
                       "procedure");
    f->blocks[i].sub = sub;
  }

  f->run_sub_count = module->subroutine_count + f->block_count;
  f->run_subs = (L1Subroutine *)malloc(sizeof(*f->run_subs) * f->run_sub_count);
  if (!f->run_subs) return fold_fail(f, 9316, "fold: out of memory");
  for (i = 0; i < module->subroutine_count; i++)
    f->run_subs[i] = module->subroutines[i];
  for (i = 0; i < f->block_count; i++)
    f->run_subs[module->subroutine_count + i] = *f->blocks[i].sub;

  f->run_module = lainir_module(f->builder, module->name, module->data,
                                module->data_count, f->run_subs, f->run_sub_count);
  if (!f->run_module)
    return fold_fail(f, 9319,
                     "fold: cannot build the module that carries the #eval "
                     "blocks");
  return true;
}

static const L1Subroutine *block_sub(const LainFold *f, const L1Inst *inst) {
  uint32_t i;
  for (i = 0; i < f->block_count; i++)
    if (f->blocks[i].block == inst) return f->blocks[i].sub;
  return NULL;
}

/* 跑一个 `#eval` 块，返回它算出来的值。
 *
 * 它在**自己的账户与自己的 TCB** 里执行（D4 = 每次 eval 一份账户，D5 = 独立空间）：
 * 块花掉的额度算不到别人头上，块也拿不到调用者的地址空间——它只有 fold 这个空间，
 * 而那里面只有映像与 fold 自己的栈。 */
static bool run_eval_block(LainFold *f, const L1Inst *inst, uint64_t *bits_out,
                           uint32_t *width_out) {
  const L1Subroutine *sub = block_sub(f, inst);
  LainVmQuota quota;
  LainVmStackLease lease = lainvm_stack_no_lease();
  LainVmTcb *tcb;
  LainVmSliceResult result;
  L1Value value;
  bool has_result;
  int trap_code;
  char message[128];

  if (!sub || !sub->name)
    return fold_fail(f, 9322, "fold: an #eval block was not lowered");

  if (inst->body && inst->body->result_count > 0 && inst->body->results[0] &&
      inst->body->results[0]->kind == TY_ADDR)
    return fold_fail(f, 9308,
                     "fold: a compile-time result that is an address must be "
                     "materialized by the host, not by the IR");

  lainvm_quota_init(&quota, f->quota.limit);
  if (f->stack_bytes > 0) {
    lease.space = &f->space;
    lease.region = lainvm_space_alloc_stack(&f->space, f->stack_bytes, 1, &quota);
    if (lainvm_space_handle_none(lease.region))
      return fold_fail(f, 9318,
                       "fold: cannot admit a stack for the compile-time run");
  }
  tcb = lainvm_tcb_new(f->image, &f->space, 1, 1, f->max_call_depth, lease,
                       &quota);
  if (!tcb) {
    if (!lainvm_stack_lease_none(lease))
      (void)lainvm_space_free(&f->space, lease.region);
    return fold_fail(f, 9314, "fold: cannot admit a compile-time activation");
  }
  if (f->caps && lainvm_tcb_set_caps(tcb, f->caps, f->diag) != 0) {
    lainvm_tcb_free(tcb);
    if (!lainvm_stack_lease_none(lease))
      (void)lainvm_space_free(&f->space, lease.region);
    return fold_fail(f, 9315, "fold: cannot resolve capabilities");
  }

  memset(&value, 0, sizeof(value));
  if (lainvm_tcb_start(tcb, sub->name, NULL, 0, f->diag) != 0) {
    const char *why = f->diag && f->diag->message[0]
                          ? f->diag->message
                          : "fold: cannot start the compile-time block";
    lainvm_tcb_free(tcb);
    if (!lainvm_stack_lease_none(lease))
      (void)lainvm_space_free(&f->space, lease.region);
    return fold_fail(f, 9305, why);
  }
  result = lainvm_engine_run(tcb, f->fuel);
  /* 结果与 trap 都先取出来：TCB 下面就要销毁了。 */
  has_result = tcb->has_result;
  value = tcb->result;
  trap_code = (int)tcb->trap.status;
  lainvm_tcb_free(tcb);
  if (!lainvm_stack_lease_none(lease))
    (void)lainvm_space_free(&f->space, lease.region);

  if (result != LAINVM_SLICE_DONE) {
    /* Trap 是块自己的失败，把引擎那枚稳定码原样报上来（1007 容量不够、
     * 1044 配额不够…），不压成一个笼统的号：诊断要能指到那一次执行。 */
    if (result == LAINVM_SLICE_TRAPPED && trap_code > 0) {
      snprintf(message, sizeof(message),
               "fold: the compile-time block trapped (%d)", trap_code);
      return fold_fail(f, trap_code, message);
    }
    return fold_fail(f, 9306, "fold: the compile-time block did not finish");
  }
  if (!has_result && inst->result_count > 0)
    return fold_fail(f, 9307, "fold: the compile-time block produced no value");

  if (inst->result_count == 0) {
    *bits_out = 0;
    *width_out = 64;
    return true;
  }
  *bits_out = value.kind == L1_VALUE_ADDR ? (uint64_t)(uintptr_t)value.as.addr
                                          : value.as.bits;
  *width_out = value.bit_width ? value.bit_width : 64;
  return true;
}

/* --- 区域重写 ------------------------------------------------------------- */

static const L1Region *rewrite_region(LainFold *f, const L1Region *region,
                                      bool fold_blocks,
                                      const L1Region *block_root,
                                      bool reject_unknown) {
  const L1Inst **insts;
  L1RegionParam *params;
  const L1Region *out;
  uint32_t written = 0;
  uint32_t i;

  if (f->failed || !region) return region;

  insts = (const L1Inst **)malloc(sizeof(*insts) *
                                  (region->inst_count ? region->inst_count : 1));
  params = (L1RegionParam *)malloc(sizeof(*params) *
                                   (region->param_count ? region->param_count : 1));
  if (!insts || !params) {
    free(insts);
    free(params);
    fold_fail(f, 9309, "fold: out of memory");
    return region;
  }

  /* 循环参数的初值也可能引用已折叠的值。 */
  for (i = 0; i < region->param_count; i++) {
    bool ignored = false;
    params[i] = region->params[i];
    params[i].init =
        rewrite_operand(f, block_root, reject_unknown, region->params[i].init,
                        &ignored);
  }

  for (i = 0; i < region->inst_count && !f->failed; i++) {
    const L1Inst *inst = &region->insts[i];
    const L1Region *body;
    const L1Region *else_body;
    /* 进入块体时，块体自己就是"块根"：在那里面块内绑定优先，外面的同名值不许串进来。
     * 这条对第一遍（还不折块）同样成立——否则块内的局部会被外围已折叠的同名值顶掉。 */
    const L1Region *child_root =
        (inst->kind == INST_EVAL && !fold_blocks) ? inst->body : block_root;
    const L1Operand *ops;
    bool changed = false;

    ops = rewrite_operands(f, inst, block_root, reject_unknown, &changed);
    if (f->failed) {
      free((void *)ops);
      break;
    }

    /* #switch 的分支在 cases[] 里，lainir_inst_rewrite 管不到它们
     * （它只换 body/else_body），所以单独走一条。 */
    if (inst->kind == INST_SWITCH) {
      L1SwitchCase *rewritten = NULL;
      const L1Region *new_default = inst->default_case;
      L1Operand selector;
      uint32_t k;

      memset(&selector, 0, sizeof(selector));
      if (inst->operand_count > 0) selector = ops[0];
      if (inst->case_count > 0) {
        rewritten =
            (L1SwitchCase *)malloc(sizeof(L1SwitchCase) * inst->case_count);
        if (!rewritten) {
          free((void *)ops);
          fold_fail(f, 9309, "fold: out of memory");
          break;
        }
      }
      for (k = 0; k < inst->case_count && !f->failed; k++) {
        rewritten[k].value = inst->cases[k].value;
        rewritten[k].body =
            rewrite_region(f, inst->cases[k].body, fold_blocks, block_root,
                           reject_unknown);
      }
      if (!f->failed && inst->default_case)
        new_default = rewrite_region(f, inst->default_case, fold_blocks,
                                     block_root, reject_unknown);
      if (f->failed) {
        free(rewritten);
        free((void *)ops);
        break;
      }
      insts[written] = lainir_inst_rewrite_switch(
          f->builder, inst, selector, rewritten, inst->case_count, new_default);
      free(rewritten);
      free((void *)ops);
      if (!insts[written]) {
        fold_fail(f, 9311, "fold: out of memory");
        break;
      }
      written++;
      continue;
    }

    /* `#eval` 块：在独立 TCB 里跑出结果，然后把块本身折掉（块体不再重写——
     * 它已经执行过了，而且引擎根本不认它）。
     * 第一遍（fold_blocks = false）不折块，只让块体跟着重写：块要**按值捕获**
     * 的外围值来自第一遍折叠的结果，所以顺序是"先折调用、再 lowering 块"。 */
    if (inst->kind == INST_EVAL && fold_blocks) {
      uint64_t bits = 0;
      uint32_t width = 64;
      if (!run_eval_block(f, inst, &bits, &width)) {
        free((void *)ops);
        break;
      }
      if (inst->result_count > 0) remember(f, inst->results[0], bits, width);
      f->folded++;
      free((void *)ops);
      continue;
    }

    body = inst->body ? rewrite_region(f, inst->body, fold_blocks, child_root,
                                       reject_unknown)
                      : inst->body;
    else_body = inst->else_body
                    ? rewrite_region(f, inst->else_body, fold_blocks, child_root,
                                     reject_unknown)
                    : NULL;
    if (f->failed) {
      free((void *)ops);
      break;
    }

    if (inst->is_eval) {
      uint64_t bits = 0;
      uint32_t width = 64;
      if (!run_eval(f, inst, ops ? ops : inst->operands, &bits, &width)) {
        free((void *)ops);
        break;
      }
      if (inst->result_count > 0) remember(f, inst->results[0], bits, width);
      f->folded++;
      free((void *)ops);
      continue; /* 这条被折掉，不进新模块 */
    }

    if (changed || body != inst->body || else_body != inst->else_body) {
      insts[written] = lainir_inst_rewrite(f->builder, inst, ops,
                                           inst->operand_count, body, else_body);
      if (!insts[written]) {
        free((void *)ops);
        fold_fail(f, 9311, "fold: out of memory");
        break;
      }
    } else {
      insts[written] = inst; /* 没动过就复用，连源位置一起保住 */
    }
    written++;
    free((void *)ops);
  }

  if (f->failed) {
    free(insts);
    free(params);
    return region;
  }

  out = lainir_region(f->builder, params, region->param_count, region->results,
                      region->result_count, insts, written);
  free(insts);
  free(params);
  if (!out) fold_fail(f, 9312, "fold: cannot rebuilt a region");
  return out;
}

/* --- 模块 ----------------------------------------------------------------- */

/* 结束一次编译期执行环境：TCB 先销毁（它结束借用），租约才真的归还；最后放映像。 */
static void end_run(LainFold *f, LainVmStackLease lease) {
  if (f->tcb) {
    lainvm_tcb_free(f->tcb);
    f->tcb = NULL;
  }
  if (!lainvm_stack_lease_none(lease))
    (void)lainvm_space_free(&f->space, lease.region);
  if (f->image) {
    lainvm_image_free(f->image);
    f->image = NULL;
  }
}

/* 失败退出时把这一次调用拿到的执行资源全部放掉：执行环境 + 那些临时过程。 */
static void abandon_run(LainFold *f, LainVmStackLease lease) {
  end_run(f, lease);
  release_blocks(f);
}

/* 起一次编译期执行环境：装载模块 + 申请栈租约 + 建 TCB + 解析能力。
 * 供给方 = 这个 fold：栈由它向 VSpace 申请，TCB 只借，执行完由它释放。
 * 失败时自己清理干净并写诊断。 */
static bool begin_run(LainFold *f, const L1Module *module,
                      LainVmStackLease *lease_out) {
  LainVmStackLease lease = lainvm_stack_no_lease();

  lainvm_space_init(&f->space);
  f->image = lainvm_image_load(module, &f->space, f->diag);
  if (!f->image) {
    fold_fail(f, 9313, "fold: cannot load the module for compile-time runs");
    return false;
  }
  if (f->stack_bytes > 0) {
    lease.space = &f->space;
    lease.region = lainvm_space_alloc_stack(&f->space, f->stack_bytes, 1, &f->quota);
    if (lainvm_space_handle_none(lease.region)) {
      fold_fail(f, 9318, "fold: cannot admit a stack for the compile-time run");
      end_run(f, lease);
      return false;
    }
  }
  f->tcb = lainvm_tcb_new(f->image, &f->space, 1, 1, f->max_call_depth, lease,
                          &f->quota);
  if (!f->tcb) {
    fold_fail(f, 9314, "fold: cannot admit a compile-time activation");
    end_run(f, lease);
    return false;
  }
  if (f->caps && lainvm_tcb_set_caps(f->tcb, f->caps, f->diag) != 0) {
    fold_fail(f, 9315, "fold: cannot resolve capabilities");
    end_run(f, lease);
    return false;
  }
  *lease_out = lease;
  return true;
}

/* 折叠分**两遍**，因为 `#eval` 块要按值捕获外围的名字，而那些值来自第一遍：
 *   第一遍  折调用形态（执行环境装载的是**输入模块**）；
 *   lowering 收集块 + 捕获替换 + 合成临时根过程；
 *   第二遍  真跑块、把块替成字面量（执行环境装载的是**含临时过程的模块**）。
 * 两遍各起一次执行环境：image 是不可变的，"让临时过程进 image"只能靠再装载一次。 */
const L1Module *lainfold_module(LainFold *fold, L1Builder *builder,
                                const L1Module *module, L1Diagnostic *diag) {
  L1Subroutine *subs;
  const L1Module *stage1;
  const L1Module *out;
  LainVmStackLease lease = lainvm_stack_no_lease();
  uint32_t i;

  if (!fold || !builder || !module) return NULL;
  fold->builder = builder;
  fold->module = module;
  fold->diag = diag;
  fold->failed = false;
  fold->value_count = 0;
  release_blocks(fold); /* 上一次调用留下的（正常路径已经放掉了） */
  if (diag) {
    diag->code = 0;
    diag->message[0] = '\0';
  }

  /* --- 第一遍：折调用形态（`#eval f(...)`） --- */
  if (!begin_run(fold, module, &lease)) return NULL;
  fold->phase1_subs = (L1Subroutine *)malloc(
      sizeof(*fold->phase1_subs) *
      (module->subroutine_count ? module->subroutine_count : 1));
  if (!fold->phase1_subs) {
    fold_fail(fold, 9316, "fold: out of memory");
    abandon_run(fold, lease);
    return NULL;
  }
  for (i = 0; i < module->subroutine_count; i++) {
    fold->phase1_subs[i] = module->subroutines[i];
    if ((fold->phase1_subs[i].flags & SUBROUTINE_EXTERN) ||
        !fold->phase1_subs[i].body)
      continue;
    fold->phase1_subs[i].body =
        rewrite_region(fold, fold->phase1_subs[i].body, false, NULL, false);
    if (fold->failed) {
      abandon_run(fold, lease);
      return NULL;
    }
  }
  stage1 = lainir_module(builder, module->name, module->data, module->data_count,
                         fold->phase1_subs, module->subroutine_count);
  if (!stage1) {
    fold_fail(fold, 9317, "fold: cannot build the folded module");
    abandon_run(fold, lease);
    return NULL;
  }
  /* 第一遍的环境收掉：第二遍要用**含临时过程**的 image。 */
  end_run(fold, lease);
  lease = lainvm_stack_no_lease();

  /* --- lowering：收集块 + 按值捕获 + 合成临时根过程 --- */
  if (!collect_module_blocks(fold, stage1) || !lower_blocks(fold, stage1)) {
    release_blocks(fold);
    return NULL;
  }

  /* --- 第二遍：块真的执行、结果替成字面量 --- */
  if (!begin_run(fold, fold->run_module, &lease)) {
    release_blocks(fold);
    return NULL;
  }
  subs = (L1Subroutine *)malloc(sizeof(*subs) *
                                (module->subroutine_count ? module->subroutine_count : 1));
  if (!subs) {
    fold_fail(fold, 9316, "fold: out of memory");
    abandon_run(fold, lease);
    return NULL;
  }
  for (i = 0; i < module->subroutine_count; i++) {
    subs[i] = stage1->subroutines[i];
    if ((subs[i].flags & SUBROUTINE_EXTERN) || !subs[i].body) continue;
    subs[i].body = rewrite_region(fold, subs[i].body, true, NULL, false);
    if (fold->failed) {
      free(subs);
      abandon_run(fold, lease);
      return NULL;
    }
  }

  out = lainir_module(builder, module->name, module->data, module->data_count,
                      subs, module->subroutine_count);
  free(subs);

  abandon_run(fold, lease);
  if (!out) {
    fold_fail(fold, 9317, "fold: cannot build the folded module");
    return NULL;
  }
  return out;
}
