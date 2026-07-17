/**
 * compiler/builder_ffi.c — L1 IR Builder FFI Functions
 *
 * Provides the C-side FFI functions that Scheme meta passes call to construct
 * L1 IR nodes (types, expressions, instructions, blocks, subroutines).
 *
 * Every function here allocates L1 IR nodes on the C heap and returns
 * cpointer values to Scheme.  These are the MUST_STAY C primitives
 * — they cannot move to Scheme because L1 IR is a C data structure.
 */

#include "lainir/lainir.h"
#include "compiler/native_runtime.h"
#include "compiler/vm_compat.h"
#include <stdlib.h>
#include <string.h>

static char *lain_strdup(const char *s) {
  size_t n = strlen(s);
  char *out = (char *)malloc(n + 1);
  if (!out) return NULL;
  memcpy(out, s, n + 1);
  return out;
}

#define strdup lain_strdup

uint32_t get_list_length(sexp list) {
  uint32_t len = 0;
  sexp curr = list;
  while (sexp_pairp(curr)) {
    len++;
    curr = sexp_cdr(curr);
  }
  return len;
}

// Convert Scheme symbol/string to C string
static const char *sexp_to_c_string(sexp ctx, sexp val) {
  if (sexp_symbolp(val))
    return sexp_string_data(sexp_symbol_to_string(ctx, val));
  if (sexp_stringp(val))
    return sexp_string_data(val);
  return "";
}


static sexp sexp_core_make_bits(sexp ctx, sexp self, sexp_sint_t n, sexp arg) {
  uint32_t width = sexp_unbox_fixnum(arg);
  L1Type *ty = malloc(sizeof(L1Type));
  ty->kind = TY_BITS;
  ty->width = width;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, ty, SEXP_FALSE, 0);
}

static sexp sexp_core_make_addr(sexp ctx, sexp self, sexp_sint_t n) {
  L1Type *ty = malloc(sizeof(L1Type));
  ty->kind = TY_ADDR;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, ty, SEXP_FALSE, 0);
}

static sexp sexp_core_make_unit(sexp ctx, sexp self, sexp_sint_t n) {
  L1Type *ty = malloc(sizeof(L1Type));
  ty->kind = TY_UNIT;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, ty, SEXP_FALSE, 0);
}

// -- make-product-type: returns addr (products are opaque pointers to L1) --
static sexp sexp_core_make_product_type(sexp ctx, sexp self, sexp_sint_t n,
                                         sexp arg_fields) {
  // Anonymous product types are just opaque pointers (addr) at L1 level.
  // Layout computation happens in Scheme (product-layout).
  L1Type *ty = malloc(sizeof(L1Type));
  ty->kind = TY_ADDR;
  ty->width = 64;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, ty, SEXP_FALSE, 0);
}

static sexp sexp_core_make_floats(sexp ctx, sexp self, sexp_sint_t n, sexp arg) {
  L1Type *ty = malloc(sizeof(L1Type));
  ty->kind = TY_FLOATS;
  ty->width = sexp_unbox_fixnum(arg);
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, ty, SEXP_FALSE, 0);
}

static sexp sexp_core_make_simd(sexp ctx, sexp self, sexp_sint_t n,
                                sexp arg_width, sexp arg_lanes) {
  L1Type *ty = malloc(sizeof(L1Type));
  ty->kind = TY_SIMD;
  ty->width = sexp_unbox_fixnum(arg_width);  // lanes encoded in width for now
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, ty, SEXP_FALSE, 0);
}

static sexp sexp_type_registered(sexp ctx, sexp self, sexp_sint_t n,
                                 sexp name_val) {
  const char *name = sexp_to_c_string(ctx, name_val);
  // Struct types are now handled in Scheme (struct-registered?) —
  // this only handles built-in L1 atoms.
  L1Type *ty = malloc(sizeof(L1Type));
  if (strcmp(name, "bool") == 0) {
    ty->kind = TY_BITS;
    ty->width = 1;
  } else if (strcmp(name, "i8") == 0 || strcmp(name, "u8") == 0) {
    ty->kind = TY_BITS;
    ty->width = 8;
  } else if (strcmp(name, "i16") == 0 || strcmp(name, "u16") == 0) {
    ty->kind = TY_BITS;
    ty->width = 16;
  } else if (strcmp(name, "i32") == 0 || strcmp(name, "u32") == 0) {
    ty->kind = TY_BITS;
    ty->width = 32;
  } else if (strcmp(name, "i64") == 0 || strcmp(name, "u64") == 0 ||
             strcmp(name, "usize") == 0) {
    ty->kind = TY_BITS;
    ty->width = 64;
  } else if (strcmp(name, "addr") == 0 || strcmp(name, "opaque") == 0 || strcmp(name, "CStr") == 0) {
    ty->kind = TY_ADDR;
  } else {
    ty->kind = TY_BITS;
    ty->width = 32;
  }
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, ty, SEXP_FALSE, 0);
}

static sexp sexp_core_make_set(sexp ctx, sexp self, sexp_sint_t n,
                               sexp arg_name, sexp arg_val) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  L1Expr *val = (L1Expr *)sexp_cpointer_value(arg_val);
  L1Instruction *inst = malloc(sizeof(L1Instruction));
  inst->kind = INST_SET;
  inst->data.set.name = strdup(name);
  inst->data.set.ty = NULL;
  inst->data.set.val = val;
  inst->next = NULL;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, inst, SEXP_FALSE, 0);
}

static sexp sexp_core_make_proc(sexp ctx, sexp self, sexp_sint_t n,
                                sexp arg_name, sexp arg_ret, sexp arg_params,
                                sexp arg_block) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  L1Type *ret_ty = (L1Type *)sexp_cpointer_value(arg_ret);
  uint32_t param_count = get_list_length(arg_params);
  L1Type **param_tys = malloc(sizeof(L1Type *) * param_count);
  sexp curr = arg_params;
  for (uint32_t i = 0; i < param_count; i++) {
    param_tys[i] = (L1Type *)sexp_cpointer_value(sexp_car(curr));
    curr = sexp_cdr(curr);
  }
  L1Subroutine *sub = malloc(sizeof(L1Subroutine));
  sub->name = strdup(name);
  sub->ret_ty = ret_ty;
  sub->param_count = param_count;
  sub->param_tys = param_tys;
  sub->blocks = NULL;
  sub->blocks_tail = NULL;
  sub->next = g_subroutines_head;
  g_subroutines_head = sub;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, sub, SEXP_FALSE, 0);
}

static sexp sexp_core_const_bits(sexp ctx, sexp self, sexp_sint_t n,
                                 sexp arg_block, sexp arg_ty, sexp arg_val) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Type *ty = (L1Type *)sexp_cpointer_value(arg_ty);
  int64_t val = sexp_unbox_fixnum(arg_val);
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_CONST;
  expr->data.const_val = val;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_const_string(sexp ctx, sexp self, sexp_sint_t n,
                                   sexp arg_block, sexp arg_ty, sexp arg_val) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Type *ty = (L1Type *)sexp_cpointer_value(arg_ty);
  const char *content = sexp_to_c_string(ctx, arg_val);
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_STRING;
  expr->data.str_val.content = strdup(content);
  expr->data.str_val.ty = ty;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_load(sexp ctx, sexp self, sexp_sint_t n, sexp arg_block,
                           sexp arg_addr, sexp arg_ty) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Expr *addr = (L1Expr *)sexp_cpointer_value(arg_addr);
  L1Type *ty = (L1Type *)sexp_cpointer_value(arg_ty);
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_LOAD;
  expr->data.load.addr = addr;
  expr->data.load.ty = ty;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_store(sexp ctx, sexp self, sexp_sint_t n, sexp arg_block,
                            sexp arg_dest, sexp arg_val) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Expr *dest = (L1Expr *)sexp_cpointer_value(arg_dest);
  L1Expr *val = (L1Expr *)sexp_cpointer_value(arg_val);
  L1Instruction *inst = malloc(sizeof(L1Instruction));
  inst->kind = INST_STORE;
  inst->data.store.dest = dest;
  inst->data.store.val = val;
  inst->data.store.store_ty = NULL;
  inst->next = NULL;
  append_inst_to_block(block, inst);
  return SEXP_VOID;
}

