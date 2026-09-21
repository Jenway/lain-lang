/* lainfold/fold.h 的实现。 */
#include "lainfold/fold.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lainvm/engine.h"
#include "lainvm/image.h"

#define FOLD_MAX_VALUES 256u
#define FOLD_MAX_ARGS 8u

typedef struct {
  const char *name;
  uint64_t bits;
  uint32_t width;
} FoldedValue;

struct LainFold {
  LainVmCaps *caps;
  uint32_t max_call_depth;
  uint64_t stack_bytes;
  uint64_t fuel;

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
  return fold;
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

static L1Operand rewrite_operand(LainFold *f, L1Operand op, bool *changed) {
  uint64_t bits = 0;
  if (op.kind == OPERAND_VALUE && lookup(f, op.name, &bits)) {
    *changed = true;
    return lainir_int(bits);
  }
  return op;
}

static const L1Operand *rewrite_operands(LainFold *f, const L1Inst *inst,
                                         bool *changed) {
  L1Operand *out;
  uint32_t i;
  if (inst->operand_count == 0) return NULL;
  out = (L1Operand *)malloc(sizeof(L1Operand) * inst->operand_count);
  if (!out) {
    fold_fail(f, 9300, "fold: out of memory");
    return NULL;
  }
  for (i = 0; i < inst->operand_count; i++)
    out[i] = rewrite_operand(f, inst->operands[i], changed);
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

/* --- 区域重写 ------------------------------------------------------------- */

static const L1Region *rewrite_region(LainFold *f, const L1Region *region) {
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
    params[i].init = rewrite_operand(f, region->params[i].init, &ignored);
  }

  for (i = 0; i < region->inst_count && !f->failed; i++) {
    const L1Inst *inst = &region->insts[i];
    const L1Region *body;
    const L1Region *else_body;
    const L1Operand *ops;
    bool changed = false;

    ops = rewrite_operands(f, inst, &changed);
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
        rewritten[k].body = rewrite_region(f, inst->cases[k].body);
      }
      if (!f->failed && inst->default_case)
        new_default = rewrite_region(f, inst->default_case);
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

    body = inst->body ? rewrite_region(f, inst->body) : inst->body;
    else_body = inst->else_body ? rewrite_region(f, inst->else_body) : NULL;
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

const L1Module *lainfold_module(LainFold *fold, L1Builder *builder,
                                const L1Module *module, L1Diagnostic *diag) {
  L1Subroutine *subs;
  const L1Module *out;
  uint32_t i;

  if (!fold || !builder || !module) return NULL;
  fold->builder = builder;
  fold->module = module;
  fold->diag = diag;
  fold->failed = false;
  fold->value_count = 0;
  if (diag) {
    diag->code = 0;
    diag->message[0] = '\0';
  }

  lainvm_space_init(&fold->space);
  fold->image = lainvm_image_load(module, &fold->space, diag);
  if (!fold->image) {
    fold_fail(fold, 9313, "fold: cannot load the module for compile-time runs");
    return NULL;
  }
  fold->tcb = lainvm_tcb_new(fold->image, &fold->space, 1, 1,
                             fold->max_call_depth, fold->stack_bytes);
  if (!fold->tcb) {
    fold_fail(fold, 9314, "fold: cannot admit a compile-time activation");
    return NULL;
  }
  if (fold->caps && lainvm_tcb_set_caps(fold->tcb, fold->caps, diag) != 0) {
    fold_fail(fold, 9315, "fold: cannot resolve capabilities");
    return NULL;
  }

  subs = (L1Subroutine *)malloc(sizeof(*subs) *
                                (module->subroutine_count ? module->subroutine_count : 1));
  if (!subs) {
    fold_fail(fold, 9316, "fold: out of memory");
    return NULL;
  }
  for (i = 0; i < module->subroutine_count; i++) {
    subs[i] = module->subroutines[i];
    if ((subs[i].flags & SUBROUTINE_EXTERN) || !subs[i].body) continue;
    subs[i].body = rewrite_region(fold, subs[i].body);
    if (fold->failed) {
      free(subs);
      return NULL;
    }
  }

  out = lainir_module(builder, module->name, module->data, module->data_count,
                      subs, module->subroutine_count);
  free(subs);

  lainvm_tcb_free(fold->tcb);
  lainvm_image_free(fold->image);
  fold->tcb = NULL;
  fold->image = NULL;
  if (!out) {
    fold_fail(fold, 9317, "fold: cannot build the folded module");
    return NULL;
  }
  return out;
}
