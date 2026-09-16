/* 用手搭一个模块，把结构打出来。只测 builder，不跑内核。 */
#include <stdio.h>

#include "lainir/build.h"

static const char *kind_name(L1InstKind kind) {
  switch (kind) {
  case INST_ADD: return "add";
  case INST_RETURN: return "return";
  case INST_SGE: return "sge";
  case INST_IF: return "if";
  case INST_YIELD: return "yield";
  case INST_LOOP: return "loop";
  case INST_BREAK: return "break";
  case INST_CONTINUE: return "continue";
  case INST_LEA: return "lea";
  case INST_LOAD: return "load";
  case INST_ZEXT: return "zext";
  default: return "?";
  }
}

int main(void) {
  L1Builder *b = lainir_builder_new();
  const L1Type *u64 = lainir_type(b, TY_BITS, 64);
  L1Operand ops[2];
  L1Operand rops[1];
  const L1Inst *insts[2];
  const L1Type *rtypes[1];

  ops[0] = lainir_int(40);
  ops[1] = lainir_int(2);
  insts[0] = lainir_inst(b, INST_ADD, "%r", u64, ops, 2);

  rops[0] = lainir_ref("%r");
  insts[1] = lainir_inst(b, INST_RETURN, NULL, NULL, rops, 1);

  rtypes[0] = u64;
  {
    const L1Region *body = lainir_region(b, NULL, 0, NULL, 0, insts, 2);
    const L1Subroutine *sub =
        lainir_subroutine(b, "answer", NULL, 0, rtypes, 1, body);
    const L1Module *module = lainir_module(b, "probe", NULL, 0, sub, 1);

    printf("module %s: %u subroutine(s)\n", module->name,
           module->subroutine_count);
    {
      const L1Subroutine *s = &module->subroutines[0];
      uint32_t i;
      printf("  #proc %s -> %u result(s), body %u inst(s)\n", s->name,
             s->result_count, s->body->inst_count);
      for (i = 0; i < s->body->inst_count; i++) {
        const L1Inst *inst = &s->body->insts[i];
        printf("    [%u] %s  results=%u ty=%s%u  ops=%u\n", i,
               kind_name(inst->kind), inst->result_count,
               inst->has_ty ? "bits/" : "none/",
               inst->has_ty ? inst->ty->width : 0u, inst->operand_count);
      }
      if (s->body->insts[0].result_count > 0)
        printf("    def %s\n", s->body->insts[0].results[0]);
      if (s->body->insts[1].operand_count > 0)
        printf("    use %s\n", s->body->insts[1].operands[0].name);
    }
  }

  lainir_builder_free(b);
  return 0;
}