// LEA: base + offset (index=0, scale=1)
static sexp sexp_core_lea(sexp ctx, sexp self, sexp_sint_t n,
                          sexp arg_block, sexp arg_base, sexp arg_offset) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Expr *base = (L1Expr *)sexp_cpointer_value(arg_base);
  uint32_t offset = sexp_unbox_fixnum(arg_offset);
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_LEA;
  expr->data.lea.base = base;
  expr->data.lea.idx = NULL;
  expr->data.lea.scale = 0;
  expr->data.lea.offset = offset;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_begin_function(sexp ctx, sexp self, sexp_sint_t n,
                                     sexp arg_name, sexp arg_param_types,
                                     sexp arg_ret_ty) {
  // arg_name: symbol (e.g. |main|)
  // arg_param_types: list of L1Type* cpointers
  // arg_ret_ty: L1Type* cpointer
  const char *name = sexp_to_c_string(ctx, arg_name);

  L1Type *ret_ty = (L1Type *)sexp_cpointer_value(arg_ret_ty);

  uint32_t param_count = 0;
  L1Type **param_tys = NULL;
  if (sexp_pairp(arg_param_types)) {
    // Count params
    sexp curr = arg_param_types;
    while (sexp_pairp(curr)) {
      param_count++;
      curr = sexp_cdr(curr);
    }
    param_tys = malloc(sizeof(L1Type *) * param_count);
    curr = arg_param_types;
    for (uint32_t i = 0; i < param_count; i++) {
      param_tys[i] = (L1Type *)sexp_cpointer_value(sexp_car(curr));
      curr = sexp_cdr(curr);
    }
  }

  L1Subroutine *sub = malloc(sizeof(L1Subroutine));
  sub->name = strdup(name);
  sub->link_name = NULL;
  sub->ret_ty = ret_ty;
  sub->param_count = param_count;
  sub->param_tys = param_tys;
  sub->blocks = NULL;
  sub->blocks_tail = NULL;
  sub->is_extern = 0;
  sub->next = g_subroutines_head;
  g_subroutines_head = sub;

  L1Block *block = lainir_new_block();
  block->parent = sub;
  sub->blocks = block;
  sub->blocks_tail = block;
  g_current_sub = sub;
  g_current_block = block;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, block, SEXP_FALSE, 0);
}

// Set the C-level link_name for a subroutine (used for pub fn name mangling)
static sexp sexp_core_set_function_link_name(sexp ctx, sexp self, sexp_sint_t n,
                                              sexp arg_name, sexp arg_link) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  const char *link = sexp_to_c_string(ctx, arg_link);
  L1Subroutine *s = g_subroutines_head;
  while (s) {
    if (strcmp(s->name, name) == 0) {
      if (s->link_name) free((void*)s->link_name);
      s->link_name = strdup(link);
      return SEXP_VOID;
    }
    s = s->next;
  }
  return SEXP_FALSE;
}

static sexp sexp_core_mark_export(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp arg_name) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  native_mark_export(name);
  return SEXP_VOID;
}

static sexp sexp_core_declare_module(sexp ctx, sexp self, sexp_sint_t n,
                                     sexp arg_name) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  native_declare_module(name);
  return SEXP_VOID;
}

static sexp sexp_core_declare_signature(sexp ctx, sexp self, sexp_sint_t n,
                                        sexp arg_name) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  native_declare_signature(name);
  return SEXP_VOID;
}

// Meta has already decided the public nominal type and its semantic layout.
// This bridge copies that record for .lci serialization; it does not infer
// visibility, identity, layout, or L1 representation.
static sexp sexp_core_declare_interface_type(
    sexp ctx, sexp self, sexp_sint_t n, sexp arg_name, sexp arg_identity,
    sexp arg_layout, sexp arg_fields) {
  uint32_t count = get_list_length(arg_fields);
  const char **names = (const char **)calloc(count ? count : 1,
                                              sizeof(const char *));
  const char **types = (const char **)calloc(count ? count : 1,
                                              sizeof(const char *));
  uint32_t *offsets =
      (uint32_t *)calloc(count ? count : 1, sizeof(uint32_t));
  sexp rest = arg_fields;
  uint32_t i = 0;
  (void)self;
  (void)n;
  if (!names || !types || !offsets) abort();
  while (sexp_pairp(rest) && i < count) {
    sexp field = sexp_car(rest);
    sexp tail = sexp_cdr(field);
    names[i] = sexp_to_c_string(ctx, sexp_car(field));
    types[i] = sexp_to_c_string(ctx, sexp_car(tail));
    tail = sexp_cdr(tail);
    offsets[i] = (uint32_t)sexp_unbox_fixnum(sexp_car(tail));
    ++i;
    rest = sexp_cdr(rest);
  }
  native_declare_interface_type(
      sexp_to_c_string(ctx, arg_name), sexp_to_c_string(ctx, arg_identity),
      (uint32_t)sexp_unbox_fixnum(sexp_car(arg_layout)),
      (uint32_t)sexp_unbox_fixnum(sexp_car(sexp_cdr(arg_layout))),
      i, names, types, offsets);
  free(names);
  free(types);
  free(offsets);
  return SEXP_VOID;
}

static sexp sexp_core_declare_interface_function(
    sexp ctx, sexp self, sexp_sint_t n, sexp arg_name, sexp arg_params,
    sexp arg_ret) {
  uint32_t count = get_list_length(arg_params);
  const char **params = (const char **)calloc(count ? count : 1,
                                               sizeof(const char *));
  sexp rest = arg_params;
  uint32_t i = 0;
  (void)self;
  (void)n;
  if (!params) abort();
  while (sexp_pairp(rest) && i < count) {
    params[i++] = sexp_to_c_string(ctx, sexp_car(rest));
    rest = sexp_cdr(rest);
  }
  native_declare_interface_function(
      sexp_to_c_string(ctx, arg_name), i, params,
      sexp_to_c_string(ctx, arg_ret));
  free(params);
  return SEXP_VOID;
}

static sexp sexp_core_declare_extern_function(sexp ctx, sexp self,
                                              sexp_sint_t n, sexp arg_name,
                                              sexp arg_link_name,
                                              sexp arg_param_types,
                                              sexp arg_ret_ty) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  const char *link_name = sexp_to_c_string(ctx, arg_link_name);
  L1Type *ret_ty = (L1Type *)sexp_cpointer_value(arg_ret_ty);

  uint32_t param_count = 0;
  L1Type **param_tys = NULL;
  if (sexp_pairp(arg_param_types)) {
    sexp curr = arg_param_types;
    while (sexp_pairp(curr)) {
      param_count++;
      curr = sexp_cdr(curr);
    }
    param_tys = malloc(sizeof(L1Type *) * param_count);
    curr = arg_param_types;
    for (uint32_t i = 0; i < param_count; i++) {
      param_tys[i] = (L1Type *)sexp_cpointer_value(sexp_car(curr));
      curr = sexp_cdr(curr);
    }
  }

  L1Subroutine *sub = g_subroutines_head;
  while (sub) {
    if (strcmp(sub->name, name) == 0)
      break;
    sub = sub->next;
  }

  if (sub) {
    if (sub->link_name) free(sub->link_name);
    if (sub->param_tys) free(sub->param_tys);
    sub->link_name = strdup(link_name);
    sub->ret_ty = ret_ty;
    sub->param_count = param_count;
    sub->param_tys = param_tys;
    sub->is_extern = 1;
  } else {
    sub = malloc(sizeof(L1Subroutine));
    sub->name = strdup(name);
    sub->link_name = strdup(link_name);
    sub->ret_ty = ret_ty;
    sub->param_count = param_count;
    sub->param_tys = param_tys;
    sub->blocks = NULL;
    sub->blocks_tail = NULL;
    sub->is_extern = 1;
    sub->next = g_subroutines_head;
    g_subroutines_head = sub;
  }

  return sexp_make_cpointer(ctx, SEXP_CPOINTER, sub, SEXP_FALSE, 0);
}

static sexp sexp_core_function_by_name(sexp ctx, sexp self, sexp_sint_t n,
                                       sexp arg_name) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  L1Subroutine *s = g_subroutines_head;
  while (s) {
    if (strcmp(s->name, name) == 0)
      return sexp_make_cpointer(ctx, SEXP_CPOINTER, s, SEXP_FALSE, 0);
    s = s->next;
  }
  // Auto-create stub for external runtime functions
  // Default signature: fn(addr, i32) -> void (L1 physical ABI knowledge)
  L1Subroutine *ext = malloc(sizeof(L1Subroutine));
  ext->name = strdup(name);
  ext->link_name = NULL;
  ext->ret_ty = malloc(sizeof(L1Type));
  ext->ret_ty->kind = TY_UNIT;
  ext->param_count = 2;
  ext->param_tys = malloc(sizeof(L1Type *) * 2);
  ext->param_tys[0] = malloc(sizeof(L1Type));
  ext->param_tys[0]->kind = TY_ADDR;
  ext->param_tys[1] = malloc(sizeof(L1Type));
  ext->param_tys[1]->kind = TY_BITS;
  ext->param_tys[1]->width = 32;
  ext->blocks = NULL;
  ext->blocks_tail = NULL;
  ext->is_extern = 1;
  ext->next = g_subroutines_head;
  g_subroutines_head = ext;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, ext, SEXP_FALSE, 0);
}

static sexp sexp_core_append_block(sexp ctx, sexp self, sexp_sint_t n,
                                   sexp arg_sub) {
  L1Subroutine *sub = (L1Subroutine *)sexp_cpointer_value(arg_sub);
  L1Block *block = lainir_new_block();
  block->parent = sub;
  if (sub->blocks_tail) {
    sub->blocks_tail->next = block;
    sub->blocks_tail = block;
  } else {
    sub->blocks = block;
    sub->blocks_tail = block;
  }
  g_current_block = block;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, block, SEXP_FALSE, 0);
}

