/* lainir/opname.h 的实现。表就是规范：拼写改了这里，解析和打印一起改。 */
#include "lainir/opname.h"

#include <string.h>

typedef struct {
  const char *name;
  L1InstKind kind;
} OpEntry;

static const OpEntry k_ops[] = {
    {"add", INST_ADD},
    {"sub", INST_SUB},
    {"mul", INST_MUL},
    {"sdiv", INST_SDIV},
    {"udiv", INST_UDIV},
    {"srem", INST_SREM},
    {"urem", INST_UREM},
    {"and", INST_AND},
    {"or", INST_OR},
    {"xor", INST_XOR},
    {"shl", INST_SHL},
    {"lshr", INST_LSHR},
    {"ashr", INST_ASHR},
    {"fadd", INST_FADD},
    {"fsub", INST_FSUB},
    {"fmul", INST_FMUL},
    {"fdiv", INST_FDIV},
    {"eq", INST_EQ},
    {"ne", INST_NE},
    {"slt", INST_SLT},
    {"sle", INST_SLE},
    {"sgt", INST_SGT},
    {"sge", INST_SGE},
    {"ult", INST_ULT},
    {"ule", INST_ULE},
    {"ugt", INST_UGT},
    {"uge", INST_UGE},
    {"foeq", INST_FOEQ},
    {"fone", INST_FONE},
    {"folt", INST_FOLT},
    {"fole", INST_FOLE},
    {"fogt", INST_FOGT},
    {"foge", INST_FOGE},
    {"fueq", INST_FUEQ},
    {"fune", INST_FUNE},
    {"fult", INST_FULT},
    {"fule", INST_FULE},
    {"fugt", INST_FUGT},
    {"fuge", INST_FUGE},
    {"zext", INST_ZEXT},
    {"sext", INST_SEXT},
    {"trunc", INST_TRUNC},
    {"bitcast", INST_BITCAST},
    {"fpext", INST_FPEXT},
    {"fptrunc", INST_FPTRUNC},
    {"fptosi", INST_FPTOSI},
    {"fptoui", INST_FPTOUI},
    {"sitofp", INST_SITOFP},
    {"uitofp", INST_UITOFP},
    {"int2ptr", INST_INT2PTR},
    {"ptr2int", INST_PTR2INT},
    {"vadd", INST_VADD},
    {"vsub", INST_VSUB},
    {"vmul", INST_VMUL},
    {"vdiv", INST_VDIV},
    {"vand", INST_VAND},
    {"vor", INST_VOR},
    {"vxor", INST_VXOR},
    {"vshl", INST_VSHL},
    {"vshuffle", INST_VSHUFFLE},
    {"vbroadcast", INST_VBROADCAST},
    {"vextract", INST_VEXTRACT},
    {"vinsert", INST_VINSERT},
    {"vcmpeq", INST_VCMPEQ},
    {"vcmpne", INST_VCMPNE},
    {"vcmplt", INST_VCMPLT},
    {"vcmpgt", INST_VCMPGT},
    {"xchg", INST_XCHG},
    {"cmpxchg", INST_CMPXCHG},
    {"rmw_add", INST_RMW_ADD},
    {"rmw_sub", INST_RMW_SUB},
    {"rmw_and", INST_RMW_AND},
    {"rmw_or", INST_RMW_OR},
    {"rmw_xor", INST_RMW_XOR},
    {"lea", INST_LEA},
    {"load", INST_LOAD},
    {"store", INST_STORE},
    {"alloca", INST_ALLOCA},
    {"data_addr", INST_DATA_ADDR},
    {"proc_addr", INST_PROC_ADDR},
    {"call", INST_CALL},
    {"call_indirect", INST_CALL_INDIRECT},
    {"if", INST_IF},
    {"loop", INST_LOOP},
    {"switch", INST_SWITCH},
    {"yield", INST_YIELD},
    {"break", INST_BREAK},
    {"continue", INST_CONTINUE},
    {"return", INST_RETURN},
};

#define OP_COUNT (sizeof(k_ops) / sizeof(k_ops[0]))

const char *lainir_opcode_name(L1InstKind kind) {
  size_t i;
  for (i = 0; i < OP_COUNT; i++) {
    if (k_ops[i].kind == kind) return k_ops[i].name;
  }
  return NULL;
}

bool lainir_opcode_kind(const char *name, uint32_t len, L1InstKind *kind_out) {
  size_t i;
  if (!name) return false;
  for (i = 0; i < OP_COUNT; i++) {
    if (strlen(k_ops[i].name) == len && memcmp(k_ops[i].name, name, len) == 0) {
      if (kind_out) *kind_out = k_ops[i].kind;
      return true;
    }
  }
  return false;
}
