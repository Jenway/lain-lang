/* lainir/build.h 的实现。
 *
 * 一个 arena：对象一个个从块里切出来，块挂在链表上，free 时整条链回收。
 * 切出来的东西不移动，所以调用方拿到的指针一直有效。
 */
#include "lainir/build.h"

#include <stdlib.h>
#include <string.h>

#define L1_ARENA_BLOCK_MIN 4096u
#define L1_ARENA_ALIGN 16u

typedef struct L1ArenaBlock {
  struct L1ArenaBlock *next;
  size_t used;
  size_t cap;
  unsigned char *data;
} L1ArenaBlock;

struct L1Builder {
  L1ArenaBlock *blocks;
};

L1Builder *lainir_builder_new(void) {
  return (L1Builder *)calloc(1, sizeof(L1Builder));
}

void lainir_builder_free(L1Builder *builder) {
  L1ArenaBlock *block;
  if (!builder) return;
  block = builder->blocks;
  while (block) {
    L1ArenaBlock *next = block->next;
    free(block->data);
    free(block);
    block = next;
  }
  free(builder);
}

static void *arena_alloc(L1Builder *builder, size_t size) {
  L1ArenaBlock *block;
  size_t need;
  if (size == 0) size = 1;
  need = (size + (L1_ARENA_ALIGN - 1)) & ~(size_t)(L1_ARENA_ALIGN - 1);
  block = builder->blocks;
  if (!block || block->cap - block->used < need) {
    size_t cap = need > L1_ARENA_BLOCK_MIN ? need : L1_ARENA_BLOCK_MIN;
    L1ArenaBlock *fresh = (L1ArenaBlock *)calloc(1, sizeof(L1ArenaBlock));
    if (!fresh) return NULL;
    fresh->data = (unsigned char *)calloc(1, cap);
    if (!fresh->data) {
      free(fresh);
      return NULL;
    }
    fresh->cap = cap;
    fresh->next = builder->blocks;
    builder->blocks = fresh;
    block = fresh;
  }
  {
    void *out = block->data + block->used;
    block->used += need;
    return out;
  }
}

static void *arena_copy(L1Builder *builder, const void *src, size_t size) {
  void *dst;
  if (!src || size == 0) return NULL;
  dst = arena_alloc(builder, size);
  if (!dst) return NULL;
  memcpy(dst, src, size);
  return dst;
}

void *lainir_builder_alloc(L1Builder *builder, size_t size) {
  if (!builder) return NULL;
  return arena_alloc(builder, size);
}

const char *lainir_builder_string(L1Builder *builder, const char *text) {
  size_t n;
  if (!text) return NULL;
  n = strlen(text) + 1;
  return (const char *)arena_copy(builder, text, n);
}

const L1Type *lainir_type(L1Builder *builder, L1TypeKind kind, uint32_t width) {
  L1Type *type = (L1Type *)arena_alloc(builder, sizeof(L1Type));
  if (!type) return NULL;
  type->kind = kind;
  type->width = width;
  return type;
}

L1Operand lainir_ref(const char *name) {
  L1Operand op;
  memset(&op, 0, sizeof(op));
  op.kind = OPERAND_VALUE;
  op.name = name;
  return op;
}

L1Operand lainir_int(uint64_t bits) {
  L1Operand op;
  memset(&op, 0, sizeof(op));
  op.kind = OPERAND_INT;
  op.bits = bits;
  return op;
}

L1Operand lainir_float_bits(uint64_t bits) {
  L1Operand op;
  memset(&op, 0, sizeof(op));
  op.kind = OPERAND_FLOAT;
  op.bits = bits;
  return op;
}

L1RegionParam lainir_param(const char *name, const L1Type *ty, L1Operand init) {
  L1RegionParam param;
  memset(&param, 0, sizeof(param));
  param.param.name = name;
  param.param.ty = ty;
  param.init = init;
  return param;
}

