/* 装载器：L1Module -> LainVmImage。
 *
 * 一遍递归：先给区域分配 id（前序），再填指令元数据、操作数引用和结果槽。
 * 区域 id 是按前序分配的，所以父区域一定排在子区域前面——名字解析沿着
 * parent 链往外走时用得着这一点。
 */
#include "image_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  LainVmImage *image;
  L1Diagnostic *diag;
  int code; /* 第一个失败码。diag 可以是 NULL，码不能丢 */
  bool failed;
} Loader;

/* 前向声明：map_data 与 size_image 用到的两个纯查表函数定义在后面。 */
static void data_sizes(const L1Module *module, uint64_t *ro_out, uint64_t *rw_out);
static void measure_image(const L1Module *module, LainVmImageSizes *out);

static void load_fail(Loader *L, int code, const char *message) {
  if (!L->failed) {
    L->code = code;
    if (L->diag) {
      L->diag->code = code;
      L->diag->line = 0;
      L->diag->column = 0;
      snprintf(L->diag->message, sizeof(L->diag->message), "%s", message);
    }
  }
  L->failed = true;
}

/* --- 名字解析 -------------------------------------------------------------
 * 区域的作用域：先参数，再按指令顺序的结果。找不到就往外走一层。 */

static bool region_lookup(const Loader *L, uint32_t region_id, const char *name,
                          uint32_t *slot_out) {
  const LainVmImageRegion *rec = &L->image->regions[region_id];
  const L1Region *region = rec->region;
  uint32_t i;
  uint32_t slot = 0;

  /* 过程签名的参数就是体区域的入口值，占最前面几个槽。 */
  for (i = 0; i < rec->extra_count; i++, slot++) {
    if (rec->extra_params[i].name && name &&
        strcmp(rec->extra_params[i].name, name) == 0) {
      *slot_out = slot;
      return true;
    }
  }
  for (i = 0; i < region->param_count; i++, slot++) {
    if (region->params[i].param.name &&
        strcmp(region->params[i].param.name, name) == 0) {
      *slot_out = slot;
      return true;
    }
  }
  for (i = 0; i < region->inst_count; i++) {
    const L1Inst *inst = &region->insts[i];
    uint32_t k;
    for (k = 0; k < inst->result_count; k++, slot++) {
      if (inst->results[k] && strcmp(inst->results[k], name) == 0) {
        *slot_out = slot;
        return true;
      }
    }
  }
  return false;
}

static bool resolve_name(const Loader *L, uint32_t region_id, const char *name,
                         uint32_t *distance_out, uint32_t *slot_out) {
  uint32_t id = region_id;
  uint32_t distance = 0;
  while (id != LAINVM_IMAGE_NO_REGION) {
    if (region_lookup(L, id, name, slot_out)) {
      *distance_out = distance;
      return true;
    }
    id = L->image->regions[id].parent;
    distance++;
  }
  return false;
}

/* --- 区域树 ---------------------------------------------------------------- */

/* 下面几个 push 的容量检查是**防御性的**：容量由 count_region() 预先数出来，
 * 超了说明计数器和装载器对不上——那是装载器的 bug，不是模块被拒。
 * 所以给它们单独的码，别和「模块非法」混在一起。 */
static bool reserve_insts(Loader *L, uint32_t count) {
  if (L->image->inst_count + count > L->image->inst_cap) {
    load_fail(L, LAINVM_IMAGE_ERR_INST_COUNT, "image: instruction count does not match the module");
    return false;
  }
  return true;
}

static bool push_operand(Loader *L, LainVmOperandRef ref, uint32_t *out) {
  if (L->image->operand_count >= L->image->operand_cap) {
    load_fail(L, LAINVM_IMAGE_ERR_OPERAND_COUNT, "image: operand count does not match the module");
    return false;
  }
  *out = L->image->operand_count;
  L->image->operands[L->image->operand_count++] = ref;
  return true;
}