static sexp sexp_core_return_value(sexp ctx, sexp self, sexp_sint_t n,
                                   sexp arg_block, sexp arg_val) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Expr *val = (L1Expr *)sexp_cpointer_value(arg_val);
  L1Instruction *inst = lainir_new_instruction(INST_RETURN);
  inst->data.ret.val = val;
  L1Block *saved = g_current_block;
  g_current_block = block;
  append_instruction(NULL, inst);
  g_current_block = saved;
  return SEXP_VOID;
}

static sexp sexp_core_return_none(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp arg_block) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Instruction *inst = lainir_new_instruction(INST_RETURN);
  inst->data.ret.val = NULL;
  L1Block *saved = g_current_block;
  g_current_block = block;
  append_instruction(NULL, inst);
  g_current_block = saved;
  return SEXP_VOID;
}

static sexp sexp_core_function_return_type(sexp ctx, sexp self, sexp_sint_t n,
                                           sexp arg_sub) {
  L1Subroutine *sub = (L1Subroutine *)sexp_cpointer_value(arg_sub);
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, sub->ret_ty, SEXP_FALSE, 0);
}

static sexp sexp_core_function_param_types(sexp ctx, sexp self, sexp_sint_t n,
                                           sexp arg_sub) {
  L1Subroutine *sub = (L1Subroutine *)sexp_cpointer_value(arg_sub);
  sexp result = SEXP_NULL;
  for (int i = (int)sub->param_count - 1; i >= 0; i--) {
    result = sexp_cons(ctx,
                       sexp_make_cpointer(ctx, SEXP_CPOINTER, sub->param_tys[i],
                                          SEXP_FALSE, 0),
                       result);
  }
  return result;
}

static sexp sexp_core_function_link_name(sexp ctx, sexp self, sexp_sint_t n,
                                         sexp arg_sub) {
  L1Subroutine *sub = (L1Subroutine *)sexp_cpointer_value(arg_sub);
  const char *name = sub->link_name ? sub->link_name : sub->name;
  return sexp_c_string(ctx, name, -1);
}

static sexp sexp_core_call(sexp ctx, sexp self, sexp_sint_t n, sexp arg_block,
                           sexp arg_fn, sexp arg_args) {
  L1Subroutine *sub = (L1Subroutine *)sexp_cpointer_value(arg_fn);
  uint32_t arg_count = get_list_length(arg_args);
  L1Expr **args = malloc(sizeof(L1Expr *) * arg_count);
  sexp curr = arg_args;
  for (uint32_t i = 0; i < arg_count; i++) {
    args[i] = (L1Expr *)sexp_cpointer_value(sexp_car(curr));
    curr = sexp_cdr(curr);
  }
  L1Expr *expr = lainir_new_expr(EXPR_CALL);
  expr->data.call.fn_name = strdup(sub->link_name ? sub->link_name : sub->name);
  expr->data.call.args = args;
  expr->data.call.arg_count = arg_count;
  expr->data.call.ret_ty = sub->ret_ty;

  // Also append as instruction
  L1Instruction *inst = malloc(sizeof(L1Instruction));
  inst->kind = INST_CALL;
  inst->data.call_inst.expr = expr;
  inst->next = NULL;
  append_instruction(g_current_sub, inst);

  // Also mark as EXPR_EVAL for compile-time interpretation
  L1Instruction *eval_inst = malloc(sizeof(L1Instruction));
  eval_inst->kind = INST_CALL;
  eval_inst->data.call_inst.expr = expr;
  eval_inst->next = NULL;
  append_instruction(g_current_sub, eval_inst);

  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_eval(sexp ctx, sexp self, sexp_sint_t n, sexp arg_block,
                           sexp arg_fn, sexp arg_args) {
  L1Subroutine *sub = (L1Subroutine *)sexp_cpointer_value(arg_fn);
  uint32_t arg_count = get_list_length(arg_args);
  L1Expr **args = malloc(sizeof(L1Expr *) * arg_count);
  sexp curr = arg_args;
  for (uint32_t i = 0; i < arg_count; i++) {
    args[i] = (L1Expr *)sexp_cpointer_value(sexp_car(curr));
    curr = sexp_cdr(curr);
  }
  L1Expr *expr = lainir_new_expr(EXPR_EVAL);
  expr->data.eval.fn_name = strdup(sub->link_name ? sub->link_name : sub->name);
  expr->data.eval.args = args;
  expr->data.eval.arg_count = arg_count;
  expr->data.eval.ret_ty = sub->ret_ty;

  L1Instruction *inst = malloc(sizeof(L1Instruction));
  inst->kind = INST_CALL;
  inst->data.call_inst.expr = expr;
  inst->next = NULL;
  append_instruction(g_current_sub, inst);

  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_eval_value(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp arg_val) {
  L1Expr *expr = (L1Expr *)sexp_cpointer_value(arg_val);
  (void)self;
  (void)n;
  
  if (!expr) return SEXP_FALSE;
  
  switch (expr->kind) {
  case EXPR_CONST:
    return sexp_make_fixnum(expr->data.const_val);
  case EXPR_STRING:
    return sexp_c_string(ctx, expr->data.str_val.content, -1);
  default:
    return SEXP_FALSE;
  }
}

static sexp sexp_core_call_expr(sexp ctx, sexp self, sexp_sint_t n,
                                sexp block_val, sexp fn_val, sexp args_val) {
  L1Subroutine *sub = (L1Subroutine *)sexp_cpointer_value(fn_val);
  uint32_t arg_count = get_list_length(args_val);

  L1Expr **args = malloc(sizeof(L1Expr *) * arg_count);
  sexp curr = args_val;
  for (uint32_t i = 0; i < arg_count; i++) {
    args[i] = (L1Expr *)sexp_cpointer_value(sexp_car(curr));
    curr = sexp_cdr(curr);
  }

  L1Expr *expr = lainir_new_expr(EXPR_CALL);
  expr->data.call.fn_name = strdup(sub->link_name ? sub->link_name : sub->name);
  expr->data.call.args = args;
  expr->data.call.arg_count = arg_count;
  expr->data.call.ret_ty = sub->ret_ty;
  // Does NOT append instruction — caller decides how to emit
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_assign_temp(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp block_val, sexp expr_val) {
  L1Expr *expr = (L1Expr *)sexp_cpointer_value(expr_val);
  char name[32];
  snprintf(name, sizeof(name), "_t%d", g_temp_counter++);

  // Append INST_SET: auto _tN = <expr>;
  L1Instruction *inst = malloc(sizeof(L1Instruction));
  inst->kind = INST_SET;
  inst->data.set.name = strdup(name);
  inst->data.set.ty = NULL;
  inst->data.set.val = expr;
  inst->next = NULL;
  append_instruction(g_current_sub, inst);

  // Return EXPR_VAR referencing the temp variable
  L1Expr *var = lainir_new_expr(EXPR_VAR);
  var->data.var.name = strdup(name);
  var->data.var.ty = infer_expr_type(expr);
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, var, SEXP_FALSE, 0);
}

static sexp sexp_core_primitive(sexp ctx, sexp self, sexp_sint_t n,
                                sexp arg_block, sexp arg_op, sexp arg_operands,
                                sexp arg_result_ty) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  const char *op = sexp_to_c_string(ctx, arg_op);
  uint32_t operand_count = get_list_length(arg_operands);
  L1Expr **operands = malloc(sizeof(L1Expr *) * operand_count);
  sexp curr = arg_operands;
  for (uint32_t i = 0; i < operand_count; i++) {
    operands[i] = (L1Expr *)sexp_cpointer_value(sexp_car(curr));
    curr = sexp_cdr(curr);
  }
  L1Type *result_ty = (L1Type *)sexp_cpointer_value(arg_result_ty);
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_PRIMITIVE;
  expr->data.primitive.opcode = strdup(op);
  expr->data.primitive.operands = operands;
  expr->data.primitive.operand_count = operand_count;
  expr->data.primitive.result_ty = result_ty;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

// Helper function to create binary expressions
static sexp sexp_core_bin_expr(sexp ctx, sexp self, sexp_sint_t n,
                               sexp arg_left, sexp arg_right, L1ExprKind kind) {
  L1Expr *left = (L1Expr *)sexp_cpointer_value(arg_left);
  L1Expr *right = (L1Expr *)sexp_cpointer_value(arg_right);
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = kind;
  expr->data.bin.left = left;
  expr->data.bin.right = right;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

// Helper function to create unary expressions
static sexp sexp_core_unary_expr(sexp ctx, sexp self, sexp_sint_t n,
                                 sexp arg_operand, L1ExprKind kind) {
  L1Expr *operand = (L1Expr *)sexp_cpointer_value(arg_operand);
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = kind;
  expr->data.unary.operand = operand;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_mul(sexp ctx, sexp self, sexp_sint_t n,
                          sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_MUL);
}

static sexp sexp_core_div(sexp ctx, sexp self, sexp_sint_t n,
                          sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_DIV);
}

static sexp sexp_core_eq(sexp ctx, sexp self, sexp_sint_t n,
                         sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_EQ);
}

static sexp sexp_core_ne(sexp ctx, sexp self, sexp_sint_t n,
                         sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_NE);
}

static sexp sexp_core_lt(sexp ctx, sexp self, sexp_sint_t n,
                         sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_LT);
}

static sexp sexp_core_le(sexp ctx, sexp self, sexp_sint_t n,
                         sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_LE);
}

