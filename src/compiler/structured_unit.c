/* Opaque structured L1Unit arena used by the Lain-written compiler.
 *
 * This layer owns only physical L1 storage and validation/execution.  It has
 * no knowledge of source bindings, modules, types aliases, or Meta policy.
 */

#include "compiler/structured_unit.h"
#include "compiler/vm_compat.h"
#include "lainir/lainir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  STRUCTURED_OBJECT_PROC = 1,
  STRUCTURED_OBJECT_EXPR = 2,
  STRUCTURED_OBJECT_INST = 3,
  STRUCTURED_OBJECT_BLOCK = 4,
} StructuredObjectKind;

typedef struct {
  uint32_t id;
  StructuredObjectKind kind;
  void *pointer;
} StructuredObject;

typedef struct StructuredUnit {
  uint32_t id;
  L1Subroutine *head;
  L1Subroutine *tail;
  StructuredObject *objects;
  uint32_t object_count;
  uint32_t object_capacity;
  L1Type **types;
  uint32_t type_count;
  uint32_t type_capacity;
  struct StructuredUnit *next;
} StructuredUnit;

static StructuredUnit *g_structured_units;
static uint32_t g_next_unit_id = 1;
static uint32_t g_next_object_id = 1;

static char *structured_strdup(const char *text);
static const char *structured_text(sexp value);

// Opaque physical slot storage for the Lain-owned compiler context.  Lain
// owns vector length, capacity, growth policy, symbol-table layout, scope
// rules, and diagnostic policy.  This host object only provides stable raw
// slots whose requested capacity is chosen by Lain.
typedef struct CompilerStorage {
  uint32_t id;
  uint32_t kind; /* 1 = i32, 2 = string */
  uint32_t capacity;
  int32_t *integers;
  char **strings;
  struct CompilerStorage *next;
} CompilerStorage;

static CompilerStorage *g_compiler_storages;
static uint32_t g_next_compiler_storage_id = 1;

static CompilerStorage *compiler_storage_find(uint32_t id) {
  for (CompilerStorage *storage = g_compiler_storages; storage;
       storage = storage->next)
    if (storage->id == id) return storage;
  return NULL;
}

uint32_t structured_compiler_storage_new(uint32_t kind) {
  CompilerStorage *storage = calloc(1, sizeof(CompilerStorage));
  if (!storage || (kind != 1 && kind != 2)) {
    free(storage);
    return 0;
  }
  storage->kind = kind;
  storage->id = g_next_compiler_storage_id++;
  if (storage->id == 0) storage->id = g_next_compiler_storage_id++;
  storage->next = g_compiler_storages;
  g_compiler_storages = storage;
  return storage->id;
}

int structured_compiler_storage_reserve(uint32_t id, uint32_t capacity) {
  CompilerStorage *storage = compiler_storage_find(id);
  if (!storage) return 0;
  if (capacity <= storage->capacity) return 1;
  if (storage->kind == 1) {
    int32_t *items = realloc(storage->integers,
                             sizeof(int32_t) * (size_t)capacity);
    if (!items) return 0;
    memset(items + storage->capacity, 0,
           sizeof(int32_t) * (size_t)(capacity - storage->capacity));
    storage->integers = items;
  } else {
    char **items = realloc(storage->strings,
                           sizeof(char *) * (size_t)capacity);
    if (!items) return 0;
    memset(items + storage->capacity, 0,
           sizeof(char *) * (size_t)(capacity - storage->capacity));
    storage->strings = items;
  }
  storage->capacity = capacity;
  return 1;
}

int structured_compiler_storage_set_string(
    uint32_t id, uint32_t index, const char *value) {
  CompilerStorage *storage = compiler_storage_find(id);
  char *copy;
  if (!storage || storage->kind != 2 || index >= storage->capacity)
    return 0;
  copy = structured_strdup(value ? value : "");
  if (!copy) return 0;
  free(storage->strings[index]);
  storage->strings[index] = copy;
  return 1;
}

int structured_compiler_storage_set_i32(
    uint32_t id, uint32_t index, int32_t value) {
  CompilerStorage *storage = compiler_storage_find(id);
  if (!storage || storage->kind != 1 || index >= storage->capacity)
    return 0;
  storage->integers[index] = value;
  return 1;
}

int structured_compiler_storage_get_i32(
    uint32_t id, uint32_t index, int32_t *value_out) {
  CompilerStorage *storage = compiler_storage_find(id);
  if (!storage || storage->kind != 1 || index >= storage->capacity ||
      !value_out)
    return 0;
  *value_out = storage->integers[index];
  return 1;
}

const char *structured_compiler_storage_get_string(
    uint32_t id, uint32_t index) {
  CompilerStorage *storage = compiler_storage_find(id);
  if (!storage || storage->kind != 2 || index >= storage->capacity ||
      !storage->strings[index])
    return NULL;
  return storage->strings[index];
}

int structured_compiler_storage_destroy(uint32_t id) {
  CompilerStorage **link = &g_compiler_storages;
  while (*link && (*link)->id != id) link = &(*link)->next;
  if (!*link) return 0;
  CompilerStorage *storage = *link;
  *link = storage->next;
  if (storage->strings)
    for (uint32_t i = 0; i < storage->capacity; ++i)
      free(storage->strings[i]);
  free(storage->strings);
  free(storage->integers);
  free(storage);
  return 1;
}

static sexp compiler_storage_new(
    sexp ctx, sexp self, sexp_sint_t n, sexp kind_value) {
  uint32_t id;
  (void)ctx;
  (void)self;
  (void)n;
  id = structured_compiler_storage_new(
      (uint32_t)sexp_unbox_fixnum(kind_value));
  return sexp_make_fixnum(id);
}

static sexp compiler_storage_reserve(
    sexp ctx, sexp self, sexp_sint_t n, sexp storage_value,
    sexp capacity_value) {
  uint32_t id = (uint32_t)sexp_unbox_fixnum(storage_value);
  uint32_t capacity = (uint32_t)sexp_unbox_fixnum(capacity_value);
  (void)ctx;
  (void)self;
  (void)n;
  return sexp_make_fixnum(
      structured_compiler_storage_reserve(id, capacity));
}

static sexp compiler_storage_set_i32(
    sexp ctx, sexp self, sexp_sint_t n, sexp storage_value,
    sexp index_value, sexp item_value) {
  uint32_t id = (uint32_t)sexp_unbox_fixnum(storage_value);
  uint32_t index = (uint32_t)sexp_unbox_fixnum(index_value);
  (void)ctx;
  (void)self;
  (void)n;
  return sexp_make_fixnum(structured_compiler_storage_set_i32(
      id, index, (int32_t)sexp_unbox_fixnum(item_value)));
}

static sexp compiler_storage_get_i32(
    sexp ctx, sexp self, sexp_sint_t n, sexp storage_value,
    sexp index_value) {
  uint32_t id = (uint32_t)sexp_unbox_fixnum(storage_value);
  uint32_t index = (uint32_t)sexp_unbox_fixnum(index_value);
  int32_t value = 0;
  (void)ctx;
  (void)self;
  (void)n;
  if (!structured_compiler_storage_get_i32(id, index, &value))
    return sexp_make_fixnum(0);
  return sexp_make_fixnum(value);
}

static sexp compiler_storage_set_string(
    sexp ctx, sexp self, sexp_sint_t n, sexp storage_value,
    sexp index_value, sexp item_value) {
  uint32_t id = (uint32_t)sexp_unbox_fixnum(storage_value);
  uint32_t index = (uint32_t)sexp_unbox_fixnum(index_value);
  const char *item = structured_text(item_value);
  (void)ctx;
  (void)self;
  (void)n;
  return sexp_make_fixnum(
      structured_compiler_storage_set_string(id, index, item));
}

static sexp compiler_storage_get_string(
    sexp ctx, sexp self, sexp_sint_t n, sexp storage_value,
    sexp index_value) {
  uint32_t id = (uint32_t)sexp_unbox_fixnum(storage_value);
  uint32_t index = (uint32_t)sexp_unbox_fixnum(index_value);
  const char *value;
  (void)self;
  (void)n;
  value = structured_compiler_storage_get_string(id, index);
  return sexp_c_string(ctx, value ? value : "", -1);
}