static bool push_result(Loader *L, uint32_t slot, uint32_t *out) {
  if (L->image->result_count >= L->image->result_cap) {
    load_fail(L, LAINVM_IMAGE_ERR_RESULT_COUNT, "image: result count does not match the module");
    return false;
  }
  *out = L->image->result_count;
  L->image->results[L->image->result_count++] = slot;
  return true;
}

static uint32_t add_region(Loader *L, const L1Region *region, uint32_t parent,
                           uint32_t depth, const L1Param *extra_params,
                           uint32_t extra_count) {
  uint32_t id;
  LainVmImageRegion *rec;
  uint32_t pos;

  if (L->image->region_count >= L->image->region_cap) {
    load_fail(L, LAINVM_IMAGE_ERR_REGION_COUNT, "image: region count does not match the module");
    return LAINVM_IMAGE_NO_REGION;
  }
  id = L->image->region_count++;
  rec = &L->image->regions[id];
  memset(rec, 0, sizeof(*rec));
  rec->region = region;
  rec->parent = parent;
  rec->extra_params = extra_params;
  rec->extra_count = extra_count;
  rec->param_slots = extra_count + (region ? region->param_count : 0);
  rec->meta_base = L->image->inst_count;
  rec->inst_count = region ? region->inst_count : 0;
  rec->init_base = L->image->operand_count;
  if (!reserve_insts(L, rec->inst_count)) return LAINVM_IMAGE_NO_REGION;
  L->image->inst_count += rec->inst_count;

  if (depth > L->image->max_region_depth) L->image->max_region_depth = depth;

  if (!region) return id;

  /* 循环参数的初值：在父区域的作用域里解析。 */
  {
    uint32_t k;
    for (k = 0; k < region->param_count; k++) {
      const L1Operand *init = &region->params[k].init;
      LainVmOperandRef ref;
      uint32_t pushed = 0;
      ref.is_literal = init->kind != OPERAND_VALUE;
      ref.frame_distance = 0;
      ref.slot = 0;
      if (!ref.is_literal &&
          !resolve_name(L, parent, init->name, &ref.frame_distance, &ref.slot)) {
        load_fail(L, LAINVM_IMAGE_ERR_LOOP_INIT, "image: undefined loop initial value");
        return LAINVM_IMAGE_NO_REGION;
      }
      if (!push_operand(L, ref, &pushed)) return LAINVM_IMAGE_NO_REGION;
    }
  }

  /* 槽数：参数在前，然后按指令顺序的结果 */
  {
    uint32_t slots = rec->param_slots;
    uint32_t i;
    for (i = 0; i < region->inst_count; i++)
      slots += region->insts[i].result_count;
    rec->slot_count = slots;
    if (slots > L->image->max_slots) L->image->max_slots = slots;
  }

  /* 先递归：子区域拿到 id 之后，父指令的元数据才填得上子区域号。 */
  for (pos = 0; pos < region->inst_count; pos++) {
    const L1Inst *inst = &region->insts[pos];
    LainVmImageInstMeta *meta = &L->image->insts[rec->meta_base + pos];
    uint32_t i;

    memset(meta, 0, sizeof(*meta));
    meta->then_region = LAINVM_IMAGE_NO_REGION;
    meta->else_region = LAINVM_IMAGE_NO_REGION;
    meta->loop_region = LAINVM_IMAGE_NO_REGION;
    meta->default_region = LAINVM_IMAGE_NO_REGION;
    meta->symbol_index = LAINVM_IMAGE_NO_INDEX;
    meta->sub_index = LAINVM_IMAGE_NO_INDEX;
    meta->case_base = LAINVM_IMAGE_NO_INDEX;

    if (inst->kind == INST_LOOP) {
      if (inst->body)
        meta->loop_region = add_region(L, inst->body, id, depth + 1, NULL, 0);
    } else {
      if (inst->body) meta->then_region = add_region(L, inst->body, id, depth + 1, NULL, 0);
      if (inst->else_body)
        meta->else_region =
            add_region(L, inst->else_body, id, depth + 1, NULL, 0);
    }
    if (inst->default_case)
      meta->default_region =
          add_region(L, inst->default_case, id, depth + 1, NULL, 0);
    if (inst->case_count > 0) {
      if (L->image->case_count + inst->case_count > L->image->case_cap) {
        load_fail(L, LAINVM_IMAGE_ERR_CASE_COUNT, "image: case count does not match the module");
        return LAINVM_IMAGE_NO_REGION;
      }
      meta->case_base = L->image->case_count;
      for (i = 0; i < inst->case_count; i++) {
        L->image->case_regions[L->image->case_count++] =
            add_region(L, inst->cases[i].body, id, depth + 1, NULL, 0);
      }
    }
    if (L->failed) return LAINVM_IMAGE_NO_REGION;

    /* 操作数引用 */
    if (inst->operand_count > 0) {
      uint32_t base = L->image->operand_count;
      for (i = 0; i < inst->operand_count; i++) {
        const L1Operand *op = &inst->operands[i];
        LainVmOperandRef ref;
        uint32_t pushed = 0;
        if (op->kind == OPERAND_VALUE) {
          ref.is_literal = false;
          ref.slot = 0;
          ref.frame_distance = 0;
          if (!op->name ||
              !resolve_name(L, id, op->name, &ref.frame_distance, &ref.slot)) {
            load_fail(L, LAINVM_IMAGE_ERR_UNDEFINED_VALUE, "image: undefined value");
            return LAINVM_IMAGE_NO_REGION;
          }
        } else {
          ref.is_literal = true;
          ref.frame_distance = 0;
          ref.slot = 0;
        }
        if (!push_operand(L, ref, &pushed)) return LAINVM_IMAGE_NO_REGION;
      }
      meta->operand_base = base;
    }

    /* 结果槽 */
    if (inst->result_count > 0) {
      uint32_t slot = rec->param_slots;
      uint32_t q;
      uint32_t base = L->image->result_count;
      for (q = 0; q < pos; q++) slot += region->insts[q].result_count;
      for (i = 0; i < inst->result_count; i++) {
        uint32_t pushed = 0;
        if (!push_result(L, slot + i, &pushed))
          return LAINVM_IMAGE_NO_REGION;
      }
      meta->result_base = base;
    }
  }
  return id;
}