L1Param lainir_proc_param(const char *name, const L1Type *ty) {
  L1Param param;
  memset(&param, 0, sizeof(param));
  param.name = name;
  param.ty = ty;
  return param;
}

static const L1Type **copy_types(L1Builder *builder, const L1Type *const *types,
                                 uint32_t count) {
  if (!types || count == 0) return NULL;
  return (const L1Type **)arena_copy(builder, types,
                                     sizeof(L1Type *) * (size_t)count);
}

const L1Region *lainir_region(L1Builder *builder, const L1RegionParam *params,
                              uint32_t param_count,
                              const L1Type *const *results,
                              uint32_t result_count,
                              const L1Inst *const *insts, uint32_t inst_count) {
  L1Region *region = (L1Region *)arena_alloc(builder, sizeof(L1Region));
  if (!region) return NULL;
  region->params = (const L1RegionParam *)arena_copy(
      builder, params, sizeof(L1RegionParam) * (size_t)param_count);
  region->param_count = param_count;
  region->results = copy_types(builder, results, result_count);
  region->result_count = result_count;
  if (insts && inst_count) {
    L1Inst *copied = (L1Inst *)arena_alloc(builder, sizeof(L1Inst) * (size_t)inst_count);
    uint32_t i;
    if (!copied) return NULL;
    for (i = 0; i < inst_count; i++) copied[i] = *insts[i];
    region->insts = copied;
  }
  region->inst_count = inst_count;
  if (params && param_count && !region->params) return NULL;
  return region;
}

/* 放操作数并返回指令指针。results 已由调用方填好。 */
static const L1Inst *finish_inst(L1Builder *builder, L1Inst *inst,
                                 const L1Operand *operands,
                                 uint32_t operand_count) {
  inst->operands = (const L1Operand *)arena_copy(
      builder, operands, sizeof(L1Operand) * (size_t)operand_count);
  inst->operand_count = operand_count;
  if (operands && operand_count && !inst->operands) return NULL;
  return inst;
}

static const L1Inst *new_inst(L1Builder *builder, L1InstKind kind,
                              const char *result, uint32_t result_count) {
  L1Inst *inst = (L1Inst *)arena_alloc(builder, sizeof(L1Inst));
  if (!inst) return NULL;
  memset(inst, 0, sizeof(*inst));
  inst->kind = kind;
  inst->order = ORDER_RELAXED;
  if (result_count > L1_MAX_RESULTS) return NULL;
  inst->result_count = result_count;
  if (result_count > 0) inst->results[0] = result;
  return inst;
}

const L1Inst *lainir_inst(L1Builder *builder, L1InstKind kind, const char *result,
                          const L1Type *ty, const L1Operand *operands,
                          uint32_t operand_count) {
  L1Inst *inst = (L1Inst *)new_inst(builder, kind, result, result ? 1u : 0u);
  if (!inst) return NULL;
  inst->ty = ty;
  inst->has_ty = ty != NULL;
  return finish_inst(builder, inst, operands, operand_count);
}

const L1Inst *lainir_inst_if(L1Builder *builder, const char *result,
                             L1Operand cond, const L1Region *then_body,
                             const L1Region *else_body) {
  L1Inst *inst = (L1Inst *)new_inst(builder, INST_IF, result, result ? 1u : 0u);
  if (!inst) return NULL;
  inst->body = then_body;
  inst->else_body = else_body;
  return finish_inst(builder, inst, &cond, 1);
}

const L1Inst *lainir_inst_loop(L1Builder *builder, const char *result,
                               const char *label, const L1Region *body) {
  L1Inst *inst = (L1Inst *)new_inst(builder, INST_LOOP, result,
                                    result ? 1u : 0u);
  if (!inst) return NULL;
  inst->label = label;
  inst->body = body;
  return finish_inst(builder, inst, NULL, 0);
}

