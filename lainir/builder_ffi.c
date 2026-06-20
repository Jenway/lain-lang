/**
 * lainir/builder_ffi.c — L1 IR Builder FFI Functions
 *
 * Provides the C-side FFI functions that Scheme meta passes call to construct
 * L1 IR nodes (types, expressions, instructions, blocks, subroutines).
 *
 * Every function here allocates L1 IR nodes on the C heap and returns
 * cpointer values to Scheme.  These are the MUST_STAY C primitives
 * — they cannot move to Scheme because L1 IR is a C data structure.
 */

#include "lainir.h"
#include <chibi/eval.h>
#include "compiler/native_runtime.h"

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
  } else if (strcmp(name, "i32") == 0 || strcmp(name, "u32") == 0) {
    ty->kind = TY_BITS;
    ty->width = 32;
  } else if (strcmp(name, "i64") == 0 || strcmp(name, "u64") == 0) {
    ty->kind = TY_BITS;
    ty->width = 64;
  } else if (strcmp(name, "addr") == 0) {
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

  L1Block *block = malloc(sizeof(L1Block));
  block->id = 0;
  block->body = NULL;
  block->body_tail = NULL;
  block->terminator = NULL;
  block->parent = sub;
  block->next = NULL;
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
  L1Block *block = malloc(sizeof(L1Block));
  block->id = sub->blocks_tail ? sub->blocks_tail->id + 1 : 0;
  block->body = NULL;
  block->body_tail = NULL;
  block->terminator = NULL;
  block->parent = sub;
  block->next = NULL;
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
  if (!block->terminator) {
    block->terminator = malloc(sizeof(L1Terminator));
    block->terminator->kind = TERM_RETURN;
    block->terminator->data.ret_val = val;
  }
  return SEXP_VOID;
}

static sexp sexp_core_return_none(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp arg_block) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  if (!block->terminator) {
    block->terminator = malloc(sizeof(L1Terminator));
    block->terminator->kind = TERM_RETURN;
    block->terminator->data.ret_val = NULL;
  }
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
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_CALL;
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

  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
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

  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_CALL;
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
  inst->data.set.val = expr;
  inst->next = NULL;
  append_instruction(g_current_sub, inst);

  // Return EXPR_VAR referencing the temp variable
  L1Expr *var = malloc(sizeof(L1Expr));
  var->kind = EXPR_VAR;
  var->data.var.name = strdup(name);
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
  expr->data.arg_idx = idx;
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
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Block *target = (L1Block *)sexp_cpointer_value(arg_target);
  if (!block->terminator) {
    block->terminator = malloc(sizeof(L1Terminator));
    block->terminator->kind = TERM_BRANCH;
    block->terminator->data.target_id = target->id;
  }
  return SEXP_VOID;
}

static sexp sexp_core_cond_branch(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp arg_block, sexp arg_cond, sexp arg_true,
                                  sexp arg_false) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Expr *cond = (L1Expr *)sexp_cpointer_value(arg_cond);
  L1Block *true_bb = (L1Block *)sexp_cpointer_value(arg_true);
  L1Block *false_bb = (L1Block *)sexp_cpointer_value(arg_false);
  if (!block->terminator) {
    block->terminator = malloc(sizeof(L1Terminator));
    block->terminator->kind = TERM_COND_BRANCH;
    block->terminator->data.cond_branch.condition = cond;
    block->terminator->data.cond_branch.true_id = true_bb->id;
    block->terminator->data.cond_branch.false_id = false_bb->id;
  }
  return SEXP_VOID;
}

static sexp sexp_core_type_is_void(sexp ctx, sexp self, sexp_sint_t n,
                                   sexp arg_ty) {
  L1Type *ty = (L1Type *)sexp_cpointer_value(arg_ty);
  return (ty && ty->kind == TY_UNIT) ? SEXP_TRUE : SEXP_FALSE;
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
  L1Block *tb = calloc(1, sizeof(L1Block));
  L1Block *eb = calloc(1, sizeof(L1Block));
  tb->id = ++g_block_id_counter;
  eb->id = ++g_block_id_counter;
  tb->parent = NULL;
  eb->parent = NULL;
  g_scratch_blocks[g_scratch_count++] = tb;
  g_scratch_blocks[g_scratch_count++] = eb;
  sexp ts = sexp_make_cpointer(ctx, SEXP_CPOINTER, tb, SEXP_FALSE, 0);
  sexp es = sexp_make_cpointer(ctx, SEXP_CPOINTER, eb, SEXP_FALSE, 0);
  return sexp_cons(ctx, ts, sexp_cons(ctx, es, SEXP_NULL));
}

static sexp sexp_core_end_if(sexp ctx, sexp self, sexp_sint_t n, sexp bv, sexp cv, sexp tv, sexp ev) {
  L1Block *parent = (L1Block *)sexp_cpointer_value(bv);
  L1Expr *cond = (L1Expr *)sexp_cpointer_value(cv);
  L1Block *tb = (L1Block *)sexp_cpointer_value(tv);
  L1Block *eb = (L1Block *)sexp_cpointer_value(ev);
  scratch_terminator_to_inst(tb);
  scratch_terminator_to_inst(eb);
  L1Instruction *inst = calloc(1, sizeof(L1Instruction));
  inst->kind = INST_IF;
  inst->data.if_stmt.condition = cond;
  inst->data.if_stmt.then_body = tb->body;
  inst->data.if_stmt.else_body = eb->body;
  L1Block *saved = g_current_block;
  g_current_block = parent;
  append_instruction(NULL, inst);
  g_current_block = saved;
  free(tb);
  free(eb);
  for (int j = 0; j < g_scratch_count; j++)
    if (g_scratch_blocks[j] == tb || g_scratch_blocks[j] == eb)
      g_scratch_blocks[j] = NULL;
  return SEXP_VOID;
}


// ── lex-to-sexp FFI (token tree → Scheme S-expression) ──────────────────

static sexp sexp_lex_to_sexp(sexp ctx, sexp self, sexp_sint_t n,
                              sexp arg_src, sexp arg_len) {
  const uint8_t *src = (const uint8_t *)sexp_cpointer_value(arg_src);
  uint32_t len = sexp_unbox_fixnum(arg_len);
  return (sexp)native_lex_to_sexp(ctx, src, len);
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