/* --- 符号 ------------------------------------------------------------------ */

static uint32_t add_symbol(Loader *L, const char *symbol, uintptr_t addr,
                           uint32_t size, uint32_t rights) {
  uint32_t index;
  if (L->image->symbol_count >= L->image->symbol_cap) {
    load_fail(L, LAINVM_IMAGE_ERR_SYMBOL_COUNT, "image: symbol count does not match the module");
    return LAINVM_IMAGE_NO_INDEX;
  }
  index = L->image->symbol_count++;
  L->image->symbols[index].symbol = symbol;
  L->image->symbols[index].addr = addr;
  L->image->symbols[index].size = size;
  L->image->symbols[index].rights = rights;
  return index;
}

/* 按**内容**比，不能按指针比：解析器给数据声明和指令里的符号各 intern 了一份，
 * 指针相同只是 builder 手动传同一个字面量时的巧合。 */
static uint32_t find_symbol(const LainVmImage *image, const char *symbol) {
  uint32_t i;
  if (!symbol) return LAINVM_IMAGE_NO_INDEX;
  for (i = 0; i < image->symbol_count; i++) {
    const char *candidate = image->symbols[i].symbol;
    if (candidate && strcmp(candidate, symbol) == 0) return i;
  }
  return LAINVM_IMAGE_NO_INDEX;
}

static uint32_t find_sub(const LainVmImage *image, const char *name) {
  uint32_t i;
  for (i = 0; i < image->sub_count; i++) {
    if (image->subs[i].sub->name && name &&
        strcmp(image->subs[i].sub->name, name) == 0)
      return i;
  }
  return LAINVM_IMAGE_NO_INDEX;
}

/* 把模块数据映射进地址空间：只读一段、可写一段。
 * 调用方给了底座就用调用方的（映像不拥有、不释放），没给就自己分配。 */
