/* 装载一个模块，把映像的表打出来。不跑内核。 */
#include <stdio.h>

#include "lainir/build.h"
#include "lainvm/image.h"

int main(void) {
  L1Builder *b = lainir_builder_new();
  const L1Type *u64 = lainir_type(b, TY_BITS, 64);
  L1Operand ops[2];
  L1Operand rops[1];
  const L1Inst *insts[2];
  const L1Type *rtypes[1];
  static const uint8_t bytes[3] = {7, 8, 9};
  L1Data data[1];
  L1Diagnostic diag;
  LainVmSpace space;
  LainVmImage *image;
  uint32_t i;

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
    const L1Module *module;
    data[0] = *lainir_data(b, "bytes", bytes, 3, false);
    module = lainir_module(b, "probe", data, 1, sub, 1);

    lainvm_space_init(&space);
    image = lainvm_image_load(module, &space, &diag);
    if (!image) {
      printf("load failed: code=%d %s\n", diag.code, diag.message);
      lainir_builder_free(b);
      return 1;
    }

    printf("image: %u region(s), %u inst meta, %u operand ref, %u result slot,"
           " %u symbol\n",
           image->region_count, image->inst_count, image->operand_count,
           image->result_count, image->symbol_count);
    printf("bounds: max_region_depth=%u max_slots=%u\n",
           image->max_region_depth, image->max_slots);

    for (i = 0; i < image->region_count; i++) {
      const LainVmImageRegion *rec = &image->regions[i];
      uint32_t pos;
      printf("region %u: param_slots=%u slot_count=%u parent=%u insts=%u\n", i,
             rec->param_slots, rec->slot_count, rec->parent, rec->inst_count);
      for (pos = 0; pos < rec->inst_count; pos++) {
        LainVmImageInstMeta meta = lainvm_image_inst_meta(image, i, pos);
        uint32_t k;
        printf("  [%u] kind=%d", pos, (int)rec->region->insts[pos].kind);
        if (meta.operand_base != LAINVM_IMAGE_NO_INDEX) {
          for (k = 0; k < rec->region->insts[pos].operand_count; k++) {
            LainVmOperandRef ref =
                lainvm_image_operand_ref(image, i, pos, k);
            if (ref.is_literal)
              printf(" op%u=lit", k);
            else
              printf(" op%u=slot(+%u)@dist%u", k, ref.slot, ref.frame_distance);
          }
        }
        if (meta.result_base != LAINVM_IMAGE_NO_INDEX) {
          for (k = 0; k < rec->region->insts[pos].result_count; k++)
            printf(" res%u=slot%u", k,
                   lainvm_image_result_slot(image, i, pos, k));
        }
        printf("\n");
      }
    }

    for (i = 0; i < image->symbol_count; i++) {
      printf("symbol %s: addr=%llx size=%u rights=%u\n",
             image->symbols[i].symbol,
             (unsigned long long)image->symbols[i].addr,
             image->symbols[i].size, image->symbols[i].rights);
    }

    {
      uint32_t body_region = LAINVM_IMAGE_NO_REGION;
      const L1Subroutine *entry =
          lainvm_image_entry(image, "answer", &body_region);
      printf("entry answer: %s body_region=%u\n", entry ? entry->name : "(none)",
             body_region);
    }

    lainvm_image_free(image);
  }

  lainir_builder_free(b);
  return 0;
}