static sexp compiler_storage_destroy(
    sexp ctx, sexp self, sexp_sint_t n, sexp storage_value) {
  uint32_t id = (uint32_t)sexp_unbox_fixnum(storage_value);
  (void)ctx;
  (void)self;
  (void)n;
  return sexp_make_fixnum(structured_compiler_storage_destroy(id));
}

static char *structured_strdup(const char *text) {
  size_t length = strlen(text);
  char *copy = malloc(length + 1);
  if (copy) memcpy(copy, text, length + 1);
  return copy;
}

static const char *structured_text(sexp value) {
  if (sexp_stringp(value)) return sexp_string_data(value);
  return "";
}

static StructuredUnit *structured_find_unit(uint32_t id) {
  for (StructuredUnit *unit = g_structured_units; unit; unit = unit->next)
    if (unit->id == id) return unit;
  return NULL;
}

static void *structured_find_object(
    StructuredUnit *unit, uint32_t id, StructuredObjectKind kind) {
  if (!unit || id == 0) return NULL;
  for (uint32_t i = 0; i < unit->object_count; i++)
    if (unit->objects[i].id == id && unit->objects[i].kind == kind)
      return unit->objects[i].pointer;
  return NULL;
}

static void *structured_find_object_global(
    uint32_t id, StructuredObjectKind kind, StructuredUnit **owner) {
  for (StructuredUnit *unit = g_structured_units; unit; unit = unit->next) {
    void *pointer = structured_find_object(unit, id, kind);
    if (pointer) {
      if (owner) *owner = unit;
      return pointer;
    }
  }
  return NULL;
}

static uint32_t structured_add_object(
    StructuredUnit *unit, StructuredObjectKind kind, void *pointer) {
  if (!unit || !pointer) return 0;
  if (unit->object_count == unit->object_capacity) {
    uint32_t capacity = unit->object_capacity ? unit->object_capacity * 2 : 32;
    StructuredObject *objects = realloc(
        unit->objects, sizeof(StructuredObject) * capacity);
    if (!objects) return 0;
    unit->objects = objects;
    unit->object_capacity = capacity;
  }
  uint32_t id = g_next_object_id++;
  if (id == 0) id = g_next_object_id++;
  unit->objects[unit->object_count++] =
      (StructuredObject){id, kind, pointer};
  return id;
}

static uint32_t structured_object_id(
    StructuredUnit *unit, StructuredObjectKind kind, const void *pointer) {
  if (!unit || !pointer) return 0;
  for (uint32_t i = 0; i < unit->object_count; i++)
    if (unit->objects[i].kind == kind &&
        unit->objects[i].pointer == pointer)
      return unit->objects[i].id;
  return 0;
}

static L1Type *structured_type(StructuredUnit *unit, uint32_t tag) {
  L1TypeKind kind = TY_BITS;
  uint32_t width = 32;
  if (tag >= 101 && tag <= 132) width = tag - 100;
  if (tag == 2) width = 1;
  if (tag == 3) { kind = TY_ADDR; width = 64; }
  if (tag == 4) { kind = TY_UNIT; width = 0; }
  L1Type *type = lainir_new_type(kind, width);
  if (!type) return NULL;
  if (unit->type_count == unit->type_capacity) {
    uint32_t capacity = unit->type_capacity ? unit->type_capacity * 2 : 32;
    L1Type **types = realloc(unit->types, sizeof(L1Type *) * capacity);
    if (!types) { free(type); return NULL; }
    unit->types = types;
    unit->type_capacity = capacity;
  }
  unit->types[unit->type_count++] = type;
  return type;
}

static sexp structured_unit_new(
    sexp ctx, sexp self, sexp_sint_t n) {
  StructuredUnit *unit = calloc(1, sizeof(StructuredUnit));
  if (!unit) return sexp_make_fixnum(0);
  unit->id = g_next_unit_id++;
  if (unit->id == 0) unit->id = g_next_unit_id++;
  unit->next = g_structured_units;
  g_structured_units = unit;
  return sexp_make_fixnum(unit->id);
}

static sexp structured_proc_new(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp unit_value, sexp name_value, sexp return_tag_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  if (!unit) return sexp_make_fixnum(0);
  L1Subroutine *sub = lainir_new_subroutine(structured_text(name_value));
  if (!sub) return sexp_make_fixnum(0);
  sub->ret_ty = structured_type(unit, sexp_unbox_fixnum(return_tag_value));
  sub->blocks = sub->blocks_tail = lainir_new_block();
  if (!sub->ret_ty || !sub->blocks) { free(sub); return sexp_make_fixnum(0); }
  sub->blocks->parent = sub;
  if (unit->tail) unit->tail->next = sub;
  else unit->head = sub;
  unit->tail = sub;
  return sexp_make_fixnum(
      structured_add_object(unit, STRUCTURED_OBJECT_PROC, sub));
}

static sexp structured_proc_extern(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp unit_value, sexp name_value, sexp return_tag_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  if (!unit) return sexp_make_fixnum(0);
  L1Subroutine *sub = lainir_new_subroutine(structured_text(name_value));
  if (!sub) return sexp_make_fixnum(0);
  sub->ret_ty = structured_type(unit, sexp_unbox_fixnum(return_tag_value));
  sub->is_extern = 1;
  if (!sub->ret_ty) { free(sub); return sexp_make_fixnum(0); }
  if (unit->tail) unit->tail->next = sub;
  else unit->head = sub;
  unit->tail = sub;
  return sexp_make_fixnum(
      structured_add_object(unit, STRUCTURED_OBJECT_PROC, sub));
}

static sexp structured_proc_param(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp unit_value, sexp proc_value, sexp type_tag_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Subroutine *sub = structured_find_object(
      unit, sexp_unbox_fixnum(proc_value), STRUCTURED_OBJECT_PROC);
  if (!sub) return sexp_make_fixnum(0);
  L1Type **types = realloc(
      sub->param_tys, sizeof(L1Type *) * (sub->param_count + 1));
  if (!types) return sexp_make_fixnum(0);
  sub->param_tys = types;
  sub->param_tys[sub->param_count] = structured_type(
      unit, sexp_unbox_fixnum(type_tag_value));
  if (!sub->param_tys[sub->param_count]) return sexp_make_fixnum(0);
  sub->param_count++;
  return sexp_make_fixnum(1);
}

static sexp structured_proc_body(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp unit_value, sexp proc_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Subroutine *sub = structured_find_object(
      unit, sexp_unbox_fixnum(proc_value), STRUCTURED_OBJECT_PROC);
  if (!sub || !sub->blocks) return sexp_make_fixnum(0);
  uint32_t old = structured_object_id(
      unit, STRUCTURED_OBJECT_BLOCK, sub->blocks);
  if (old) return sexp_make_fixnum(old);
  return sexp_make_fixnum(structured_add_object(
      unit, STRUCTURED_OBJECT_BLOCK, sub->blocks));
}

static uint32_t structured_register_expr(StructuredUnit *unit, L1Expr *expr) {
  return structured_add_object(unit, STRUCTURED_OBJECT_EXPR, expr);
}

static sexp structured_expr_i32(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  if (!unit) return sexp_make_fixnum(0);
  L1Expr *expr = lainir_new_expr(EXPR_CONST);
  if (!expr) return sexp_make_fixnum(0);
  expr->data.const_val = sexp_unbox_fixnum(value);
  return sexp_make_fixnum(structured_register_expr(unit, expr));
}

static sexp structured_expr_string(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  if (!unit) return sexp_make_fixnum(0);
  L1Expr *expr = lainir_new_expr(EXPR_STRING);
  if (!expr) return sexp_make_fixnum(0);
  expr->data.str_val.content = structured_strdup(structured_text(value));
  expr->data.str_val.ty = structured_type(unit, 3);
  return sexp_make_fixnum(structured_register_expr(unit, expr));
}

static sexp structured_expr_arg(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp unit_value, sexp index_value, sexp type_tag_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  if (!unit) return sexp_make_fixnum(0);
  L1Expr *expr = lainir_new_expr(EXPR_ARG);
  if (!expr) return sexp_make_fixnum(0);
  expr->data.arg.index = sexp_unbox_fixnum(index_value);
  expr->data.arg.ty = structured_type(unit, sexp_unbox_fixnum(type_tag_value));
  return sexp_make_fixnum(structured_register_expr(unit, expr));
}