static bool map_data(Loader *L, const L1Module *module,
                     const LainVmImagePlacement *placement) {
  uint64_t ro_size = 0;
  uint64_t rw_size = 0;
  uint32_t i;
  uint64_t ro_at = 0;
  uint64_t rw_at = 0;

  data_sizes(module, &ro_size, &rw_size);
  if (ro_size > 0) {
    if (placement && placement->ro_arena) {
      L->image->ro_arena = placement->ro_arena;
      L->image->owns_ro = false;
    } else {
      L->image->ro_arena = (uint8_t *)calloc(1, (size_t)ro_size);
      if (!L->image->ro_arena) {
        load_fail(L, LAINVM_IMAGE_ERR_RO_ALLOC, "image: cannot map read-only data");
        return false;
      }
      L->image->owns_ro = true;
    }
    if (lainvm_space_handle_none(lainvm_space_map_external(
            L->image->space, (uintptr_t)L->image->ro_arena, ro_size, ro_size,
            LAINVM_MEM_READ, 0))) {
      load_fail(L, LAINVM_IMAGE_ERR_RO_MAP, "image: address space rejected read-only data");
      return false;
    }
  }
  if (rw_size > 0) {
    if (placement && placement->rw_arena) {
      L->image->rw_arena = placement->rw_arena;
      L->image->owns_rw = false;
    } else {
      L->image->rw_arena = (uint8_t *)calloc(1, (size_t)rw_size);
      if (!L->image->rw_arena) {
        load_fail(L, LAINVM_IMAGE_ERR_RW_ALLOC, "image: cannot map writable data");
        return false;
      }
      L->image->owns_rw = true;
    }
    if (lainvm_space_handle_none(lainvm_space_map_external(
            L->image->space, (uintptr_t)L->image->rw_arena, rw_size, rw_size,
            LAINVM_MEM_READ | LAINVM_MEM_WRITE, 0))) {
      load_fail(L, LAINVM_IMAGE_ERR_RW_MAP, "image: address space rejected writable data");
      return false;
    }
  }

  for (i = 0; i < module->data_count; i++) {
    const L1Data *data = &module->data[i];
    uintptr_t addr;
    uint32_t rights;
    if (data->is_writable) {
      addr = (uintptr_t)(L->image->rw_arena + rw_at);
      rights = LAINVM_MEM_READ | LAINVM_MEM_WRITE;
      if (data->bytes && data->size) memcpy(L->image->rw_arena + rw_at, data->bytes, data->size);
      rw_at += data->size;
    } else {
      addr = (uintptr_t)(L->image->ro_arena + ro_at);
      rights = LAINVM_MEM_READ;
      if (data->bytes && data->size) memcpy(L->image->ro_arena + ro_at, data->bytes, data->size);
      ro_at += data->size;
    }
    if (add_symbol(L, data->symbol, addr, data->size, rights) ==
        LAINVM_IMAGE_NO_INDEX)
      return false;
  }
  return true;
}

/* 装载后把指令里的符号换成下标：运行时不许碰字符串。 */
static void resolve_symbols(LainVmImage *image) {
  uint32_t i;
  for (i = 0; i < image->region_count; i++) {
    const LainVmImageRegion *rec = &image->regions[i];
    uint32_t pos;
    for (pos = 0; pos < rec->inst_count; pos++) {
      const L1Inst *inst = &rec->region->insts[pos];
      LainVmImageInstMeta *meta = &image->insts[rec->meta_base + pos];
      if (inst->kind == INST_DATA_ADDR)
        meta->symbol_index = find_symbol(image, inst->symbol);
      else if (inst->kind == INST_CALL || inst->kind == INST_PROC_ADDR)
        meta->sub_index = find_sub(image, inst->symbol);
      else if (inst->kind == INST_CALL_INDIRECT)
        meta->sub_index = LAINVM_IMAGE_NO_INDEX;
    }
  }
}

/* --- 按模块量尺寸 ----------------------------------------------------------
 *
 * 装载器在递归里会持有 regions[] / insts[] 的指针，所以不能边装边长
 * （realloc 会把它们挪走）。改成一趟纯计数的先序遍历，先把容量数准，
 * 再一次性分配——分配之后映像的尺寸就固定了。
 * ------------------------------------------------------------------------- */