static sexp sexp_core_gt(sexp ctx, sexp self, sexp_sint_t n,
                         sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_GT);
}

static sexp sexp_core_ge(sexp ctx, sexp self, sexp_sint_t n,
                         sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_GE);
}

static sexp sexp_core_add(sexp ctx, sexp self, sexp_sint_t n,
                          sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_ADD);
}

static sexp sexp_core_sub(sexp ctx, sexp self, sexp_sint_t n,
                          sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_SUB);
}

static sexp sexp_core_fadd(sexp ctx, sexp self, sexp_sint_t n,
                           sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_FADD);
}

static sexp sexp_core_fsub(sexp ctx, sexp self, sexp_sint_t n,
                           sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_FSUB);
}

static sexp sexp_core_fmul(sexp ctx, sexp self, sexp_sint_t n,
                           sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_FMUL);
}

static sexp sexp_core_fdiv(sexp ctx, sexp self, sexp_sint_t n,
                           sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_FDIV);
}

static sexp sexp_core_feq(sexp ctx, sexp self, sexp_sint_t n,
                          sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_FEQ);
}

static sexp sexp_core_flt(sexp ctx, sexp self, sexp_sint_t n,
                          sexp arg_left, sexp arg_right) {
  return sexp_core_bin_expr(ctx, self, n, arg_left, arg_right, EXPR_FLT);
}

static sexp sexp_core_popcount(sexp ctx, sexp self, sexp_sint_t n,
                               sexp arg_operand) {
  return sexp_core_unary_expr(ctx, self, n, arg_operand, EXPR_POPCOUNT);
}

static sexp sexp_core_clz(sexp ctx, sexp self, sexp_sint_t n,
                          sexp arg_operand) {
  return sexp_core_unary_expr(ctx, self, n, arg_operand, EXPR_CLZ);
}

static sexp sexp_core_rotl(sexp ctx, sexp self, sexp_sint_t n,
                           sexp arg_operand) {
  return sexp_core_unary_expr(ctx, self, n, arg_operand, EXPR_ROTL);
}

static sexp sexp_core_int2ptr(sexp ctx, sexp self, sexp_sint_t n,
                              sexp arg_operand) {
  return sexp_core_unary_expr(ctx, self, n, arg_operand, EXPR_INT2PTR);
}

static sexp sexp_core_ptr2int(sexp ctx, sexp self, sexp_sint_t n,
                              sexp arg_operand) {
  return sexp_core_unary_expr(ctx, self, n, arg_operand, EXPR_PTR2INT);
}

static sexp sexp_core_local_alloc(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp arg_block, sexp arg_element_ty,
                                  sexp arg_byte_size) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Type *element_ty = (L1Type *)sexp_cpointer_value(arg_element_ty);
  uint32_t byte_size = sexp_unbox_fixnum(arg_byte_size);
  L1Type *result_ty = malloc(sizeof(L1Type));
  result_ty->kind = TY_ADDR;
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_ALLOCA;
  expr->data.alloca.element_ty = element_ty;
  expr->data.alloca.byte_size = byte_size;
  expr->data.alloca.result_ty = result_ty;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_param(sexp ctx, sexp self, sexp_sint_t n,
                            sexp arg_function, sexp arg_index) {
  // arg_function is L1Block* cpointer (we only need the index for EXPR_ARG)
  uint32_t idx = sexp_unbox_fixnum(arg_index);
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_ARG;
  expr->data.arg.index = idx;
  expr->data.arg.ty = NULL;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_block_function(sexp ctx, sexp self, sexp_sint_t n,
                                     sexp arg_block) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  if (block->parent)
    return sexp_make_cpointer(ctx, SEXP_CPOINTER, block->parent, SEXP_FALSE, 0);
  return SEXP_FALSE;
}

static sexp sexp_core_branch(sexp ctx, sexp self, sexp_sint_t n, sexp arg_block,
                             sexp arg_target) {
  /* Unstructured branch — no-op in structured IR.
   * Meta passes should use begin-if!/end-if! instead. */
  (void)ctx; (void)self; (void)n; (void)arg_block; (void)arg_target;
  return SEXP_VOID;
}

static sexp sexp_core_cond_branch(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp arg_block, sexp arg_cond, sexp arg_true,
                                  sexp arg_false) {
  /* Unstructured cond-branch — no-op in structured IR.
   * Meta passes should use begin-if!/end-if! instead. */
  (void)ctx; (void)self; (void)n; (void)arg_block; (void)arg_cond;
  (void)arg_true; (void)arg_false;
  return SEXP_VOID;
}

static sexp sexp_core_type_is_void(sexp ctx, sexp self, sexp_sint_t n,
                                   sexp arg_ty) {
  L1Type *ty = (L1Type *)sexp_cpointer_value(arg_ty);
  return (ty && ty->kind == TY_UNIT) ? SEXP_TRUE : SEXP_FALSE;
}

static sexp sexp_core_type_is_addr(sexp ctx, sexp self, sexp_sint_t n,
                                   sexp arg_ty) {
  L1Type *ty = (L1Type *)sexp_cpointer_value(arg_ty);
  return (ty && ty->kind == TY_ADDR) ? SEXP_TRUE : SEXP_FALSE;
}


// ── Scratch blocks + structured if FFI ────────────────────────────────────

sexp sexp_core_set_current_block(sexp ctx, sexp self, sexp_sint_t n, sexp bv) {
  g_current_block = (L1Block *)sexp_cpointer_value(bv);
  return SEXP_VOID;
}

sexp sexp_core_get_current_block(sexp ctx, sexp self, sexp_sint_t n) {
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, g_current_block, SEXP_FALSE, 0);
}

sexp sexp_core_begin_if(sexp ctx, sexp self, sexp_sint_t n, sexp bv, sexp cv) {
  L1Block *tb = lainir_new_block();
  L1Block *eb = lainir_new_block();
  tb->parent = NULL;
  eb->parent = NULL;
  sexp ts = sexp_make_cpointer(ctx, SEXP_CPOINTER, tb, SEXP_FALSE, 0);
  sexp es = sexp_make_cpointer(ctx, SEXP_CPOINTER, eb, SEXP_FALSE, 0);
  return sexp_cons(ctx, ts, sexp_cons(ctx, es, SEXP_NULL));
}

static sexp sexp_core_end_if(sexp ctx, sexp self, sexp_sint_t n, sexp bv, sexp cv, sexp tv, sexp ev) {
  L1Block *parent = (L1Block *)sexp_cpointer_value(bv);
  L1Expr *cond = (L1Expr *)sexp_cpointer_value(cv);
  L1Block *tb = (L1Block *)sexp_cpointer_value(tv);
  L1Block *eb = (L1Block *)sexp_cpointer_value(ev);
  L1Instruction *inst = lainir_new_instruction(INST_IF);
  inst->data.if_stmt.condition = cond;
  inst->data.if_stmt.then_body = tb;
  inst->data.if_stmt.else_body = eb;
  L1Block *saved = g_current_block;
  g_current_block = parent;
  append_instruction(NULL, inst);
  g_current_block = saved;
  return SEXP_VOID;
}


// ============================================================================

// ── Thin type-size query (reads width from L1Type cpointer, no computation) ──

static sexp sexp_core_type_size(sexp ctx, sexp self, sexp_sint_t n,
                                 sexp arg_ty) {
  L1Type *ty = (L1Type *)sexp_cpointer_value(arg_ty);
  if (!ty) return sexp_make_fixnum(0);
  uint32_t size = (ty->kind == TY_BITS) ? (ty->width / 8) : 8;
  return sexp_make_fixnum((sexp_sint_t)size);
}

// ── New: offset-based field access (no struct type needed) ──

static sexp sexp_core_field_offset(sexp ctx, sexp self, sexp_sint_t n,
                                    sexp arg_block, sexp arg_base,
                                    sexp arg_offset, sexp arg_field_ty) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Expr *base = (L1Expr *)sexp_cpointer_value(arg_base);
  uint32_t offset = sexp_unbox_fixnum(arg_offset);
  L1Type *field_ty = (L1Type *)sexp_cpointer_value(arg_field_ty);
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_FIELD;
  expr->data.field.base = base;
  expr->data.field.struct_ty = NULL;   // use offset directly
  expr->data.field.field_index = offset;
  expr->data.field.field_ty = field_ty;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

// ── New: integer-based aggregate (no struct type needed) ──