static sexp structured_expr_var(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp unit_value, sexp name_value, sexp type_tag_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  if (!unit) return sexp_make_fixnum(0);
  L1Expr *expr = lainir_new_expr(EXPR_VAR);
  if (!expr) return sexp_make_fixnum(0);
  expr->data.var.name = structured_strdup(structured_text(name_value));
  expr->data.var.ty = structured_type(unit, sexp_unbox_fixnum(type_tag_value));
  return sexp_make_fixnum(structured_register_expr(unit, expr));
}

static sexp structured_expr_binary(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp op_value,
    sexp left_value, sexp right_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Expr *left = structured_find_object(
      unit, sexp_unbox_fixnum(left_value), STRUCTURED_OBJECT_EXPR);
  L1Expr *right = structured_find_object(
      unit, sexp_unbox_fixnum(right_value), STRUCTURED_OBJECT_EXPR);
  if (!left || !right) return sexp_make_fixnum(0);
  uint32_t op = sexp_unbox_fixnum(op_value);
  L1ExprKind kind = op == 1 ? EXPR_ADD :
      op == 2 ? EXPR_EQ : op == 3 ? EXPR_NE :
      op == 4 ? EXPR_LT : op == 5 ? EXPR_LE :
      op == 6 ? EXPR_GT : op == 7 ? EXPR_GE :
      op == 8 ? EXPR_SUB : EXPR_MUL;
  L1Expr *expr = lainir_new_expr(kind);
  if (!expr) return sexp_make_fixnum(0);
  expr->data.bin.left = left;
  expr->data.bin.right = right;
  return sexp_make_fixnum(structured_register_expr(unit, expr));
}

static sexp structured_expr_call(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp unit_value, sexp name_value, sexp return_tag_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  if (!unit) return sexp_make_fixnum(0);
  L1Expr *expr = lainir_new_expr(EXPR_CALL);
  if (!expr) return sexp_make_fixnum(0);
  expr->data.call.fn_name = structured_strdup(structured_text(name_value));
  expr->data.call.ret_ty = structured_type(
      unit, sexp_unbox_fixnum(return_tag_value));
  return sexp_make_fixnum(structured_register_expr(unit, expr));
}

static sexp structured_expr_alloca(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp unit_value, sexp byte_size_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  if (!unit) return sexp_make_fixnum(0);
  L1Expr *expr = lainir_new_expr(EXPR_ALLOCA);
  if (!expr) return sexp_make_fixnum(0);
  expr->data.alloca.element_ty = structured_type(unit, 1);
  expr->data.alloca.byte_size = (uint32_t)sexp_unbox_fixnum(byte_size_value);
  expr->data.alloca.result_ty = structured_type(unit, 3);
  if (!expr->data.alloca.element_ty || !expr->data.alloca.result_ty)
    return sexp_make_fixnum(0);
  return sexp_make_fixnum(structured_register_expr(unit, expr));
}

static sexp structured_expr_lea(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp unit_value, sexp base_value, sexp offset_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Expr *base = structured_find_object(
      unit, sexp_unbox_fixnum(base_value), STRUCTURED_OBJECT_EXPR);
  if (!base) return sexp_make_fixnum(0);
  L1Expr *expr = lainir_new_expr(EXPR_LEA);
  if (!expr) return sexp_make_fixnum(0);
  expr->data.lea.base = base;
  expr->data.lea.idx = NULL;
  expr->data.lea.scale = 1;
  expr->data.lea.offset = (uint32_t)sexp_unbox_fixnum(offset_value);
  return sexp_make_fixnum(structured_register_expr(unit, expr));
}

static sexp structured_expr_load(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp unit_value, sexp addr_value, sexp type_tag_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Expr *addr = structured_find_object(
      unit, sexp_unbox_fixnum(addr_value), STRUCTURED_OBJECT_EXPR);
  if (!addr) return sexp_make_fixnum(0);
  L1Expr *expr = lainir_new_expr(EXPR_LOAD);
  if (!expr) return sexp_make_fixnum(0);
  expr->data.load.addr = addr;
  expr->data.load.ty = structured_type(
      unit, sexp_unbox_fixnum(type_tag_value));
  expr->data.load.ordering = MEM_ORDER_RELAXED;
  if (!expr->data.load.ty) return sexp_make_fixnum(0);
  return sexp_make_fixnum(structured_register_expr(unit, expr));
}

static sexp structured_call_arg(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp unit_value, sexp call_value, sexp arg_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Expr *call = structured_find_object(
      unit, sexp_unbox_fixnum(call_value), STRUCTURED_OBJECT_EXPR);
  L1Expr *arg = structured_find_object(
      unit, sexp_unbox_fixnum(arg_value), STRUCTURED_OBJECT_EXPR);
  if (!call || call->kind != EXPR_CALL || !arg) return sexp_make_fixnum(0);
  L1Expr **args = realloc(
      call->data.call.args,
      sizeof(L1Expr *) * (call->data.call.arg_count + 1));
  if (!args) return sexp_make_fixnum(0);
  call->data.call.args = args;
  call->data.call.args[call->data.call.arg_count++] = arg;
  return sexp_make_fixnum(1);
}

static sexp structured_proc_let(
    sexp ctx, sexp self, sexp_sint_t n, sexp proc_value,
    sexp name_value, sexp type_tag_value, sexp expr_value) {
  StructuredUnit *unit = NULL;
  L1Subroutine *sub = structured_find_object_global(
      sexp_unbox_fixnum(proc_value), STRUCTURED_OBJECT_PROC, &unit);
  L1Expr *expr = structured_find_object(
      unit, sexp_unbox_fixnum(expr_value), STRUCTURED_OBJECT_EXPR);
  if (!sub || !expr) return sexp_make_fixnum(0);
  L1Instruction *inst = lainir_new_instruction(INST_LET);
  if (!inst) return sexp_make_fixnum(0);
  inst->data.let.name = structured_strdup(structured_text(name_value));
  inst->data.let.ty = structured_type(unit, sexp_unbox_fixnum(type_tag_value));
  inst->data.let.val = expr;
  if (!structured_add_object(unit, STRUCTURED_OBJECT_INST, inst)) {
    free(inst->data.let.name);
    free(inst);
    return sexp_make_fixnum(0);
  }
  append_inst_to_block(sub->blocks, inst);
  return sexp_make_fixnum(1);
}

static sexp structured_proc_return(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp unit_value, sexp proc_value, sexp expr_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  uint32_t expr_id = (uint32_t)sexp_unbox_fixnum(expr_value);
  L1Subroutine *sub = structured_find_object(
      unit, sexp_unbox_fixnum(proc_value), STRUCTURED_OBJECT_PROC);
  L1Expr *expr = expr_id == 0 ? NULL : structured_find_object(
      unit, expr_id, STRUCTURED_OBJECT_EXPR);
  if (!sub || (expr_id != 0 && !expr)) return sexp_make_fixnum(0);
  L1Instruction *inst = lainir_new_instruction(INST_RETURN);
  if (!inst) return sexp_make_fixnum(0);
  inst->data.ret.val = expr;
  if (!structured_add_object(unit, STRUCTURED_OBJECT_INST, inst)) {
    free(inst);
    return sexp_make_fixnum(0);
  }
  append_inst_to_block(sub->blocks, inst);
  return sexp_make_fixnum(1);
}