typedef struct {
  uint32_t regions;
  uint32_t insts;
  uint32_t operands;
  uint32_t results;
  uint32_t cases;
} ImageTotals;

/* 必须和 add_region 的 push 次数逐项对上：
 *   每个区域 +1；+param_count 个循环参数初值；每个区域 +inst_count 条指令；
 *   每条指令 +operand_count 个操作数、+result_count 个结果、+case_count 个 case；
 *   子区域按 add_region 的递归顺序走一遍（loop 只有 body，其余有 body/else/default）。 */
static void count_region(const L1Region *region, ImageTotals *t) {
  uint32_t pos;
  uint32_t i;
  if (!region) return;
  t->regions++;
  t->insts += region->inst_count;
  t->operands += region->param_count;
  for (pos = 0; pos < region->inst_count; pos++) {
    const L1Inst *inst = &region->insts[pos];
    t->operands += inst->operand_count;
    t->results += inst->result_count;
    t->cases += inst->case_count;
    if (inst->kind == INST_LOOP) {
      count_region(inst->body, t);
    } else {
      count_region(inst->body, t);
      count_region(inst->else_body, t);
    }
    count_region(inst->default_case, t);
    for (i = 0; i < inst->case_count; i++) count_region(inst->cases[i].body, t);
  }
}

/* 数据段的字节数：只读一段、可写一段（.rodata / .data）。 */
static void data_sizes(const L1Module *module, uint64_t *ro_out,
                       uint64_t *rw_out) {
  uint64_t ro_size = 0;
  uint64_t rw_size = 0;
  uint32_t i;
  for (i = 0; i < module->data_count; i++) {
    if (module->data[i].is_writable)
      rw_size += module->data[i].size;
    else
      ro_size += module->data[i].size;
  }
  *ro_out = ro_size;
  *rw_out = rw_size;
}

/* 装载要的上界：最深的词法嵌套、单个区域最多的槽数。与 add_region 的记账同源
 * （根区域带子过程签名参数，嵌套区域不带），给 admit 算帧与槽的上限。 */
static void measure_region(const L1Region *region, uint32_t depth,
                           uint32_t extra_count, uint32_t *max_depth,
                           uint32_t *max_slots) {
  uint32_t slots;
  uint32_t pos;
  uint32_t i;
  if (!region) return;
  if (depth > *max_depth) *max_depth = depth;
  slots = extra_count + region->param_count;
  for (pos = 0; pos < region->inst_count; pos++)
    slots += region->insts[pos].result_count;
  if (slots > *max_slots) *max_slots = slots;
  for (pos = 0; pos < region->inst_count; pos++) {
    const L1Inst *inst = &region->insts[pos];
    if (inst->kind == INST_LOOP) {
      measure_region(inst->body, depth + 1, 0, max_depth, max_slots);
    } else {
      measure_region(inst->body, depth + 1, 0, max_depth, max_slots);
      measure_region(inst->else_body, depth + 1, 0, max_depth, max_slots);
    }
    measure_region(inst->default_case, depth + 1, 0, max_depth, max_slots);
    for (i = 0; i < inst->case_count; i++)
      measure_region(inst->cases[i].body, depth + 1, 0, max_depth, max_slots);
  }
}

/* 装载一个模块要多少地方。不分配、不映射、不碰地址空间。 */
static void measure_image(const L1Module *module, LainVmImageSizes *out) {
  ImageTotals t;
  uint32_t depth = 0;
  uint32_t slots = 0;
  uint32_t i;
  memset(&t, 0, sizeof(t));
  for (i = 0; i < module->subroutine_count; i++) {
    const L1Subroutine *sub = &module->subroutines[i];
    if ((sub->flags & SUBROUTINE_EXTERN) || !sub->body) continue;
    count_region(sub->body, &t);
    measure_region(sub->body, 1, sub->param_count, &depth, &slots);
  }
  memset(out, 0, sizeof(*out));
  out->region_count = t.regions;
  out->inst_count = t.insts;
  out->operand_count = t.operands;
  out->result_count = t.results;
  out->case_count = t.cases;
  out->symbol_count = module->data_count;
  out->sub_count = module->subroutine_count;
  data_sizes(module, &out->ro_bytes, &out->rw_bytes);
  out->code_bytes = sizeof(LainVmCodeEntry) * (uint64_t)module->subroutine_count;
  out->max_region_depth = depth;
  out->max_slots = slots;
}