static sexp sexp_core_aggregate_layout(sexp ctx, sexp self, sexp_sint_t n,
                                        sexp arg_block, sexp arg_total_size,
                                        sexp arg_layout) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  uint32_t total_size = sexp_unbox_fixnum(arg_total_size);
  uint32_t field_count = get_list_length(arg_layout);

  L1Type *result_ty = malloc(sizeof(L1Type));
  result_ty->kind = TY_ADDR;
  L1Expr *alloca_expr = malloc(sizeof(L1Expr));
  alloca_expr->kind = EXPR_ALLOCA;
  alloca_expr->data.alloca.element_ty = NULL;
  alloca_expr->data.alloca.byte_size = total_size;
  alloca_expr->data.alloca.result_ty = result_ty;

  static int agg_counter = 0;
  char agg_name[64];
  snprintf(agg_name, sizeof(agg_name), "__agg_%d", agg_counter++);
  L1Instruction *set_inst = malloc(sizeof(L1Instruction));
  set_inst->kind = INST_SET;
  set_inst->data.set.name = strdup(agg_name);
  set_inst->data.set.ty = NULL;
  set_inst->data.set.val = alloca_expr;
  set_inst->next = NULL;
  append_inst_to_block(block, set_inst);

  L1Expr *agg_var = malloc(sizeof(L1Expr));
  agg_var->kind = EXPR_VAR;
  agg_var->data.var.name = strdup(agg_name);
  agg_var->data.var.ty = result_ty;

  sexp curr = arg_layout;
  for (uint32_t i = 0; i < field_count; i++) {
    sexp pair = sexp_car(curr);
    uint32_t off = (uint32_t)sexp_unbox_fixnum(sexp_car(pair));
    L1Expr *val = (L1Expr *)sexp_cpointer_value(sexp_cdr(pair));
    L1Type *field_ty = infer_expr_type(val);

    L1Expr *offset_expr = malloc(sizeof(L1Expr));
    offset_expr->kind = EXPR_CONST;
    offset_expr->data.const_val = off;
    L1Expr *dest_expr = malloc(sizeof(L1Expr));
    dest_expr->kind = EXPR_LEA;
    dest_expr->data.lea.base = agg_var;
    dest_expr->data.lea.idx = offset_expr;
    dest_expr->data.lea.scale = 1;
    dest_expr->data.lea.offset = 0;

    L1Instruction *store_inst = malloc(sizeof(L1Instruction));
    store_inst->kind = INST_STORE;
    store_inst->data.store.dest = dest_expr;
    store_inst->data.store.val = val;
    store_inst->data.store.store_ty = field_ty;
    store_inst->next = NULL;
    append_inst_to_block(block, store_inst);
    curr = sexp_cdr(curr);
  }

  return sexp_make_cpointer(ctx, SEXP_CPOINTER, agg_var, SEXP_FALSE, 0);
}

static sexp sexp_core_call_indirect(sexp ctx, sexp self, sexp_sint_t n,
                                    sexp arg_block, sexp arg_fn_ptr,
                                    sexp arg_ret_ty, sexp arg_args) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Expr *fn_ptr = (L1Expr *)sexp_cpointer_value(arg_fn_ptr);
  L1Type *ret_ty = (L1Type *)sexp_cpointer_value(arg_ret_ty);
  uint32_t arg_count = get_list_length(arg_args);
  L1Expr **args = malloc(sizeof(L1Expr *) * arg_count);
  sexp curr = arg_args;
  for (uint32_t i = 0; i < arg_count; i++) {
    args[i] = (L1Expr *)sexp_cpointer_value(sexp_car(curr));
    curr = sexp_cdr(curr);
  }
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_CALL_INDIRECT;
  expr->data.call_indirect.fn_ptr = fn_ptr;
  expr->data.call_indirect.ret_ty = ret_ty;
  expr->data.call_indirect.param_tys = NULL;
  expr->data.call_indirect.param_count = 0;
  expr->data.call_indirect.args = args;
  expr->data.call_indirect.arg_count = arg_count;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_function_ref(sexp ctx, sexp self, sexp_sint_t n,
                                    sexp arg_name) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  L1Expr *var = malloc(sizeof(L1Expr));
  var->kind = EXPR_VAR;
  var->data.var.name = strdup(name);
  var->data.var.ty = malloc(sizeof(L1Type));
  var->data.var.ty->kind = TY_ADDR;
  var->data.var.ty->width = 64;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, var, SEXP_FALSE, 0);
}

// ============================================================================
// LAIN-AST FFI — 让 Scheme 操作 C 侧的拓扑树
// ============================================================================

#include "lainast/lain_ast.h"

// 全局单 arena (简化: 当前只支持一次解析一个文件)
static AstArena g_ast_arena;
static int g_ast_arena_inited = 0;

// Stable multi-source syntax storage.  AstNodeId remains local to AstArena;
// callers see an opaque tagged i32 whose registry entry identifies the
// owning store, unit, and local node.  This lets the existing ast.node-*
// topology API traverse multiple live arenas without widening every Lain
// frontend function signature.
typedef struct {
    uint32_t id;
    int alive;
    AstArena arena;
    AstNodeId root;
    uint32_t stable_base;
} AstSyntaxUnit;

typedef struct {
    uint32_t id;
    int alive;
    AstSyntaxUnit *units;
    uint32_t unit_count;
    uint32_t unit_capacity;
} AstSyntaxStore;

static AstSyntaxStore *g_ast_stores = NULL;
static uint32_t g_ast_store_count = 0;
static uint32_t g_ast_store_capacity = 0;
static uint32_t g_ast_next_store_id = 1;
static uint32_t g_ast_next_unit_id = 1;

#define AST_STABLE_HANDLE_TAG UINT32_C(0x40000000)
#define AST_STABLE_HANDLE_MASK UINT32_C(0x3fffffff)

typedef struct {
    uint32_t store_id;
    uint32_t unit_id;
    AstNodeId local_id;
} AstStableNodeRef;

static AstStableNodeRef *g_ast_stable_nodes = NULL;
static uint32_t g_ast_stable_node_count = 0;
static uint32_t g_ast_stable_node_capacity = 0;

static AstSyntaxStore *ast_syntax_store_get(uint32_t store_id) {
    uint32_t i;
    for (i = 0; i < g_ast_store_count; i++) {
        AstSyntaxStore *store = &g_ast_stores[i];
        if (store->alive && store->id == store_id) return store;
    }
    return NULL;
}

static AstSyntaxUnit *ast_syntax_unit_get(AstSyntaxStore *store,
                                           uint32_t unit_id) {
    uint32_t i;
    if (!store) return NULL;
    for (i = 0; i < store->unit_count; i++) {
        AstSyntaxUnit *unit = &store->units[i];
        if (unit->alive && unit->id == unit_id) return unit;
    }
    return NULL;
}

static uint32_t ast_syntax_register_nodes(AstSyntaxStore *store,
                                          AstSyntaxUnit *unit) {
    uint32_t count = ast_count(&unit->arena);
    uint32_t needed = g_ast_stable_node_count + count;
    uint32_t i;
    if (needed >= AST_STABLE_HANDLE_TAG) return 0;
    if (needed > g_ast_stable_node_capacity) {
        uint32_t next = g_ast_stable_node_capacity ?
            g_ast_stable_node_capacity * 2 : 1024;
        while (next < needed) next *= 2;
        AstStableNodeRef *grown = realloc(
            g_ast_stable_nodes, sizeof(*grown) * next);
        if (!grown) return 0;
        g_ast_stable_nodes = grown;
        g_ast_stable_node_capacity = next;
    }
    unit->stable_base = g_ast_stable_node_count + 1;
    for (i = 1; i <= count; i++) {
        AstStableNodeRef *ref = &g_ast_stable_nodes[g_ast_stable_node_count++];
        ref->store_id = store->id;
        ref->unit_id = unit->id;
        ref->local_id = (AstNodeId)i;
    }
    return 1;
}

static uint32_t ast_syntax_stable_handle(const AstSyntaxUnit *unit,
                                         AstNodeId local_id) {
    if (!unit || local_id == AST_NULL) return 0;
    return AST_STABLE_HANDLE_TAG |
           (unit->stable_base + (uint32_t)local_id - 1);
}

static const AstNode *ast_ffi_node_get(uint32_t handle,
                                       AstSyntaxUnit **out_unit,
                                       AstNodeId *out_local) {
    AstSyntaxUnit *unit = NULL;
    AstNodeId local = (AstNodeId)handle;
    const AstNode *node = NULL;
    if ((handle & AST_STABLE_HANDLE_TAG) != 0) {
        uint32_t index = handle & AST_STABLE_HANDLE_MASK;
        AstStableNodeRef *ref;
        AstSyntaxStore *store;
        if (index == 0 || index > g_ast_stable_node_count) goto done;
        ref = &g_ast_stable_nodes[index - 1];
        store = ast_syntax_store_get(ref->store_id);
        unit = ast_syntax_unit_get(store, ref->unit_id);
        local = ref->local_id;
        if (unit) node = ast_get(&unit->arena, local);
    } else if (g_ast_arena_inited) {
        node = ast_get(&g_ast_arena, local);
    }
done:
    if (out_unit) *out_unit = unit;
    if (out_local) *out_local = local;
    return node;
}

static uint32_t ast_ffi_child_handle(AstSyntaxUnit *unit,
                                     AstNodeId local_child) {
    return unit ? ast_syntax_stable_handle(unit, local_child)
                : (uint32_t)local_child;
}

static const AstNode *ast_syntax_node_get(uint32_t store_id,
                                           uint32_t unit_id,
                                           uint32_t node_handle) {
    AstSyntaxStore *store = ast_syntax_store_get(store_id);
    AstSyntaxUnit *unit = ast_syntax_unit_get(store, unit_id);
    AstSyntaxUnit *owner = NULL;
    const AstNode *node;
    if (!unit) return NULL;
    node = ast_ffi_node_get(node_handle, &owner, NULL);
    if (owner) return owner == unit ? node : NULL;
    // Explicit unit APIs retain local-node compatibility during migration.
    return ast_get(&unit->arena, (AstNodeId)node_handle);
}