static sexp structured_proc_store(
    sexp ctx, sexp self, sexp_sint_t n, sexp proc_value,
    sexp dest_value, sexp expr_value, sexp type_tag_value) {
  StructuredUnit *unit = NULL;
  L1Subroutine *sub = structured_find_object_global(
      sexp_unbox_fixnum(proc_value), STRUCTURED_OBJECT_PROC, &unit);
  L1Expr *dest = structured_find_object(
      unit, sexp_unbox_fixnum(dest_value), STRUCTURED_OBJECT_EXPR);
  L1Expr *expr = structured_find_object(
      unit, sexp_unbox_fixnum(expr_value), STRUCTURED_OBJECT_EXPR);
  if (!sub || !dest || !expr) return sexp_make_fixnum(0);
  L1Instruction *inst = lainir_new_instruction(INST_STORE);
  if (!inst) return sexp_make_fixnum(0);
  inst->data.store.dest = dest;
  inst->data.store.val = expr;
  inst->data.store.store_ty = structured_type(
      unit, sexp_unbox_fixnum(type_tag_value));
  inst->data.store.ordering = MEM_ORDER_RELAXED;
  if (!inst->data.store.store_ty ||
      !structured_add_object(unit, STRUCTURED_OBJECT_INST, inst))
    return sexp_make_fixnum(0);
  append_inst_to_block(sub->blocks, inst);
  return sexp_make_fixnum(1);
}

static L1Block *structured_return_block(
    StructuredUnit *unit, L1Subroutine *sub, L1Expr *expr) {
  L1Block *block = lainir_new_block();
  L1Instruction *ret = lainir_new_instruction(INST_RETURN);
  if (!block || !ret) { free(block); free(ret); return NULL; }
  block->parent = sub;
  ret->data.ret.val = expr;
  if (!structured_add_object(unit, STRUCTURED_OBJECT_INST, ret)) {
    free(ret);
    free(block);
    return NULL;
  }
  append_inst_to_block(block, ret);
  return block;
}

static sexp structured_proc_if_return(
    sexp ctx, sexp self, sexp_sint_t n, sexp proc_value,
    sexp condition_value, sexp then_value, sexp else_value) {
  StructuredUnit *unit = NULL;
  L1Subroutine *sub = structured_find_object_global(
      sexp_unbox_fixnum(proc_value), STRUCTURED_OBJECT_PROC, &unit);
  L1Expr *condition = structured_find_object(
      unit, sexp_unbox_fixnum(condition_value), STRUCTURED_OBJECT_EXPR);
  L1Expr *then_expr = structured_find_object(
      unit, sexp_unbox_fixnum(then_value), STRUCTURED_OBJECT_EXPR);
  L1Expr *else_expr = structured_find_object(
      unit, sexp_unbox_fixnum(else_value), STRUCTURED_OBJECT_EXPR);
  if (!sub || !condition || !then_expr || !else_expr) return sexp_make_fixnum(0);
  L1Instruction *inst = lainir_new_instruction(INST_IF);
  if (!inst) return sexp_make_fixnum(0);
  inst->data.if_stmt.condition = condition;
  inst->data.if_stmt.then_body = structured_return_block(unit, sub, then_expr);
  inst->data.if_stmt.else_body = structured_return_block(unit, sub, else_expr);
  if (!inst->data.if_stmt.then_body || !inst->data.if_stmt.else_body)
    return sexp_make_fixnum(0);
  if (!structured_add_object(unit, STRUCTURED_OBJECT_INST, inst))
    return sexp_make_fixnum(0);
  append_inst_to_block(sub->blocks, inst);
  return sexp_make_fixnum(1);
}

static sexp structured_proc_if_return_then(
    sexp ctx, sexp self, sexp_sint_t n, sexp proc_value,
    sexp condition_value, sexp then_value) {
  StructuredUnit *unit = NULL;
  L1Subroutine *sub = structured_find_object_global(
      sexp_unbox_fixnum(proc_value), STRUCTURED_OBJECT_PROC, &unit);
  L1Expr *condition = structured_find_object(
      unit, sexp_unbox_fixnum(condition_value), STRUCTURED_OBJECT_EXPR);
  L1Expr *then_expr = structured_find_object(
      unit, sexp_unbox_fixnum(then_value), STRUCTURED_OBJECT_EXPR);
  if (!sub || !condition || !then_expr) return sexp_make_fixnum(0);
  L1Instruction *inst = lainir_new_instruction(INST_IF);
  if (!inst) return sexp_make_fixnum(0);
  inst->data.if_stmt.condition = condition;
  inst->data.if_stmt.then_body = structured_return_block(unit, sub, then_expr);
  inst->data.if_stmt.else_body = NULL;
  if (!inst->data.if_stmt.then_body ||
      !structured_add_object(unit, STRUCTURED_OBJECT_INST, inst))
    return sexp_make_fixnum(0);
  append_inst_to_block(sub->blocks, inst);
  return sexp_make_fixnum(1);
}

static sexp structured_block_if(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp block_value, sexp condition_value) {
  StructuredUnit *unit = NULL;
  L1Block *block = structured_find_object_global(
      sexp_unbox_fixnum(block_value), STRUCTURED_OBJECT_BLOCK, &unit);
  L1Expr *condition = structured_find_object(
      unit, sexp_unbox_fixnum(condition_value), STRUCTURED_OBJECT_EXPR);
  if (!block || !condition) return sexp_make_fixnum(0);
  L1Instruction *inst = lainir_new_instruction(INST_IF);
  L1Block *then_block = lainir_new_block();
  if (!inst || !then_block) return sexp_make_fixnum(0);
  then_block->parent = block->parent;
  inst->data.if_stmt.condition = condition;
  inst->data.if_stmt.then_body = then_block;
  inst->data.if_stmt.else_body = NULL;
  if (!structured_add_object(unit, STRUCTURED_OBJECT_INST, inst))
    return sexp_make_fixnum(0);
  uint32_t block_id = structured_add_object(
      unit, STRUCTURED_OBJECT_BLOCK, then_block);
  if (!block_id) return sexp_make_fixnum(0);
  append_inst_to_block(block, inst);
  return sexp_make_fixnum(block_id);
}

static sexp structured_block_else(
    sexp ctx, sexp self, sexp_sint_t n, sexp then_block_value) {
  StructuredUnit *unit = NULL;
  L1Block *then_block = structured_find_object_global(
      sexp_unbox_fixnum(then_block_value), STRUCTURED_OBJECT_BLOCK, &unit);
  if (!then_block || !unit) return sexp_make_fixnum(0);
  for (uint32_t i = 0; i < unit->object_count; i++) {
    StructuredObject *object = &unit->objects[i];
    if (object->kind != STRUCTURED_OBJECT_INST) continue;
    L1Instruction *inst = object->pointer;
    if (!inst || inst->kind != INST_IF) continue;
    if (inst->data.if_stmt.then_body != then_block) continue;
    if (inst->data.if_stmt.else_body) {
      return sexp_make_fixnum(structured_object_id(
          unit, STRUCTURED_OBJECT_BLOCK, inst->data.if_stmt.else_body));
    }
    L1Block *else_block = lainir_new_block();
    if (!else_block) return sexp_make_fixnum(0);
    else_block->parent = then_block->parent;
    uint32_t block_id = structured_add_object(
        unit, STRUCTURED_OBJECT_BLOCK, else_block);
    if (!block_id) { free(else_block); return sexp_make_fixnum(0); }
    inst->data.if_stmt.else_body = else_block;
    return sexp_make_fixnum(block_id);
  }
  return sexp_make_fixnum(0);
}

static sexp structured_block_let(
    sexp ctx, sexp self, sexp_sint_t n, sexp block_value,
    sexp name_value, sexp type_tag_value, sexp expr_value) {
  StructuredUnit *unit = NULL;
  L1Block *block = structured_find_object_global(
      sexp_unbox_fixnum(block_value), STRUCTURED_OBJECT_BLOCK, &unit);
  L1Expr *expr = structured_find_object(
      unit, sexp_unbox_fixnum(expr_value), STRUCTURED_OBJECT_EXPR);
  if (!block || !expr) return sexp_make_fixnum(0);
  L1Instruction *inst = lainir_new_instruction(INST_LET);
  if (!inst) return sexp_make_fixnum(0);
  inst->data.let.name = structured_strdup(structured_text(name_value));
  inst->data.let.ty = structured_type(unit, sexp_unbox_fixnum(type_tag_value));
  inst->data.let.val = expr;
  if (!inst->data.let.ty ||
      !structured_add_object(unit, STRUCTURED_OBJECT_INST, inst))
    return sexp_make_fixnum(0);
  append_inst_to_block(block, inst);
  return sexp_make_fixnum(1);
}