int lainvm_image_size(const L1Module *module, LainVmImageSizes *out) {
  if (!module || !out) return LAINVM_IMAGE_ERR_ARG;
  measure_image(module, out);
  return 0;
}

/* 调用方给的底座够不够。只查它真会用到的那几块；NULL 的由映像自己分配。 */
static bool placement_fits(const LainVmImagePlacement *placement,
                           const LainVmImageSizes *sizes) {
  if (placement->ro_arena && placement->ro_bytes < sizes->ro_bytes) return false;
  if (placement->rw_arena && placement->rw_bytes < sizes->rw_bytes) return false;
  if (placement->code_arena && placement->code_bytes < sizes->code_bytes)
    return false;
  return true;
}

/* 按 count 分配一块表；count 为 0 时返回 NULL 且不算失败。 */
static void *alloc_table(uint32_t count, size_t elem_size, Loader *L,
                         int code) {
  void *table;
  if (count == 0) return NULL;
  table = calloc((size_t)count, elem_size);
  if (!table) load_fail(L, code, "image: cannot allocate the image tables");
  return table;
}

/* 按模块自己的计数一次分配各张表：装载完就不再变。计数与公开的
 * lainvm_image_size 同源（measure_image），所以这里没有第二套算法。 */
static bool size_image(Loader *L, const L1Module *module) {
  LainVmImageSizes sizes;
  LainVmImage *image = L->image;

  measure_image(module, &sizes);
  image->region_cap = sizes.region_count;
  image->inst_cap = sizes.inst_count;
  image->operand_cap = sizes.operand_count;
  image->result_cap = sizes.result_count;
  image->case_cap = sizes.case_count;
  image->symbol_cap = sizes.symbol_count;
  image->sub_cap = sizes.sub_count;

  image->regions = (LainVmImageRegion *)alloc_table(
      sizes.region_count, sizeof(LainVmImageRegion), L, LAINVM_IMAGE_ERR_TABLE_ALLOC);
  if (sizes.region_count && !image->regions) return false;
  image->insts =
      (LainVmImageInstMeta *)alloc_table(sizes.inst_count, sizeof(LainVmImageInstMeta), L,
                                         LAINVM_IMAGE_ERR_TABLE_ALLOC);
  if (sizes.inst_count && !image->insts) return false;
  image->operands = (LainVmOperandRef *)alloc_table(
      sizes.operand_count, sizeof(LainVmOperandRef), L, LAINVM_IMAGE_ERR_TABLE_ALLOC);
  if (sizes.operand_count && !image->operands) return false;
  image->results = (uint32_t *)alloc_table(sizes.result_count, sizeof(uint32_t), L, LAINVM_IMAGE_ERR_TABLE_ALLOC);
  if (sizes.result_count && !image->results) return false;
  image->case_regions =
      (uint32_t *)alloc_table(sizes.case_count, sizeof(uint32_t), L, LAINVM_IMAGE_ERR_TABLE_ALLOC);
  if (sizes.case_count && !image->case_regions) return false;
  image->symbols = (LainVmImageSymbol *)alloc_table(
      image->symbol_cap, sizeof(LainVmImageSymbol), L, LAINVM_IMAGE_ERR_TABLE_ALLOC);
  if (image->symbol_cap && !image->symbols) return false;
  image->subs = (LainVmImageSub *)alloc_table(image->sub_cap,
                                              sizeof(LainVmImageSub), L, LAINVM_IMAGE_ERR_TABLE_ALLOC);
  if (image->sub_cap && !image->subs) return false;
  return true;
}

/* 失败路径：码已经在 loader 里（diag 为 NULL 时也不丢），收回映像并报出去。 */
static int admit_fail(Loader *L, LainVmImage *image, LainVmImage **out) {
  int code = L->code ? L->code : LAINVM_IMAGE_ERR_TABLE_ALLOC;
  lainvm_image_free(image);
  *out = NULL;
  return code;
}