const L1Inst *lainir_inst_eval(L1Builder *builder, const char *result,
                               const L1Region *body) {
  L1Inst *inst = (L1Inst *)new_inst(builder, INST_EVAL, result,
                                    result ? 1u : 0u);
  if (!inst) return NULL;
  inst->body = body;
  return finish_inst(builder, inst, NULL, 0);
}

const L1Inst *lainir_inst_switch(L1Builder *builder, const char *result,
                                 L1Operand selector, const L1Type *ty,
                                 const L1SwitchCase *cases, uint32_t case_count,
                                 const L1Region *default_case) {
  L1Inst *inst =
      (L1Inst *)new_inst(builder, INST_SWITCH, result, result ? 1u : 0u);
  if (!inst) return NULL;
  inst->ty = ty;
  inst->has_ty = ty != NULL;
  if (cases && case_count) {
    L1SwitchCase *copied = (L1SwitchCase *)arena_alloc(
        builder, sizeof(L1SwitchCase) * (size_t)case_count);
    uint32_t i;
    if (!copied) return NULL;
    for (i = 0; i < case_count; i++) copied[i] = cases[i];
    inst->cases = copied;
  }
  inst->case_count = case_count;
  inst->default_case = default_case;
  return finish_inst(builder, inst, &selector, 1);
}

const L1Inst *lainir_inst_jump(L1Builder *builder, L1InstKind kind,
                               const char *label, const L1Operand *operands,
                               uint32_t operand_count) {
  L1Inst *inst = (L1Inst *)new_inst(builder, kind, NULL, 0);
  if (!inst) return NULL;
  inst->label = label;
  return finish_inst(builder, inst, operands, operand_count);
}

const L1Inst *lainir_inst_call(L1Builder *builder, const char *result,
                               const char *symbol, const L1Operand *operands,
                               uint32_t operand_count) {
  L1Inst *inst = (L1Inst *)new_inst(builder, INST_CALL, result,
                                    result ? 1u : 0u);
  if (!inst) return NULL;
  inst->symbol = symbol;
  return finish_inst(builder, inst, operands, operand_count);
}

const L1Inst *lainir_inst_mem(L1Builder *builder, L1InstKind kind,
                              const char *result, const L1Type *ty,
                              L1MemOrder order, bool is_volatile,
                              const L1Operand *operands, uint32_t operand_count) {
  L1Inst *inst = (L1Inst *)new_inst(builder, kind, result, result ? 1u : 0u);
  if (!inst) return NULL;
  inst->ty = ty;
  inst->has_ty = ty != NULL;
  inst->order = order;
  inst->is_volatile = is_volatile;
  return finish_inst(builder, inst, operands, operand_count);
}

const L1Inst *lainir_inst_symbol(L1Builder *builder, L1InstKind kind,
                                 const char *result, const char *symbol) {
  L1Inst *inst = (L1Inst *)new_inst(builder, kind, result, result ? 1u : 0u);
  if (!inst) return NULL;
  inst->symbol = symbol;
  return finish_inst(builder, inst, NULL, 0);
}

const L1Inst *lainir_inst_rewrite(L1Builder *builder, const L1Inst *inst,
                                  const L1Operand *operands,
                                  uint32_t operand_count, const L1Region *body,
                                  const L1Region *else_body) {
  L1Inst *copy;
  if (!builder || !inst) return NULL;
  copy = (L1Inst *)arena_alloc(builder, sizeof(L1Inst));
  if (!copy) return NULL;
  *copy = *inst;
  copy->operands = (const L1Operand *)arena_copy(
      builder, operands, sizeof(L1Operand) * (size_t)operand_count);
  copy->operand_count = operand_count;
  if (operands && operand_count && !copy->operands) return NULL;
  if (body) copy->body = body;
  if (else_body) copy->else_body = else_body;
  return copy;
}