static sexp structured_block_return(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp block_value, sexp expr_value) {
  StructuredUnit *unit = NULL;
  uint32_t expr_id = (uint32_t)sexp_unbox_fixnum(expr_value);
  L1Block *block = structured_find_object_global(
      sexp_unbox_fixnum(block_value), STRUCTURED_OBJECT_BLOCK, &unit);
  L1Expr *expr = expr_id == 0 ? NULL : structured_find_object(
      unit, expr_id, STRUCTURED_OBJECT_EXPR);
  if (!block || (expr_id != 0 && !expr)) return sexp_make_fixnum(0);
  L1Instruction *inst = lainir_new_instruction(INST_RETURN);
  if (!inst) return sexp_make_fixnum(0);
  inst->data.ret.val = expr;
  if (!structured_add_object(unit, STRUCTURED_OBJECT_INST, inst))
    return sexp_make_fixnum(0);
  append_inst_to_block(block, inst);
  return sexp_make_fixnum(1);
}

static sexp structured_block_call(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp block_value, sexp expr_value) {
  StructuredUnit *unit = NULL;
  L1Block *block = structured_find_object_global(
      sexp_unbox_fixnum(block_value), STRUCTURED_OBJECT_BLOCK, &unit);
  L1Expr *expr = structured_find_object(
      unit, sexp_unbox_fixnum(expr_value), STRUCTURED_OBJECT_EXPR);
  if (!block || !expr || expr->kind != EXPR_CALL)
    return sexp_make_fixnum(0);
  L1Instruction *inst = lainir_new_instruction(INST_CALL);
  if (!inst) return sexp_make_fixnum(0);
  inst->data.call_inst.expr = expr;
  if (!structured_add_object(unit, STRUCTURED_OBJECT_INST, inst))
    return sexp_make_fixnum(0);
  append_inst_to_block(block, inst);
  return sexp_make_fixnum(1);
}

static sexp structured_block_store(
    sexp ctx, sexp self, sexp_sint_t n, sexp block_value,
    sexp dest_value, sexp expr_value, sexp type_tag_value) {
  StructuredUnit *unit = NULL;
  L1Block *block = structured_find_object_global(
      sexp_unbox_fixnum(block_value), STRUCTURED_OBJECT_BLOCK, &unit);
  L1Expr *dest = structured_find_object(
      unit, sexp_unbox_fixnum(dest_value), STRUCTURED_OBJECT_EXPR);
  L1Expr *expr = structured_find_object(
      unit, sexp_unbox_fixnum(expr_value), STRUCTURED_OBJECT_EXPR);
  if (!block || !dest || !expr) return sexp_make_fixnum(0);
  L1Instruction *inst = lainir_new_instruction(INST_STORE);
  if (!inst) return sexp_make_fixnum(0);
  inst->data.store.dest = dest;
  inst->data.store.val = expr;
  inst->data.store.store_ty = structured_type(
      unit, sexp_unbox_fixnum(type_tag_value));
  inst->data.store.ordering = MEM_ORDER_RELAXED;
  if (!inst->data.store.store_ty ||
      !structured_add_object(unit, STRUCTURED_OBJECT_INST, inst))
    return sexp_make_fixnum(0);
  append_inst_to_block(block, inst);
  return sexp_make_fixnum(1);
}

static sexp structured_block_set(
    sexp ctx, sexp self, sexp_sint_t n, sexp block_value,
    sexp name_value, sexp type_tag_value, sexp expr_value) {
  StructuredUnit *unit = NULL;
  L1Block *block = structured_find_object_global(
      sexp_unbox_fixnum(block_value), STRUCTURED_OBJECT_BLOCK, &unit);
  L1Expr *expr = structured_find_object(
      unit, sexp_unbox_fixnum(expr_value), STRUCTURED_OBJECT_EXPR);
  if (!block || !expr) return sexp_make_fixnum(0);
  L1Instruction *inst = lainir_new_instruction(INST_SET);
  if (!inst) return sexp_make_fixnum(0);
  inst->data.set.name = structured_strdup(structured_text(name_value));
  inst->data.set.ty = structured_type(
      unit, sexp_unbox_fixnum(type_tag_value));
  inst->data.set.val = expr;
  if (!inst->data.set.name || !inst->data.set.ty ||
      !structured_add_object(unit, STRUCTURED_OBJECT_INST, inst)) {
    free(inst->data.set.name);
    free(inst);
    return sexp_make_fixnum(0);
  }
  append_inst_to_block(block, inst);
  return sexp_make_fixnum(1);
}

static sexp structured_block_loop(
    sexp ctx, sexp self, sexp_sint_t n, sexp block_value) {
  StructuredUnit *unit = NULL;
  L1Block *block = structured_find_object_global(
      sexp_unbox_fixnum(block_value), STRUCTURED_OBJECT_BLOCK, &unit);
  if (!block || !unit) return sexp_make_fixnum(0);
  L1Instruction *inst = lainir_new_instruction(INST_LOOP);
  L1Block *body = lainir_new_block();
  if (!inst || !body) { free(inst); free(body); return sexp_make_fixnum(0); }
  body->parent = block->parent;
  inst->data.loop.body = body;
  inst->data.loop.label = NULL;
  if (!structured_add_object(unit, STRUCTURED_OBJECT_INST, inst)) {
    free(body);
    free(inst);
    return sexp_make_fixnum(0);
  }
  uint32_t body_id = structured_add_object(
      unit, STRUCTURED_OBJECT_BLOCK, body);
  if (!body_id) { free(body); free(inst); return sexp_make_fixnum(0); }
  append_inst_to_block(block, inst);
  return sexp_make_fixnum(body_id);
}

static sexp structured_block_jump(
    sexp block_value, L1InstKind kind) {
  StructuredUnit *unit = NULL;
  L1Block *block = structured_find_object_global(
      sexp_unbox_fixnum(block_value), STRUCTURED_OBJECT_BLOCK, &unit);
  if (!block || !unit) return sexp_make_fixnum(0);
  L1Instruction *inst = lainir_new_instruction(kind);
  if (!inst) return sexp_make_fixnum(0);
  inst->data.jump.label = NULL;
  if (!structured_add_object(unit, STRUCTURED_OBJECT_INST, inst)) {
    free(inst);
    return sexp_make_fixnum(0);
  }
  append_inst_to_block(block, inst);
  return sexp_make_fixnum(1);
}

static sexp structured_block_break(
    sexp ctx, sexp self, sexp_sint_t n, sexp block_value) {
  return structured_block_jump(block_value, INST_BREAK);
}

static sexp structured_block_continue(
    sexp ctx, sexp self, sexp_sint_t n, sexp block_value) {
  return structured_block_jump(block_value, INST_CONTINUE);
}

/* Read-only physical L1Unit ABI.  The stable numeric tags below describe
 * node shape only; evaluation policy belongs to l1_interpreter.lain. */

static sexp structured_read_find_proc(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp name_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  const char *name = structured_text(name_value);
  if (!unit) return sexp_make_fixnum(0);
  for (L1Subroutine *sub = unit->head; sub; sub = sub->next)
    if (strcmp(sub->name, name) == 0)
      return sexp_make_fixnum(structured_object_id(
          unit, STRUCTURED_OBJECT_PROC, sub));
  return sexp_make_fixnum(0);
}

static sexp structured_read_proc_is_extern(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp proc_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Subroutine *sub = structured_find_object(
      unit, sexp_unbox_fixnum(proc_value), STRUCTURED_OBJECT_PROC);
  return sexp_make_fixnum(sub && sub->is_extern ? 1 : 0);
}

static sexp structured_read_proc_param_count(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp proc_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Subroutine *sub = structured_find_object(
      unit, sexp_unbox_fixnum(proc_value), STRUCTURED_OBJECT_PROC);
  return sexp_make_fixnum(sub ? sub->param_count : 0);
}

static sexp structured_read_proc_first_inst(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp proc_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Subroutine *sub = structured_find_object(
      unit, sexp_unbox_fixnum(proc_value), STRUCTURED_OBJECT_PROC);
  L1Instruction *inst = sub && sub->blocks ? sub->blocks->body : NULL;
  return sexp_make_fixnum(structured_object_id(
      unit, STRUCTURED_OBJECT_INST, inst));
}