int lainvm_image_admit(const L1Module *module, LainVmSpace *space,
                       const LainVmImagePlacement *placement,
                       LainVmImage **out, L1Diagnostic *diag) {
  LainVmImageSizes sizes;
  LainVmImage *image;
  Loader loader;
  uint32_t i;

  if (!module || !space || !out) return LAINVM_IMAGE_ERR_ARG;
  *out = NULL;
  measure_image(module, &sizes);
  if (placement && !placement_fits(placement, &sizes))
    return LAINVM_IMAGE_ERR_PLACEMENT;

  image = (LainVmImage *)calloc(1, sizeof(LainVmImage));
  if (!image) return LAINVM_IMAGE_ERR_TABLE_ALLOC;
  image->module = module;
  image->space = space;

  loader.image = image;
  loader.diag = diag;
  loader.code = 0;
  loader.failed = false;
  if (diag) {
    diag->code = 0;
    diag->message[0] = '\0';
  }

  if (!size_image(&loader, module)) return admit_fail(&loader, image, out);
  if (!map_data(&loader, module, placement))
    return admit_fail(&loader, image, out);

  /* 代码段：每条子过程一条记录。权限是 READ|CALL——可执行，不可写。 */
  if (module->subroutine_count > 0) {
    size_t bytes = sizeof(LainVmCodeEntry) * (size_t)module->subroutine_count;
    if (placement && placement->code_arena) {
      image->code_arena = (LainVmCodeEntry *)placement->code_arena;
      image->owns_code = false;
    } else {
      image->code_arena = (LainVmCodeEntry *)calloc(1, bytes);
      if (!image->code_arena) {
        load_fail(&loader, LAINVM_IMAGE_ERR_CODE_ALLOC, "image: cannot map code");
        return admit_fail(&loader, image, out);
      }
      image->owns_code = true;
    }
    if (lainvm_space_handle_none(lainvm_space_map_external(
            image->space, (uintptr_t)image->code_arena, (uint64_t)bytes,
            (uint64_t)bytes, LAINVM_MEM_READ | LAINVM_MEM_CALL, 0))) {
      load_fail(&loader, LAINVM_IMAGE_ERR_CODE_MAP, "image: address space rejected code");
      return admit_fail(&loader, image, out);
    }
    for (i = 0; i < module->subroutine_count; i++) {
      image->code_arena[i].sub_index = i;
      image->code_arena[i].reserved = 0; /* 调用方给的底座不是清零过的 */
      image->subs[i].entry_addr = (uintptr_t)&image->code_arena[i];
    }
  }

  for (i = 0; i < module->subroutine_count; i++) {
    const L1Subroutine *sub = &module->subroutines[i];
    uint32_t body = LAINVM_IMAGE_NO_REGION;
    uint32_t first = image->region_count;
    uint32_t r;
    if (!(sub->flags & SUBROUTINE_EXTERN) && sub->body)
      body = add_region(&loader, sub->body, LAINVM_IMAGE_NO_REGION, 1,
                        sub->params, sub->param_count);
    if (loader.failed) return admit_fail(&loader, image, out);
    for (r = first; r < image->region_count; r++) image->regions[r].sub_index = i;
    image->subs[i].sub = sub;
    image->subs[i].body_region = body;
    image->sub_count = i + 1;
  }

  resolve_symbols(image);
  *out = image;
  return 0;
}

/* 便捷包装：底座全由映像自己分配。 */
LainVmImage *lainvm_image_load(const L1Module *module, LainVmSpace *space,
                               L1Diagnostic *diag) {
  LainVmImage *image = NULL;
  if (lainvm_image_admit(module, space, NULL, &image, diag) != 0) return NULL;
  return image;
}

void lainvm_image_free(LainVmImage *image) {
  if (!image) return;
  if (image->owns_ro) free(image->ro_arena);
  if (image->owns_rw) free(image->rw_arena);
  if (image->owns_code) free(image->code_arena);
  free(image->regions);
  free(image->insts);
  free(image->operands);
  free(image->results);
  free(image->case_regions);
  free(image->symbols);
  free(image->subs);
  free(image);
}