const L1Inst *lainir_inst_rewrite_switch(L1Builder *builder,
                                         const L1Inst *inst,
                                         L1Operand selector,
                                         const L1SwitchCase *cases,
                                         uint32_t case_count,
                                         const L1Region *default_case) {
  L1Inst *copy;
  if (!builder || !inst) return NULL;
  copy = (L1Inst *)arena_alloc(builder, sizeof(L1Inst));
  if (!copy) return NULL;
  *copy = *inst;
  if (cases && case_count) {
    L1SwitchCase *copied = (L1SwitchCase *)arena_alloc(
        builder, sizeof(L1SwitchCase) * (size_t)case_count);
    uint32_t i;
    if (!copied) return NULL;
    for (i = 0; i < case_count; i++) copied[i] = cases[i];
    copy->cases = copied;
  } else {
    copy->cases = NULL;
  }
  copy->case_count = case_count;
  if (default_case) copy->default_case = default_case;
  return finish_inst(builder, copy, &selector, 1);
}

const L1Subroutine *lainir_subroutine(L1Builder *builder, const char *name,                                      const L1Param *params, uint32_t param_count,
                                      const L1Type *const *results,
                                      uint32_t result_count,
                                      const L1Region *body) {
  L1Subroutine *sub = (L1Subroutine *)arena_alloc(builder, sizeof(L1Subroutine));
  if (!sub) return NULL;
  memset(sub, 0, sizeof(*sub));
  sub->name = name;
  sub->flags = SUBROUTINE_NONE;
  sub->params = (const L1Param *)arena_copy(
      builder, params, sizeof(L1Param) * (size_t)param_count);
  sub->param_count = param_count;
  sub->results = copy_types(builder, results, result_count);
  sub->result_count = result_count;
  sub->body = body;
  if (params && param_count && !sub->params) return NULL;
  return sub;
}

const L1Subroutine *lainir_subroutine_extern(
    L1Builder *builder, const char *name, const char *link_name,
    const L1Param *params, uint32_t param_count, const L1Type *const *results,
    uint32_t result_count) {
  L1Subroutine *sub = (L1Subroutine *)arena_alloc(builder, sizeof(L1Subroutine));
  if (!sub) return NULL;
  memset(sub, 0, sizeof(*sub));
  sub->name = name;
  sub->flags = SUBROUTINE_EXTERN;
  sub->link_name = link_name;
  sub->params = (const L1Param *)arena_copy(
      builder, params, sizeof(L1Param) * (size_t)param_count);
  sub->param_count = param_count;
  sub->results = copy_types(builder, results, result_count);
  sub->result_count = result_count;
  sub->body = NULL;
  if (params && param_count && !sub->params) return NULL;
  return sub;
}

const L1Data *lainir_data(L1Builder *builder, const char *symbol,
                          const uint8_t *bytes, uint32_t size, bool is_writable) {
  L1Data *data = (L1Data *)arena_alloc(builder, sizeof(L1Data));
  if (!data) return NULL;
  memset(data, 0, sizeof(*data));
  data->symbol = symbol;
  data->bytes = (const uint8_t *)arena_copy(builder, bytes, size);
  data->size = size;
  data->is_writable = is_writable;
  if (bytes && size && !data->bytes) return NULL;
  return data;
}

const L1Module *lainir_module(L1Builder *builder, const char *name,
                              const L1Data *data, uint32_t data_count,
                              const L1Subroutine *subs, uint32_t sub_count) {
  L1Module *module = (L1Module *)arena_alloc(builder, sizeof(L1Module));
  if (!module) return NULL;
  memset(module, 0, sizeof(*module));
  module->name = name;
  module->data = (const L1Data *)arena_copy(
      builder, data, sizeof(L1Data) * (size_t)data_count);
  module->data_count = data_count;
  module->subroutines = (const L1Subroutine *)arena_copy(
      builder, subs, sizeof(L1Subroutine) * (size_t)sub_count);
  module->subroutine_count = sub_count;
  if (data && data_count && !module->data) return NULL;
  if (subs && sub_count && !module->subroutines) return NULL;
  return module;
}