static sexp structured_read_inst_next(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp inst_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Instruction *inst = structured_find_object(
      unit, sexp_unbox_fixnum(inst_value), STRUCTURED_OBJECT_INST);
  return sexp_make_fixnum(structured_object_id(
      unit, STRUCTURED_OBJECT_INST, inst ? inst->next : NULL));
}

static sexp structured_read_inst_kind(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp inst_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Instruction *inst = structured_find_object(
      unit, sexp_unbox_fixnum(inst_value), STRUCTURED_OBJECT_INST);
  if (!inst) return sexp_make_fixnum(0);
  if (inst->kind == INST_LET) return sexp_make_fixnum(1);
  if (inst->kind == INST_IF) return sexp_make_fixnum(2);
  if (inst->kind == INST_RETURN) return sexp_make_fixnum(3);
  return sexp_make_fixnum(255);
}

static sexp structured_read_inst_expr(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp inst_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Instruction *inst = structured_find_object(
      unit, sexp_unbox_fixnum(inst_value), STRUCTURED_OBJECT_INST);
  L1Expr *expr = NULL;
  if (inst && inst->kind == INST_LET) expr = inst->data.let.val;
  if (inst && inst->kind == INST_IF) expr = inst->data.if_stmt.condition;
  if (inst && inst->kind == INST_RETURN) expr = inst->data.ret.val;
  return sexp_make_fixnum(structured_object_id(
      unit, STRUCTURED_OBJECT_EXPR, expr));
}

static sexp structured_read_inst_name(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp inst_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Instruction *inst = structured_find_object(
      unit, sexp_unbox_fixnum(inst_value), STRUCTURED_OBJECT_INST);
  const char *name = inst && inst->kind == INST_LET ? inst->data.let.name : "";
  return sexp_c_string(ctx, name ? name : "", -1);
}

static sexp structured_read_inst_branch(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value,
    sexp inst_value, sexp branch_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Instruction *inst = structured_find_object(
      unit, sexp_unbox_fixnum(inst_value), STRUCTURED_OBJECT_INST);
  if (!inst || inst->kind != INST_IF) return sexp_make_fixnum(0);
  L1Block *block = sexp_unbox_fixnum(branch_value)
      ? inst->data.if_stmt.then_body : inst->data.if_stmt.else_body;
  return sexp_make_fixnum(structured_object_id(
      unit, STRUCTURED_OBJECT_INST, block ? block->body : NULL));
}

static sexp structured_read_expr_kind(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp expr_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Expr *expr = structured_find_object(
      unit, sexp_unbox_fixnum(expr_value), STRUCTURED_OBJECT_EXPR);
  if (!expr) return sexp_make_fixnum(0);
  if (expr->kind == EXPR_VAR) return sexp_make_fixnum(1);
  if (expr->kind == EXPR_CONST) return sexp_make_fixnum(2);
  if (expr->kind == EXPR_ARG) return sexp_make_fixnum(3);
  if (expr->kind == EXPR_ADD) return sexp_make_fixnum(4);
  if (expr->kind == EXPR_EQ) return sexp_make_fixnum(5);
  if (expr->kind == EXPR_NE) return sexp_make_fixnum(6);
  if (expr->kind == EXPR_CALL) return sexp_make_fixnum(7);
  if (expr->kind == EXPR_STRING) return sexp_make_fixnum(8);
  return sexp_make_fixnum(255);
}

static sexp structured_read_expr_i32(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp expr_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Expr *expr = structured_find_object(
      unit, sexp_unbox_fixnum(expr_value), STRUCTURED_OBJECT_EXPR);
  return sexp_make_fixnum(
      expr && expr->kind == EXPR_CONST ? expr->data.const_val : 0);
}

static sexp structured_read_expr_index(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp expr_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Expr *expr = structured_find_object(
      unit, sexp_unbox_fixnum(expr_value), STRUCTURED_OBJECT_EXPR);
  return sexp_make_fixnum(
      expr && expr->kind == EXPR_ARG ? expr->data.arg.index : 0);
}

static sexp structured_read_expr_name(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp expr_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Expr *expr = structured_find_object(
      unit, sexp_unbox_fixnum(expr_value), STRUCTURED_OBJECT_EXPR);
  const char *name = "";
  if (expr && expr->kind == EXPR_VAR) name = expr->data.var.name;
  if (expr && expr->kind == EXPR_CALL) name = expr->data.call.fn_name;
  return sexp_c_string(ctx, name ? name : "", -1);
}

static sexp structured_read_expr_child(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value,
    sexp expr_value, sexp right_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Expr *expr = structured_find_object(
      unit, sexp_unbox_fixnum(expr_value), STRUCTURED_OBJECT_EXPR);
  if (!expr) return sexp_make_fixnum(0);
  L1Expr *child = sexp_unbox_fixnum(right_value)
      ? expr->data.bin.right : expr->data.bin.left;
  return sexp_make_fixnum(structured_object_id(
      unit, STRUCTURED_OBJECT_EXPR, child));
}

static sexp structured_read_call_arg_count(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp expr_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Expr *expr = structured_find_object(
      unit, sexp_unbox_fixnum(expr_value), STRUCTURED_OBJECT_EXPR);
  return sexp_make_fixnum(
      expr && expr->kind == EXPR_CALL ? expr->data.call.arg_count : 0);
}

static sexp structured_read_call_arg(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value,
    sexp expr_value, sexp index_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Expr *expr = structured_find_object(
      unit, sexp_unbox_fixnum(expr_value), STRUCTURED_OBJECT_EXPR);
  uint32_t index = sexp_unbox_fixnum(index_value);
  if (!expr || expr->kind != EXPR_CALL || index >= expr->data.call.arg_count)
    return sexp_make_fixnum(0);
  return sexp_make_fixnum(structured_object_id(
      unit, STRUCTURED_OBJECT_EXPR, expr->data.call.args[index]));
}

typedef struct StructuredEvalContext StructuredEvalContext;

typedef struct StructuredEvalFrame {
  uint32_t id;
  StructuredEvalContext *owner;
  int32_t *args;
  uint8_t *arg_set;
  uint32_t arg_capacity;
  char **names;
  int32_t *values;
  uint32_t binding_count;
  uint32_t binding_capacity;
  struct StructuredEvalFrame *next;
} StructuredEvalFrame;

struct StructuredEvalContext {
  uint32_t id;
  int32_t status;
  StructuredEvalFrame *frames;
  struct StructuredEvalContext *next;
};

typedef struct StructuredEvalResult {
  uint32_t id;
  int32_t status;
  int32_t value;
  struct StructuredEvalResult *next;
} StructuredEvalResult;

static StructuredEvalContext *g_eval_contexts;
static StructuredEvalResult *g_eval_results;
static uint32_t g_next_eval_id = 1;
static uint32_t g_next_frame_id = 1;
static uint32_t g_next_result_id = 1;

static StructuredEvalContext *structured_eval_find(uint32_t id) {
  for (StructuredEvalContext *eval = g_eval_contexts; eval; eval = eval->next)
    if (eval->id == id) return eval;
  return NULL;
}

static StructuredEvalFrame *structured_frame_find(uint32_t id) {
  for (StructuredEvalContext *eval = g_eval_contexts; eval; eval = eval->next)
    for (StructuredEvalFrame *frame = eval->frames; frame; frame = frame->next)
      if (frame->id == id) return frame;
  return NULL;
}

static StructuredEvalResult *structured_result_find(uint32_t id) {
  for (StructuredEvalResult *result = g_eval_results; result; result = result->next)
    if (result->id == id) return result;
  return NULL;
}

static sexp structured_eval_new(sexp ctx, sexp self, sexp_sint_t n) {
  StructuredEvalContext *eval = calloc(1, sizeof(StructuredEvalContext));
  if (!eval) return sexp_make_fixnum(0);
  eval->id = g_next_eval_id++;
  eval->next = g_eval_contexts;
  g_eval_contexts = eval;
  return sexp_make_fixnum(eval->id);
}