const LainVmImageRegion *lainvm_image_region(const LainVmImage *image,
                                             uint32_t region_id) {
  if (!image || region_id >= image->region_count) return NULL;
  return &image->regions[region_id];
}

const L1Inst *lainvm_image_inst(const LainVmImage *image, uint32_t region_id,
                                uint32_t position) {
  const LainVmImageRegion *rec = lainvm_image_region(image, region_id);
  if (!rec || position >= rec->inst_count) return NULL;
  return &rec->region->insts[position];
}

LainVmImageInstMeta lainvm_image_inst_meta(const LainVmImage *image,
                                           uint32_t region_id,
                                           uint32_t position) {
  LainVmImageInstMeta empty;
  const LainVmImageRegion *rec = lainvm_image_region(image, region_id);
  memset(&empty, 0xFF, sizeof(empty));
  if (!rec || position >= rec->inst_count) return empty;
  return image->insts[rec->meta_base + position];
}

LainVmOperandRef lainvm_image_operand_ref(const LainVmImage *image,
                                          uint32_t region_id, uint32_t position,
                                          uint32_t operand_index) {
  LainVmOperandRef ref;
  LainVmImageInstMeta meta;
  memset(&ref, 0, sizeof(ref));
  meta = lainvm_image_inst_meta(image, region_id, position);
  if (meta.operand_base == LAINVM_IMAGE_NO_INDEX) return ref;
  return image->operands[meta.operand_base + operand_index];
}

uint32_t lainvm_image_result_slot(const LainVmImage *image, uint32_t region_id,
                                  uint32_t position, uint32_t result_index) {
  LainVmImageInstMeta meta = lainvm_image_inst_meta(image, region_id, position);
  if (meta.result_base == LAINVM_IMAGE_NO_INDEX) return LAINVM_IMAGE_NO_INDEX;
  return image->results[meta.result_base + result_index];
}

const L1Subroutine *lainvm_image_entry(const LainVmImage *image,
                                       const char *name,
                                       uint32_t *body_region_out) {
  uint32_t index;
  if (!image) return NULL;
  index = find_sub(image, name);
  if (index == LAINVM_IMAGE_NO_INDEX) return NULL;
  if (body_region_out) *body_region_out = image->subs[index].body_region;
  return image->subs[index].sub;
}

uintptr_t lainvm_image_symbol_addr(const LainVmImage *image,
                                   uint32_t symbol_index) {
  if (!image || symbol_index >= image->symbol_count) return 0;
  return image->symbols[symbol_index].addr;
}

uintptr_t lainvm_image_sub_addr(const LainVmImage *image, uint32_t sub_index) {
  if (!image || sub_index >= image->sub_count) return 0;
  return image->subs[sub_index].entry_addr;
}

uint32_t lainvm_image_sub_at(const LainVmImage *image, uintptr_t addr) {
  uintptr_t base;
  uintptr_t offset;
  uint32_t index;
  if (!image || !image->code_arena) return LAINVM_IMAGE_NO_INDEX;
  base = (uintptr_t)image->code_arena;
  if (addr < base) return LAINVM_IMAGE_NO_INDEX;
  offset = addr - base;
  if (offset % sizeof(LainVmCodeEntry) != 0) return LAINVM_IMAGE_NO_INDEX;
  index = (uint32_t)(offset / sizeof(LainVmCodeEntry));
  if (index >= image->sub_count) return LAINVM_IMAGE_NO_INDEX;
  return image->code_arena[index].sub_index;
}

LainVmOperandRef lainvm_image_param_init_ref(const LainVmImage *image,
                                             uint32_t region_id,
                                             uint32_t param_index) {
  LainVmOperandRef ref;
  const LainVmImageRegion *rec = lainvm_image_region(image, region_id);
  memset(&ref, 0, sizeof(ref));
  if (!rec || param_index >= rec->region->param_count) return ref;
  return image->operands[rec->init_base + param_index];
}