static sexp sexp_ast_store_new(sexp ctx, sexp self, sexp_sint_t n) {
    AstSyntaxStore *store;
    if (g_ast_store_count == g_ast_store_capacity) {
        uint32_t next = g_ast_store_capacity ? g_ast_store_capacity * 2 : 4;
        AstSyntaxStore *grown = realloc(g_ast_stores, sizeof(*grown) * next);
        if (!grown) return sexp_make_fixnum(0);
        g_ast_stores = grown;
        g_ast_store_capacity = next;
    }
    store = &g_ast_stores[g_ast_store_count++];
    memset(store, 0, sizeof(*store));
    store->id = g_ast_next_store_id++;
    store->alive = 1;
    return sexp_make_fixnum((sexp_sint_t)store->id);
}

static sexp sexp_ast_store_destroy(sexp ctx, sexp self, sexp_sint_t n,
                                   sexp arg_store) {
    uint32_t i;
    AstSyntaxStore *store = ast_syntax_store_get(
        (uint32_t)sexp_unbox_fixnum(arg_store));
    if (!store) return sexp_make_fixnum(0);
    for (i = 0; i < store->unit_count; i++) {
        if (store->units[i].alive) {
            ast_arena_destroy(&store->units[i].arena);
            store->units[i].alive = 0;
        }
    }
    free(store->units);
    store->units = NULL;
    store->unit_count = 0;
    store->unit_capacity = 0;
    store->alive = 0;
    return sexp_make_fixnum(1);
}

static sexp sexp_ast_unit_parse(sexp ctx, sexp self, sexp_sint_t n,
                                sexp arg_store, sexp arg_src,
                                sexp arg_len) {
    AstSyntaxUnit *unit;
    AstSyntaxStore *store = ast_syntax_store_get(
        (uint32_t)sexp_unbox_fixnum(arg_store));
    const char *src = sexp_to_c_string(ctx, arg_src);
    uint32_t len = (uint32_t)sexp_unbox_fixnum(arg_len);
    if (!store || !src) return sexp_make_fixnum(0);
    if (store->unit_count == store->unit_capacity) {
        uint32_t next = store->unit_capacity ? store->unit_capacity * 2 : 4;
        AstSyntaxUnit *grown = realloc(store->units, sizeof(*grown) * next);
        if (!grown) return sexp_make_fixnum(0);
        store->units = grown;
        store->unit_capacity = next;
    }
    unit = &store->units[store->unit_count++];
    memset(unit, 0, sizeof(*unit));
    unit->id = g_ast_next_unit_id++;
    unit->alive = 1;
    ast_arena_init(&unit->arena);
    unit->root = ast_parse(&unit->arena, src, len);
    if (!ast_syntax_register_nodes(store, unit)) {
        ast_arena_destroy(&unit->arena);
        unit->alive = 0;
        unit->root = AST_NULL;
        return sexp_make_fixnum(0);
    }
    return sexp_make_fixnum((sexp_sint_t)unit->id);
}

static sexp sexp_ast_unit_destroy(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp arg_store, sexp arg_unit) {
    AstSyntaxStore *store = ast_syntax_store_get(
        (uint32_t)sexp_unbox_fixnum(arg_store));
    AstSyntaxUnit *unit = ast_syntax_unit_get(
        store, (uint32_t)sexp_unbox_fixnum(arg_unit));
    if (!unit) return sexp_make_fixnum(0);
    ast_arena_destroy(&unit->arena);
    unit->alive = 0;
    unit->root = AST_NULL;
    return sexp_make_fixnum(1);
}

static AstSyntaxUnit *ast_syntax_unit_from_args(sexp arg_store,
                                                sexp arg_unit) {
    AstSyntaxStore *store = ast_syntax_store_get(
        (uint32_t)sexp_unbox_fixnum(arg_store));
    return ast_syntax_unit_get(store, (uint32_t)sexp_unbox_fixnum(arg_unit));
}

static sexp sexp_ast_unit_root(sexp ctx, sexp self, sexp_sint_t n,
                               sexp arg_store, sexp arg_unit) {
    AstSyntaxUnit *unit = ast_syntax_unit_from_args(arg_store, arg_unit);
    return sexp_make_fixnum(unit ?
        (sexp_sint_t)ast_syntax_stable_handle(unit, unit->root) : 0);
}

static sexp sexp_ast_unit_node_count(sexp ctx, sexp self, sexp_sint_t n,
                                     sexp arg_store, sexp arg_unit) {
    AstSyntaxUnit *unit = ast_syntax_unit_from_args(arg_store, arg_unit);
    return sexp_make_fixnum(unit ? (sexp_sint_t)ast_count(&unit->arena) : 0);
}

static sexp sexp_ast_unit_node_kind(sexp ctx, sexp self, sexp_sint_t n,
                                    sexp s, sexp u, sexp id) {
    const AstNode *node = ast_syntax_node_get(
        (uint32_t)sexp_unbox_fixnum(s), (uint32_t)sexp_unbox_fixnum(u),
        (uint32_t)sexp_unbox_fixnum(id));
    return sexp_make_fixnum(node ? (sexp_sint_t)node->kind : -1);
}

static sexp sexp_ast_unit_node_text(sexp ctx, sexp self, sexp_sint_t n,
                                    sexp s, sexp u, sexp id) {
    const AstNode *node = ast_syntax_node_get(
        (uint32_t)sexp_unbox_fixnum(s), (uint32_t)sexp_unbox_fixnum(u),
        (uint32_t)sexp_unbox_fixnum(id));
    return node && node->text ? sexp_c_string(ctx, node->text, -1) : SEXP_FALSE;
}

static sexp sexp_ast_unit_node_string_value(sexp ctx, sexp self,
                                            sexp_sint_t n, sexp s, sexp u,
                                            sexp id) {
    const AstNode *node = ast_syntax_node_get(
        (uint32_t)sexp_unbox_fixnum(s), (uint32_t)sexp_unbox_fixnum(u),
        (uint32_t)sexp_unbox_fixnum(id));
    size_t len;
    if (!node || !node->text) return SEXP_FALSE;
    len = strlen(node->text);
    if (len < 2 || node->text[0] != '"' || node->text[len - 1] != '"')
        return SEXP_FALSE;
    return sexp_c_string(ctx, node->text + 1, (sexp_sint_t)(len - 2));
}

static sexp sexp_ast_unit_node_is_atom_text(sexp ctx, sexp self,
                                            sexp_sint_t n, sexp s, sexp u,
                                            sexp id, sexp arg_text) {
    const AstNode *node = ast_syntax_node_get(
        (uint32_t)sexp_unbox_fixnum(s), (uint32_t)sexp_unbox_fixnum(u),
        (uint32_t)sexp_unbox_fixnum(id));
    const char *value = sexp_to_c_string(ctx, arg_text);
    return sexp_make_fixnum(node && node->kind == AST_ATOM && node->text &&
                            value && strcmp(node->text, value) == 0 ? 1 : 0);
}

static sexp sexp_ast_unit_node_is_infix_text(sexp ctx, sexp self,
                                             sexp_sint_t n, sexp s, sexp u,
                                             sexp id, sexp arg_text) {
    uint32_t store_id = (uint32_t)sexp_unbox_fixnum(s);
    uint32_t unit_id = (uint32_t)sexp_unbox_fixnum(u);
    const AstNode *node = ast_syntax_node_get(
        store_id, unit_id, (uint32_t)sexp_unbox_fixnum(id));
    const AstNode *op = node ? ast_syntax_node_get(store_id, unit_id, node->op)
                             : NULL;
    const char *value = sexp_to_c_string(ctx, arg_text);
    return sexp_make_fixnum(node && node->kind == AST_INFIX && op && op->text &&
                            value && strcmp(op->text, value) == 0 ? 1 : 0);
}

static sexp sexp_ast_unit_node_atom_class(sexp ctx, sexp self, sexp_sint_t n,
                                          sexp s, sexp u, sexp id) {
    const AstNode *node = ast_syntax_node_get(
        (uint32_t)sexp_unbox_fixnum(s), (uint32_t)sexp_unbox_fixnum(u),
        (uint32_t)sexp_unbox_fixnum(id));
    const char *value;
    size_t len;
    size_t i;
    if (!node || node->kind != AST_ATOM || !node->text)
        return sexp_make_fixnum(0);
    value = node->text;
    len = strlen(value);
    if (len >= 2 && value[0] == '"' && value[len - 1] == '"')
        return sexp_make_fixnum(2);
    if (len > 0 && !(len > 1 && value[0] == '0')) {
        for (i = 0; i < len && value[i] >= '0' && value[i] <= '9'; i++) {}
        if (i == len) return sexp_make_fixnum(1);
    }
    return sexp_make_fixnum(3);
}