static sexp structured_eval_frame_new(
    sexp ctx, sexp self, sexp_sint_t n, sexp eval_value) {
  StructuredEvalContext *eval = structured_eval_find(sexp_unbox_fixnum(eval_value));
  if (!eval) return sexp_make_fixnum(0);
  StructuredEvalFrame *frame = calloc(1, sizeof(StructuredEvalFrame));
  if (!frame) { eval->status = 7101; return sexp_make_fixnum(0); }
  frame->id = g_next_frame_id++;
  frame->owner = eval;
  frame->next = eval->frames;
  eval->frames = frame;
  return sexp_make_fixnum(frame->id);
}

static sexp structured_frame_arg_set(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp frame_value, sexp index_value, sexp value) {
  StructuredEvalFrame *frame = structured_frame_find(sexp_unbox_fixnum(frame_value));
  uint32_t index = sexp_unbox_fixnum(index_value);
  if (!frame) return sexp_make_fixnum(0);
  if (index >= frame->arg_capacity) {
    uint32_t capacity = frame->arg_capacity ? frame->arg_capacity : 4;
    while (capacity <= index) capacity *= 2;
    int32_t *args = calloc(capacity, sizeof(int32_t));
    uint8_t *set = calloc(capacity, 1);
    if (!args || !set) {
      free(args);
      free(set);
      frame->owner->status = 7101;
      return sexp_make_fixnum(0);
    }
    if (frame->arg_capacity) {
      memcpy(args, frame->args, sizeof(int32_t) * frame->arg_capacity);
      memcpy(set, frame->arg_set, frame->arg_capacity);
    }
    free(frame->args);
    free(frame->arg_set);
    frame->args = args;
    frame->arg_set = set;
    frame->arg_capacity = capacity;
  }
  frame->args[index] = sexp_unbox_fixnum(value);
  frame->arg_set[index] = 1;
  return sexp_make_fixnum(1);
}

static sexp structured_frame_arg_get(
    sexp ctx, sexp self, sexp_sint_t n, sexp frame_value, sexp index_value) {
  StructuredEvalFrame *frame = structured_frame_find(sexp_unbox_fixnum(frame_value));
  uint32_t index = sexp_unbox_fixnum(index_value);
  if (!frame || index >= frame->arg_capacity || !frame->arg_set[index]) {
    if (frame) frame->owner->status = 7006;
    return sexp_make_fixnum(0);
  }
  return sexp_make_fixnum(frame->args[index]);
}

static sexp structured_frame_var_set(
    sexp ctx, sexp self, sexp_sint_t n,
    sexp frame_value, sexp name_value, sexp value) {
  StructuredEvalFrame *frame = structured_frame_find(sexp_unbox_fixnum(frame_value));
  const char *name = structured_text(name_value);
  if (!frame) return sexp_make_fixnum(0);
  for (uint32_t i = 0; i < frame->binding_count; i++) {
    if (strcmp(frame->names[i], name) == 0) {
      frame->values[i] = sexp_unbox_fixnum(value);
      return sexp_make_fixnum(1);
    }
  }
  if (frame->binding_count == frame->binding_capacity) {
    uint32_t capacity = frame->binding_capacity ? frame->binding_capacity * 2 : 8;
    char **names = calloc(capacity, sizeof(char *));
    int32_t *values = calloc(capacity, sizeof(int32_t));
    if (!names || !values) {
      free(names);
      free(values);
      frame->owner->status = 7101;
      return sexp_make_fixnum(0);
    }
    if (frame->binding_count) {
      memcpy(names, frame->names, sizeof(char *) * frame->binding_count);
      memcpy(values, frame->values, sizeof(int32_t) * frame->binding_count);
    }
    free(frame->names);
    free(frame->values);
    frame->names = names;
    frame->values = values;
    frame->binding_capacity = capacity;
  }
  frame->names[frame->binding_count] = structured_strdup(name);
  if (!frame->names[frame->binding_count]) {
    frame->owner->status = 7101;
    return sexp_make_fixnum(0);
  }
  frame->values[frame->binding_count++] = sexp_unbox_fixnum(value);
  return sexp_make_fixnum(1);
}

static sexp structured_frame_var_get(
    sexp ctx, sexp self, sexp_sint_t n, sexp frame_value, sexp name_value) {
  StructuredEvalFrame *frame = structured_frame_find(sexp_unbox_fixnum(frame_value));
  const char *name = structured_text(name_value);
  if (!frame) return sexp_make_fixnum(0);
  for (uint32_t i = frame->binding_count; i > 0; i--)
    if (strcmp(frame->names[i - 1], name) == 0)
      return sexp_make_fixnum(frame->values[i - 1]);
  frame->owner->status = 7007;
  return sexp_make_fixnum(0);
}

static sexp structured_eval_fail(
    sexp ctx, sexp self, sexp_sint_t n, sexp eval_value, sexp status_value) {
  StructuredEvalContext *eval = structured_eval_find(sexp_unbox_fixnum(eval_value));
  if (!eval) return sexp_make_fixnum(0);
  if (eval->status == 0) eval->status = sexp_unbox_fixnum(status_value);
  return sexp_make_fixnum(1);
}

static sexp structured_eval_status(
    sexp ctx, sexp self, sexp_sint_t n, sexp eval_value) {
  StructuredEvalContext *eval = structured_eval_find(sexp_unbox_fixnum(eval_value));
  return sexp_make_fixnum(eval ? eval->status : 7102);
}

static void structured_frame_free(StructuredEvalFrame *frame) {
  while (frame) {
    StructuredEvalFrame *next = frame->next;
    for (uint32_t i = 0; i < frame->binding_count; i++) free(frame->names[i]);
    free(frame->names); free(frame->values);
    free(frame->args); free(frame->arg_set);
    free(frame);
    frame = next;
  }
}

static sexp structured_eval_destroy(
    sexp ctx, sexp self, sexp_sint_t n, sexp eval_value) {
  uint32_t id = sexp_unbox_fixnum(eval_value);
  StructuredEvalContext **link = &g_eval_contexts;
  while (*link && (*link)->id != id) link = &(*link)->next;
  if (!*link) return sexp_make_fixnum(0);
  StructuredEvalContext *eval = *link;
  *link = eval->next;
  structured_frame_free(eval->frames);
  free(eval);
  return sexp_make_fixnum(1);
}

static sexp structured_result_new(
    sexp ctx, sexp self, sexp_sint_t n, sexp status_value, sexp value) {
  StructuredEvalResult *result = calloc(1, sizeof(StructuredEvalResult));
  if (!result) return sexp_make_fixnum(0);
  result->id = g_next_result_id++;
  result->status = sexp_unbox_fixnum(status_value);
  result->value = sexp_unbox_fixnum(value);
  result->next = g_eval_results;
  g_eval_results = result;
  return sexp_make_fixnum(result->id);
}

static sexp structured_result_status(
    sexp ctx, sexp self, sexp_sint_t n, sexp result_value) {
  StructuredEvalResult *result = structured_result_find(sexp_unbox_fixnum(result_value));
  return sexp_make_fixnum(result ? result->status : 7103);
}

static sexp structured_result_value(
    sexp ctx, sexp self, sexp_sint_t n, sexp result_value) {
  StructuredEvalResult *result = structured_result_find(sexp_unbox_fixnum(result_value));
  return sexp_make_fixnum(result ? result->value : 0);
}

static sexp structured_result_destroy(
    sexp ctx, sexp self, sexp_sint_t n, sexp result_value) {
  uint32_t id = sexp_unbox_fixnum(result_value);
  StructuredEvalResult **link = &g_eval_results;
  while (*link && (*link)->id != id) link = &(*link)->next;
  if (!*link) return sexp_make_fixnum(0);
  StructuredEvalResult *result = *link;
  *link = result->next;
  free(result);
  return sexp_make_fixnum(1);
}

static sexp structured_unit_verify(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp entry_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Diagnostic diagnostic;
  if (!unit) return sexp_make_fixnum(0);
  return sexp_make_fixnum(lainir_verify_module(
      unit->head, structured_text(entry_value), &diagnostic) ? 1 : 0);
}

/* Return the verifier's stable diagnostic code instead of collapsing every
 * failure to false.  This is still a physical L1 capability: source-level
 * diagnostic policy remains in the Lain compiler. */
