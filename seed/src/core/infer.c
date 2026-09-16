/* lainir/infer.h 的实现。 */
#include "lainir/infer.h"

#include <string.h>

void lainir_types_init(LainIrTypes *types, const L1Module *module) {
  types->module = module;
  types->bits1.kind = TY_BITS;
  types->bits1.width = 1;
  types->addr.kind = TY_ADDR;
  types->addr.width = 0;
}

const L1Subroutine *lainir_find_subroutine(const L1Module *module,
                                           const char *name) {
  uint32_t i;
  if (!module || !name) return NULL;
  for (i = 0; i < module->subroutine_count; i++) {
    const L1Subroutine *sub = &module->subroutines[i];
    if (sub->name && strcmp(sub->name, name) == 0) return sub;
  }
  return NULL;
}

const L1Data *lainir_find_data(const L1Module *module, const char *symbol) {
  uint32_t i;
  if (!module || !symbol) return NULL;
  for (i = 0; i < module->data_count; i++) {
    const L1Data *data = &module->data[i];
    if (data->symbol && strcmp(data->symbol, symbol) == 0) return data;
  }
  return NULL;
}

uint32_t lainir_type_width(const L1Type *type) {
  if (!type) return 0;
  if (type->kind == TY_ADDR) return 64;
  return type->width ? type->width : 64;
}

static bool is_compare(L1InstKind kind) {
  switch (kind) {
  case INST_EQ: case INST_NE:
  case INST_SLT: case INST_SLE: case INST_SGT: case INST_SGE:
  case INST_ULT: case INST_ULE: case INST_UGT: case INST_UGE:
  case INST_FOEQ: case INST_FONE: case INST_FOLT: case INST_FOLE:
  case INST_FOGT: case INST_FOGE:
  case INST_FUEQ: case INST_FUNE: case INST_FULT: case INST_FULE:
  case INST_FUGT: case INST_FUGE:
    return true;
  default:
    return false;
  }
}

static bool is_address_result(L1InstKind kind) {
  switch (kind) {
  case INST_LEA:
  case INST_ALLOCA:
  case INST_DATA_ADDR:
  case INST_PROC_ADDR:
  case INST_INT2PTR:
    return true;
  default:
    return false;
  }
}

const L1Type *lainir_inst_result_type(const LainIrTypes *types,
                                      const L1Inst *inst, uint32_t index) {
  if (!types || !inst || index >= inst->result_count) return NULL;
  if (is_compare(inst->kind)) return &types->bits1;
  if (is_address_result(inst->kind)) return &types->addr;
  if (inst->kind == INST_IF || inst->kind == INST_LOOP) {
    if (inst->body && index < inst->body->result_count)
      return inst->body->results[index];
    return NULL;
  }
  if (inst->kind == INST_CALL) {
    const L1Subroutine *callee =
        lainir_find_subroutine(types->module, inst->symbol);
    if (callee && index < callee->result_count) return callee->results[index];
    return NULL;
  }
  return inst->has_ty ? inst->ty : NULL;
}