static sexp sexp_ast_unit_node_line(sexp ctx, sexp self, sexp_sint_t n,
                                    sexp s, sexp u, sexp id) {
    const AstNode *node = ast_syntax_node_get(
        (uint32_t)sexp_unbox_fixnum(s), (uint32_t)sexp_unbox_fixnum(u),
        (uint32_t)sexp_unbox_fixnum(id));
    return sexp_make_fixnum(node ? (sexp_sint_t)node->line : 0);
}

static sexp sexp_ast_unit_node_col(sexp ctx, sexp self, sexp_sint_t n,
                                   sexp s, sexp u, sexp id) {
    const AstNode *node = ast_syntax_node_get(
        (uint32_t)sexp_unbox_fixnum(s), (uint32_t)sexp_unbox_fixnum(u),
        (uint32_t)sexp_unbox_fixnum(id));
    return sexp_make_fixnum(node ? (sexp_sint_t)node->col : 0);
}

static sexp ast_unit_node_edge(sexp ctx, sexp s, sexp u, sexp id, int edge) {
    AstSyntaxUnit *unit = ast_syntax_unit_from_args(s, u);
    const AstNode *node = ast_syntax_node_get(
        (uint32_t)sexp_unbox_fixnum(s), (uint32_t)sexp_unbox_fixnum(u),
        (uint32_t)sexp_unbox_fixnum(id));
    AstNodeId value = AST_NULL;
    if (node) {
        if (edge == 0) value = node->left;
        else if (edge == 1) value = node->right;
        else if (edge == 2) value = node->op;
        else value = node->next;
    }
    return sexp_make_fixnum((sexp_sint_t)ast_ffi_child_handle(unit, value));
}

static sexp sexp_ast_unit_node_left(sexp ctx, sexp self, sexp_sint_t n,
                                    sexp s, sexp u, sexp id) {
    return ast_unit_node_edge(ctx, s, u, id, 0);
}
static sexp sexp_ast_unit_node_right(sexp ctx, sexp self, sexp_sint_t n,
                                     sexp s, sexp u, sexp id) {
    return ast_unit_node_edge(ctx, s, u, id, 1);
}
static sexp sexp_ast_unit_node_op(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp s, sexp u, sexp id) {
    return ast_unit_node_edge(ctx, s, u, id, 2);
}
static sexp sexp_ast_unit_node_next(sexp ctx, sexp self, sexp_sint_t n,
                                    sexp s, sexp u, sexp id) {
    return ast_unit_node_edge(ctx, s, u, id, 3);
}

// ast.parse!(src, len) → root node id
static sexp sexp_ast_parse(sexp ctx, sexp self, sexp_sint_t n,
                            sexp arg_src, sexp arg_len) {
    const char *src = sexp_to_c_string(ctx, arg_src);
    uint32_t len = sexp_unbox_fixnum(arg_len);

    if (g_ast_arena_inited) ast_arena_destroy(&g_ast_arena);
    ast_arena_init(&g_ast_arena);
    g_ast_arena_inited = 1;

    AstNodeId root = ast_parse(&g_ast_arena, src, len);
    return sexp_make_fixnum((sexp_sint_t)root);
}

// ast.node-count() → count
static sexp sexp_ast_node_count(sexp ctx, sexp self, sexp_sint_t n) {
    return sexp_make_fixnum((sexp_sint_t)ast_count(&g_ast_arena));
}

// ast.node-kind(id) → 0-4
static sexp sexp_ast_node_kind(sexp ctx, sexp self, sexp_sint_t n,
                                sexp arg_id) {
    uint32_t id = (uint32_t)sexp_unbox_fixnum(arg_id);
    const AstNode *node = ast_ffi_node_get(id, NULL, NULL);
    if (!node) return sexp_make_fixnum(-1);
    return sexp_make_fixnum((sexp_sint_t)node->kind);
}

// ast.node-text(id) → string or #f
static sexp sexp_ast_node_text(sexp ctx, sexp self, sexp_sint_t n,
                                sexp arg_id) {
    uint32_t id = (uint32_t)sexp_unbox_fixnum(arg_id);
    const AstNode *node = ast_ffi_node_get(id, NULL, NULL);
    if (!node || !node->text) return SEXP_FALSE;
    return sexp_c_string(ctx, node->text, -1);
}

// Lexical string payload.  This removes only the delimiter quotes, matching
// the bootstrap reader's current string-literal contract; it assigns no
// source-language meaning to the token.
static sexp sexp_ast_node_string_value(sexp ctx, sexp self, sexp_sint_t n,
                                       sexp arg_id) {
    uint32_t id = (uint32_t)sexp_unbox_fixnum(arg_id);
    const AstNode *node = ast_ffi_node_get(id, NULL, NULL);
    size_t len;
    if (!node || !node->text) return SEXP_FALSE;
    len = strlen(node->text);
    if (len < 2 || node->text[0] != '"' || node->text[len - 1] != '"')
        return SEXP_FALSE;
    return sexp_c_string(ctx, node->text + 1, (sexp_sint_t)(len - 2));
}

// Batched topology predicates used by the self-hosted Meta reader.  They do
// not assign language meaning; they only avoid repeated VM crossings for the
// common kind/op/text query sequences.
static sexp sexp_ast_node_is_atom_text(sexp ctx, sexp self, sexp_sint_t n,
                                       sexp arg_id, sexp arg_text) {
    uint32_t id = (uint32_t)sexp_unbox_fixnum(arg_id);
    const AstNode *node = ast_ffi_node_get(id, NULL, NULL);
    const char *text = sexp_to_c_string(ctx, arg_text);
    return sexp_make_fixnum(node && node->kind == AST_ATOM && node->text &&
                            text && strcmp(node->text, text) == 0 ? 1 : 0);
}

static sexp sexp_ast_node_is_infix_text(sexp ctx, sexp self, sexp_sint_t n,
                                        sexp arg_id, sexp arg_text) {
    uint32_t id = (uint32_t)sexp_unbox_fixnum(arg_id);
    AstSyntaxUnit *unit = NULL;
    const AstNode *node = ast_ffi_node_get(id, &unit, NULL);
    const AstNode *op = node ? (unit ? ast_get(&unit->arena, node->op)
                                     : ast_get(&g_ast_arena, node->op)) : NULL;
    const char *text = sexp_to_c_string(ctx, arg_text);
    return sexp_make_fixnum(node && node->kind == AST_INFIX && op && op->text &&
                            text && strcmp(op->text, text) == 0 ? 1 : 0);
}

// 0=non-atom, 1=canonical unsigned decimal atom, 2=quoted atom, 3=other atom.
static sexp sexp_ast_node_atom_class(sexp ctx, sexp self, sexp_sint_t n,
                                     sexp arg_id) {
    uint32_t id = (uint32_t)sexp_unbox_fixnum(arg_id);
    const AstNode *node = ast_ffi_node_get(id, NULL, NULL);
    const char *text;
    size_t len;
    size_t i;
    if (!node || node->kind != AST_ATOM || !node->text)
        return sexp_make_fixnum(0);
    text = node->text;
    len = strlen(text);
    if (len >= 2 && text[0] == '"' && text[len - 1] == '"')
        return sexp_make_fixnum(2);
    if (len > 0 && !(len > 1 && text[0] == '0')) {
        for (i = 0; i < len && text[i] >= '0' && text[i] <= '9'; i++) {}
        if (i == len) return sexp_make_fixnum(1);
    }
    return sexp_make_fixnum(3);
}

// ast.node-line(id) → line number
static sexp sexp_ast_node_line(sexp ctx, sexp self, sexp_sint_t n,
                                sexp arg_id) {
    uint32_t id = (uint32_t)sexp_unbox_fixnum(arg_id);
    const AstNode *node = ast_ffi_node_get(id, NULL, NULL);
    if (!node) return sexp_make_fixnum(0);
    return sexp_make_fixnum((sexp_sint_t)node->line);
}

// ast.node-col(id) → column number
static sexp sexp_ast_node_col(sexp ctx, sexp self, sexp_sint_t n,
                               sexp arg_id) {
    uint32_t id = (uint32_t)sexp_unbox_fixnum(arg_id);
    const AstNode *node = ast_ffi_node_get(id, NULL, NULL);
    if (!node) return sexp_make_fixnum(0);
    return sexp_make_fixnum((sexp_sint_t)node->col);
}

// ast.node-left(id) → child id or 0
static sexp sexp_ast_node_left(sexp ctx, sexp self, sexp_sint_t n,
                                sexp arg_id) {
    uint32_t id = (uint32_t)sexp_unbox_fixnum(arg_id);
    AstSyntaxUnit *unit = NULL;
    const AstNode *node = ast_ffi_node_get(id, &unit, NULL);
    if (!node) return sexp_make_fixnum(0);
    return sexp_make_fixnum((sexp_sint_t)ast_ffi_child_handle(unit, node->left));
}

// ast.node-right(id) → child id or 0
static sexp sexp_ast_node_right(sexp ctx, sexp self, sexp_sint_t n,
                                 sexp arg_id) {
    uint32_t id = (uint32_t)sexp_unbox_fixnum(arg_id);
    AstSyntaxUnit *unit = NULL;
    const AstNode *node = ast_ffi_node_get(id, &unit, NULL);
    if (!node) return sexp_make_fixnum(0);
    return sexp_make_fixnum((sexp_sint_t)ast_ffi_child_handle(unit, node->right));
}