static sexp structured_unit_verify_code(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp entry_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Diagnostic diagnostic;
  (void)ctx;
  (void)self;
  (void)n;
  if (!unit) return sexp_make_fixnum(2099);
  if (lainir_verify_module(
          unit->head, structured_text(entry_value), &diagnostic))
    return sexp_make_fixnum(0);
  return sexp_make_fixnum(diagnostic.code ? diagnostic.code : 2098);
}

static sexp structured_unit_verify_message(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp entry_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Diagnostic diagnostic;
  (void)self;
  (void)n;
  if (!unit) return sexp_c_string(ctx, "unknown structured unit", -1);
  if (lainir_verify_module(
          unit->head, structured_text(entry_value), &diagnostic))
    return sexp_c_string(ctx, "", -1);
  return sexp_c_string(ctx, diagnostic.message, -1);
}

static sexp structured_unit_execute(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value, sexp entry_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  L1Diagnostic diagnostic;
  LainirValue result = lainir_value_unit();
  LainirRunRequest request;
  const char *error = NULL;
  if (!unit || !lainir_verify_module(
      unit->head, structured_text(entry_value), &diagnostic))
    return sexp_make_fixnum(0);
  memset(&request, 0, sizeof(request));
  request.module = unit->head;
  request.entry_name = structured_text(entry_value);
  if (lainir_run(&request, &result, &error) != LAINIR_RUN_OK ||
      result.kind != LAINIR_VALUE_BITS)
    return sexp_make_fixnum(0);
  return sexp_make_fixnum((sexp_sint_t)result.as.bits);
}

static sexp structured_unit_debug_text(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value) {
  StructuredUnit *unit = structured_find_unit(sexp_unbox_fixnum(unit_value));
  if (!unit) return sexp_c_string(ctx, "", -1);
  FILE *file = tmpfile();
  if (!file) return sexp_c_string(ctx, "", -1);
  lainir_emit_text_module(file, unit->head);
  long length = ftell(file);
  if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return sexp_c_string(ctx, "", -1);
  }
  char *text = malloc((size_t)length + 1);
  if (!text) { fclose(file); return sexp_c_string(ctx, "", -1); }
  size_t read = fread(text, 1, (size_t)length, file);
  text[read] = '\0';
  fclose(file);
  sexp result = sexp_c_string(ctx, text, (sexp_sint_t)read);
  free(text);
  return result;
}

static sexp structured_unit_destroy(
    sexp ctx, sexp self, sexp_sint_t n, sexp unit_value) {
  uint32_t id = sexp_unbox_fixnum(unit_value);
  StructuredUnit **link = &g_structured_units;
  while (*link && (*link)->id != id) link = &(*link)->next;
  if (!*link) return sexp_make_fixnum(0);
  StructuredUnit *unit = *link;
  *link = unit->next;
  lainir_free_subroutines(unit->head);
  for (uint32_t i = 0; i < unit->type_count; i++) free(unit->types[i]);
  free(unit->types);
  free(unit->objects);
  free(unit);
  return sexp_make_fixnum(1);
}

void native_register_structured_unit_ffi(
    void *ctx_ptr, void *env_ptr, native_foreign_registrar registrar,
    void *user_data) {
  (void)ctx_ptr;
  (void)env_ptr;
#define REG(name, args, fn) registrar(user_data, name, args, (void *)(fn))
  REG("compiler.storage-new!", 1, compiler_storage_new);
  REG("compiler.storage-reserve!", 2, compiler_storage_reserve);
  REG("compiler.storage-set-i32!", 3, compiler_storage_set_i32);
  REG("compiler.storage-get-i32!", 2, compiler_storage_get_i32);
  REG("compiler.storage-set-string!", 3, compiler_storage_set_string);
  REG("compiler.storage-get-string!", 2, compiler_storage_get_string);
  REG("compiler.storage-destroy!", 1, compiler_storage_destroy);
  REG("l1.unit-new!", 0, structured_unit_new);
  REG("l1.proc-new!", 3, structured_proc_new);
  REG("l1.proc-extern!", 3, structured_proc_extern);
  REG("l1.proc-param!", 3, structured_proc_param);
  REG("l1.proc-body!", 2, structured_proc_body);
  REG("l1.expr-i32!", 2, structured_expr_i32);
  REG("l1.expr-string!", 2, structured_expr_string);
  REG("l1.expr-arg!", 3, structured_expr_arg);
  REG("l1.expr-var!", 3, structured_expr_var);
  REG("l1.expr-binary!", 4, structured_expr_binary);
  REG("l1.expr-call!", 3, structured_expr_call);
  REG("l1.expr-alloca!", 2, structured_expr_alloca);
  REG("l1.expr-lea!", 3, structured_expr_lea);
  REG("l1.expr-load!", 3, structured_expr_load);
  REG("l1.call-arg!", 3, structured_call_arg);
  REG("l1.proc-let!", 4, structured_proc_let);
  REG("l1.proc-return!", 3, structured_proc_return);
  REG("l1.proc-store!", 4, structured_proc_store);
  REG("l1.proc-if-return!", 4, structured_proc_if_return);
  REG("l1.proc-if-return-then!", 3, structured_proc_if_return_then);
  REG("l1.block-if!", 2, structured_block_if);
  REG("l1.block-else!", 1, structured_block_else);
  REG("l1.block-let!", 4, structured_block_let);
  REG("l1.block-return!", 2, structured_block_return);
  REG("l1.block-call!", 2, structured_block_call);
  REG("l1.block-store!", 4, structured_block_store);
  REG("l1.block-set!", 4, structured_block_set);
  REG("l1.block-loop!", 1, structured_block_loop);
  REG("l1.block-break!", 1, structured_block_break);
  REG("l1.block-continue!", 1, structured_block_continue);
  REG("l1.unit-verify!", 2, structured_unit_verify);
  REG("l1.unit-verify-code!", 2, structured_unit_verify_code);
  REG("l1.unit-verify-message!", 2, structured_unit_verify_message);
  REG("l1.read-find-proc!", 2, structured_read_find_proc);
  REG("l1.read-proc-is-extern!", 2, structured_read_proc_is_extern);
  REG("l1.read-proc-param-count!", 2, structured_read_proc_param_count);
  REG("l1.read-proc-first-inst!", 2, structured_read_proc_first_inst);
  REG("l1.read-inst-next!", 2, structured_read_inst_next);
  REG("l1.read-inst-kind!", 2, structured_read_inst_kind);
  REG("l1.read-inst-expr!", 2, structured_read_inst_expr);
  REG("l1.read-inst-name!", 2, structured_read_inst_name);
  REG("l1.read-inst-branch!", 3, structured_read_inst_branch);
  REG("l1.read-expr-kind!", 2, structured_read_expr_kind);
  REG("l1.read-expr-i32!", 2, structured_read_expr_i32);
  REG("l1.read-expr-index!", 2, structured_read_expr_index);
  REG("l1.read-expr-name!", 2, structured_read_expr_name);
  REG("l1.read-expr-child!", 3, structured_read_expr_child);
  REG("l1.read-call-arg-count!", 2, structured_read_call_arg_count);
  REG("l1.read-call-arg!", 3, structured_read_call_arg);
  REG("l1.eval-new!", 0, structured_eval_new);
  REG("l1.eval-frame-new!", 1, structured_eval_frame_new);
  REG("l1.eval-frame-arg-set!", 3, structured_frame_arg_set);
  REG("l1.eval-frame-arg!", 2, structured_frame_arg_get);
  REG("l1.eval-frame-var-set!", 3, structured_frame_var_set);
  REG("l1.eval-frame-var!", 2, structured_frame_var_get);
  REG("l1.eval-fail!", 2, structured_eval_fail);
  REG("l1.eval-status!", 1, structured_eval_status);
  REG("l1.eval-destroy!", 1, structured_eval_destroy);
  REG("l1.result-new!", 2, structured_result_new);
  REG("l1.result-status!", 1, structured_result_status);
  REG("l1.result-value!", 1, structured_result_value);
  REG("l1.result-destroy!", 1, structured_result_destroy);
  REG("core.execute-lainir-unit!", 2, structured_unit_execute);
  REG("core.lainir-unit-debug-text!", 1, structured_unit_debug_text);
  REG("core.destroy-lainir-unit!", 1, structured_unit_destroy);
#undef REG
}