// ast.node-op(id) → child id or 0
static sexp sexp_ast_node_op(sexp ctx, sexp self, sexp_sint_t n,
                              sexp arg_id) {
    uint32_t id = (uint32_t)sexp_unbox_fixnum(arg_id);
    AstSyntaxUnit *unit = NULL;
    const AstNode *node = ast_ffi_node_get(id, &unit, NULL);
    if (!node) return sexp_make_fixnum(0);
    return sexp_make_fixnum((sexp_sint_t)ast_ffi_child_handle(unit, node->op));
}

// ast.node-next(id) → sibling id or 0
static sexp sexp_ast_node_next(sexp ctx, sexp self, sexp_sint_t n,
                                sexp arg_id) {
    uint32_t id = (uint32_t)sexp_unbox_fixnum(arg_id);
    AstSyntaxUnit *unit = NULL;
    const AstNode *node = ast_ffi_node_get(id, &unit, NULL);
    if (!node) return sexp_make_fixnum(0);
    return sexp_make_fixnum((sexp_sint_t)ast_ffi_child_handle(unit, node->next));
}

// ast.destroy!() — free arena
static sexp sexp_ast_destroy(sexp ctx, sexp self, sexp_sint_t n) {
    if (g_ast_arena_inited) {
        ast_arena_destroy(&g_ast_arena);
        g_ast_arena_inited = 0;
    }
    return SEXP_VOID;
}

// ============================================================================
// FFI Registration (in builder_ffi.c, not native_runtime.c)
// ============================================================================

#include "compiler/builder_ffi.h"

void native_register_core_ffi(
    void *ctx_ptr, void *env_ptr, native_foreign_registrar registrar,
    void *user_data) {
  (void)ctx_ptr;
  (void)env_ptr;
#ifdef REG
#undef REG
#endif
#define REG(name, args, fn) registrar(user_data, name, args, (void *)(fn))
  REG("core.make-bits", 1, sexp_core_make_bits);
  REG("core.make-addr", 0, sexp_core_make_addr);
  REG("core.make-unit", 0, sexp_core_make_unit);
  REG("core.make-floats", 1, sexp_core_make_floats);
  REG("core.make-simd", 2, sexp_core_make_simd);
  REG("core.make-product-type!", 1, sexp_core_make_product_type);
  REG("type.registered-raw", 1, sexp_type_registered);
  REG("core.make-set", 2, sexp_core_make_set);
  REG("core.make-proc", 4, sexp_core_make_proc);
  REG("core.const-bits!", 3, sexp_core_const_bits);
  REG("core.const-string!", 3, sexp_core_const_string);
  REG("core.load!", 3, sexp_core_load);
  REG("core.store!", 3, sexp_core_store);
  REG("core.lea!", 3, sexp_core_lea);
  REG("core.begin-function!", 3, sexp_core_begin_function);
  REG("core.function-by-name", 1, sexp_core_function_by_name);
  REG("core.append-block!", 1, sexp_core_append_block);
  REG("core.return-value!", 2, sexp_core_return_value);
  REG("core.return-none!", 1, sexp_core_return_none);
  REG("core.function-return-type", 1, sexp_core_function_return_type);
  REG("core.function-param-types", 1, sexp_core_function_param_types);
  REG("core.function-link-name", 1, sexp_core_function_link_name);
  REG("core.call!", 3, sexp_core_call);
  REG("core.primitive!", 4, sexp_core_primitive);
  REG("core.add!", 2, sexp_core_add);
  REG("core.sub!", 2, sexp_core_sub);
  REG("core.mul!", 2, sexp_core_mul);
  REG("core.div!", 2, sexp_core_div);
  REG("core.eq!", 2, sexp_core_eq);
  REG("core.ne!", 2, sexp_core_ne);
  REG("core.lt!", 2, sexp_core_lt);
  REG("core.le!", 2, sexp_core_le);
  REG("core.gt!", 2, sexp_core_gt);
  REG("core.ge!", 2, sexp_core_ge);
  REG("core.fadd!", 2, sexp_core_fadd);
  REG("core.fsub!", 2, sexp_core_fsub);
  REG("core.fmul!", 2, sexp_core_fmul);
  REG("core.fdiv!", 2, sexp_core_fdiv);
  REG("core.feq!", 2, sexp_core_feq);
  REG("core.flt!", 2, sexp_core_flt);
  REG("core.popcount!", 1, sexp_core_popcount);
  REG("core.clz!", 1, sexp_core_clz);
  REG("core.rotl!", 1, sexp_core_rotl);
  REG("core.int2ptr!", 1, sexp_core_int2ptr);
  REG("core.ptr2int!", 1, sexp_core_ptr2int);
  REG("core.local-alloc!", 3, sexp_core_local_alloc);
  REG("core.param", 2, sexp_core_param);
  REG("core.block-function", 1, sexp_core_block_function);
  REG("core.branch!", 2, sexp_core_branch);
  REG("core.cond-branch!", 4, sexp_core_cond_branch);
  REG("core.type-is-void!", 1, sexp_core_type_is_void);
  REG("core.type-is-addr!", 1, sexp_core_type_is_addr);
  REG("core.type-size-in-bytes!", 1, sexp_core_type_size);
  REG("core.field-offset!", 4, sexp_core_field_offset);
  REG("core.aggregate-layout!", 3, sexp_core_aggregate_layout);
  REG("core.call-indirect!", 4, sexp_core_call_indirect);
  REG("core.function-ref!", 1, sexp_core_function_ref);
  REG("core.declare-extern-function!", 4, sexp_core_declare_extern_function);
  REG("core.call-expr!", 3, sexp_core_call_expr);
  REG("core.set-current-block!", 1, sexp_core_set_current_block);
  REG("core.get-current-block", 0, sexp_core_get_current_block);
  REG("core.begin-if!", 2, sexp_core_begin_if);
  REG("core.end-if!", 4, sexp_core_end_if);
  REG("core.assign-temp!", 2, sexp_core_assign_temp);
  REG("core.set-function-link-name!", 2, sexp_core_set_function_link_name);
  REG("core.declare-module!", 1, sexp_core_declare_module);
  REG("core.declare-signature!", 1, sexp_core_declare_signature);
  REG("core.mark-export!", 1, sexp_core_mark_export);
  REG("core.declare-interface-type!", 4,
      sexp_core_declare_interface_type);
  REG("core.declare-interface-function!", 3,
      sexp_core_declare_interface_function);
  REG("core.eval!", 3, sexp_core_eval);
  REG("core.eval-value!", 1, sexp_core_eval_value);

  // LAIN-AST FFI
  REG("ast.parse!", 2, sexp_ast_parse);
  REG("ast.node-count", 0, sexp_ast_node_count);
  REG("ast.node-kind", 1, sexp_ast_node_kind);
  REG("ast.node-text", 1, sexp_ast_node_text);
  REG("ast.node-string-value", 1, sexp_ast_node_string_value);
  REG("ast.node-is-atom-text", 2, sexp_ast_node_is_atom_text);
  REG("ast.node-is-infix-text", 2, sexp_ast_node_is_infix_text);
  REG("ast.node-atom-class", 1, sexp_ast_node_atom_class);
  REG("ast.node-line", 1, sexp_ast_node_line);
  REG("ast.node-col", 1, sexp_ast_node_col);
  REG("ast.node-left", 1, sexp_ast_node_left);
  REG("ast.node-right", 1, sexp_ast_node_right);
  REG("ast.node-op", 1, sexp_ast_node_op);
  REG("ast.node-next", 1, sexp_ast_node_next);
  REG("ast.destroy!", 0, sexp_ast_destroy);
  REG("ast.store-new!", 0, sexp_ast_store_new);
  REG("ast.store-destroy!", 1, sexp_ast_store_destroy);
  REG("ast.unit-parse!", 3, sexp_ast_unit_parse);
  REG("ast.unit-destroy!", 2, sexp_ast_unit_destroy);
  REG("ast.unit-root", 2, sexp_ast_unit_root);
  REG("ast.unit-node-count", 2, sexp_ast_unit_node_count);
  REG("ast.unit-node-kind", 3, sexp_ast_unit_node_kind);
  REG("ast.unit-node-text", 3, sexp_ast_unit_node_text);
  REG("ast.unit-node-string-value", 3, sexp_ast_unit_node_string_value);
  REG("ast.unit-node-is-atom-text", 4, sexp_ast_unit_node_is_atom_text);
  REG("ast.unit-node-is-infix-text", 4, sexp_ast_unit_node_is_infix_text);
  REG("ast.unit-node-atom-class", 3, sexp_ast_unit_node_atom_class);
  REG("ast.unit-node-line", 3, sexp_ast_unit_node_line);
  REG("ast.unit-node-col", 3, sexp_ast_unit_node_col);
  REG("ast.unit-node-left", 3, sexp_ast_unit_node_left);
  REG("ast.unit-node-right", 3, sexp_ast_unit_node_right);
  REG("ast.unit-node-op", 3, sexp_ast_unit_node_op);
  REG("ast.unit-node-next", 3, sexp_ast_unit_node_next);
#undef REG
}

// ============================================================================
