#include "lainir/core.h"
#include "lainir/verify.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct Name {
  const char *value;
  L1Type *ty;
  const struct Name *next;
} Name;

typedef struct LoopScope {
  const char *label;
  const struct LoopScope *parent;
} LoopScope;

typedef struct {
  L1Subroutine *module;
  L1Subroutine *sub;
  L1Diagnostic *diagnostic;
  int current_line;
  int current_column;
} VerifyContext;

static int verify_block(VerifyContext *ctx, L1Block *block,
                        const Name *incoming, const LoopScope *loops);

static int fail(VerifyContext *ctx, int code, const char *format, ...) {
  va_list args;
  if (ctx->diagnostic) {
    ctx->diagnostic->code = code;
    ctx->diagnostic->line = ctx->current_line;
    ctx->diagnostic->column = ctx->current_column;
    va_start(args, format);
    vsnprintf(ctx->diagnostic->message, sizeof(ctx->diagnostic->message),
              format, args);
    va_end(args);
  }
  return 0;
}

static int same_type(const L1Type *left, const L1Type *right) {
  return left && right && left->kind == right->kind && left->width == right->width;
}

static L1Subroutine *find_subroutine(L1Subroutine *head, const char *name) {
  for (; head; head = head->next)
    if (head->name && name && strcmp(head->name, name) == 0)
      return head;
  return NULL;
}

static const Name *find_name(const Name *names, const char *value) {
  for (; names; names = names->next)
    if (strcmp(names->value, value) == 0)
      return names;
  return NULL;
}

static int name_exists(const Name *names, const char *value) {
  return find_name(names, value) != NULL;
}

static int loop_exists(const LoopScope *loop, const char *label) {
  for (; loop; loop = loop->parent)
    if (!label || (loop->label && strcmp(loop->label, label) == 0))
      return 1;
  return 0;
}

static int verify_expr(VerifyContext *ctx, L1Expr *expr, const Name *names) {
  uint32_t i;
  L1Subroutine *callee;
  if (!expr)
    return fail(ctx, 2001, "missing expression in procedure `%s`", ctx->sub->name);

  switch (expr->kind) {
  case EXPR_VAR: {
    const Name *binding = expr->data.var.name ?
                              find_name(names, expr->data.var.name) : NULL;
    if (!binding)
      return fail(ctx, 2002, "undefined value `%%%s` in procedure `%s`",
                  expr->data.var.name ? expr->data.var.name : "?", ctx->sub->name);
    /* Resolving a variable is also the point where a parsed use receives its
       declared type.  No later verifier rule is allowed to treat it as
       type-unknown. */
    expr->data.var.ty = binding->ty;
    return 1;
  }
  case EXPR_ARG:
    if (expr->data.arg.index >= ctx->sub->param_count)
      return fail(ctx, 2003, "argument %u is out of range in procedure `%s`",
                  expr->data.arg.index, ctx->sub->name);
    /* Parser-created #arg nodes receive their type from the signature. */
    expr->data.arg.ty = ctx->sub->param_tys[expr->data.arg.index];
    return 1;
  case EXPR_ADD: case EXPR_SUB: case EXPR_MUL: case EXPR_DIV:
  case EXPR_EQ: case EXPR_NE: case EXPR_LT: case EXPR_LE: case EXPR_GT: case EXPR_GE:
    return verify_expr(ctx, expr->data.bin.left, names) &&
           verify_expr(ctx, expr->data.bin.right, names);
  case EXPR_FADD: case EXPR_FSUB: case EXPR_FMUL: case EXPR_FDIV: case EXPR_FEQ: case EXPR_FLT: {
    L1Type *left;
    L1Type *right;
    if (!verify_expr(ctx, expr->data.bin.left, names) ||
        !verify_expr(ctx, expr->data.bin.right, names)) return 0;
    left = infer_expr_type(expr->data.bin.left);
    right = infer_expr_type(expr->data.bin.right);
    if (!left || !right || left->kind != TY_FLOATS ||
        right->kind != TY_FLOATS || left->width != right->width)
      return fail(ctx, 2039, "float operation requires equal-width #float operands");
    return 1;
  }
  case EXPR_SDIV: case EXPR_UDIV:
  case EXPR_SLT: case EXPR_SLE: case EXPR_SGT: case EXPR_SGE:
  case EXPR_ULT: case EXPR_ULE: case EXPR_UGT: case EXPR_UGE: {
    L1Type *left;
    L1Type *right;
    if (!verify_expr(ctx, expr->data.bin.left, names) ||
        !verify_expr(ctx, expr->data.bin.right, names))
      return 0;
    left = infer_expr_type(expr->data.bin.left);
    right = infer_expr_type(expr->data.bin.right);
    if (!left || !right || left->kind != TY_BITS || right->kind != TY_BITS)
      return fail(ctx, 2030, "explicit integer operation requires #bits operands");
    if (expr->data.bin.left->kind != EXPR_CONST &&
        expr->data.bin.right->kind != EXPR_CONST &&
        !same_type(left, right))
      return fail(ctx, 2031, "explicit integer operand widths do not match");
    return 1;
  }
  case EXPR_POPCOUNT: case EXPR_CLZ: case EXPR_ROTL:
  case EXPR_INT2PTR: case EXPR_PTR2INT:
    return verify_expr(ctx, expr->data.unary.operand, names);
  case EXPR_ZEXT: case EXPR_SEXT: case EXPR_TRUNC: {
    L1Type *source;
    if (!verify_expr(ctx, expr->data.conversion.operand, names)) return 0;
    source = infer_expr_type(expr->data.conversion.operand);
    if (!source || source->kind != TY_BITS ||
        !expr->data.conversion.target_ty ||
        expr->data.conversion.target_ty->kind != TY_BITS)
      return fail(ctx, 2023, "integer conversion requires #bits operands");
    if ((expr->kind == EXPR_TRUNC &&
         expr->data.conversion.target_ty->width >= source->width) ||
        (expr->kind != EXPR_TRUNC &&
         expr->data.conversion.target_ty->width <= source->width))
      return fail(ctx, 2024, "integer conversion has invalid width direction");
    return 1;
  }
  case EXPR_BITCAST: {
    L1Type *source;
    L1Type *target = expr->data.conversion.target_ty;
    if (!verify_expr(ctx, expr->data.conversion.operand, names)) return 0;
    source = infer_expr_type(expr->data.conversion.operand);
    if (!source || !target ||
        !((source->kind == TY_BITS || source->kind == TY_FLOATS) &&
          (target->kind == TY_BITS || target->kind == TY_FLOATS)) ||
        source->width != target->width)
      return fail(ctx, 2038,
                  "#bitcast requires equal-width #bits/#float physical types");
    return 1;
  }
  case EXPR_LOAD:
    if (!verify_expr(ctx, expr->data.load.addr, names)) return 0;
    {
      L1Type *address = infer_expr_type(expr->data.load.addr);
      if (!address || address->kind != TY_ADDR)
        return fail(ctx, 2032, "#load operand is not #addr");
    }
    return 1;
  case EXPR_LEA:
    if (!verify_expr(ctx, expr->data.lea.base, names) ||
        (expr->data.lea.idx &&
         !verify_expr(ctx, expr->data.lea.idx, names)))
      return 0;
    {
      L1Type *base = infer_expr_type(expr->data.lea.base);
      L1Type *index = expr->data.lea.idx
                          ? infer_expr_type(expr->data.lea.idx) : NULL;
      if (!base || base->kind != TY_ADDR)
        return fail(ctx, 2033, "#lea base is not #addr");
      if (index && index->kind != TY_BITS)
        return fail(ctx, 2034, "#lea index is not #bits");
    }
    return 1;
  case EXPR_FIELD:
    if (!verify_expr(ctx, expr->data.field.base, names)) return 0;
    {
      L1Type *base = infer_expr_type(expr->data.field.base);
      if (!base || base->kind != TY_ADDR)
        return fail(ctx, 2035, "#field base is not #addr");
    }
    return 1;
  case EXPR_CALL:
    callee = find_subroutine(ctx->module, expr->data.call.fn_name);
    if (!callee)
      return fail(ctx, 2004, "unknown call target `%s` in procedure `%s`",
                  expr->data.call.fn_name, ctx->sub->name);
    if (callee->param_count != expr->data.call.arg_count)
      return fail(ctx, 2005, "call to `%s` expects %u arguments, got %u",
                  callee->name, callee->param_count, expr->data.call.arg_count);
    expr->data.call.ret_ty = callee->ret_ty;
    for (i = 0; i < expr->data.call.arg_count; i++) {
      L1Expr *arg = expr->data.call.args[i];
      if (arg->kind == EXPR_LOAD && !arg->data.load.ty)
        arg->data.load.ty = callee->param_tys[i];
      if (!verify_expr(ctx, arg, names)) return 0;
      if (!same_type(callee->param_tys[i], infer_expr_type(arg)) &&
          !(arg->kind == EXPR_CONST && callee->param_tys[i]->kind == TY_BITS))
        return fail(ctx, 2006, "argument %u to `%s` has incompatible type",
                    i, callee->name);
    }
    return 1;
  case EXPR_EVAL:
    if (!expr->data.eval.block)
      return fail(ctx, 2037, "#eval requires a block");
    expr->data.eval.ret_ty = ctx->sub->ret_ty;
    return verify_block(ctx, expr->data.eval.block, names, NULL);
  case EXPR_PROC_ADDR:
    if (!find_subroutine(ctx->module, expr->data.proc_addr.fn_name))
      return fail(ctx, 2025, "unknown procedure address `%s`",
                  expr->data.proc_addr.fn_name);
    return 1;
  case EXPR_CALL_INDIRECT:
    if (!verify_expr(ctx, expr->data.call_indirect.fn_ptr, names)) return 0;
    {
      L1Type *target_ty = infer_expr_type(expr->data.call_indirect.fn_ptr);
      if (!target_ty || target_ty->kind != TY_ADDR)
        return fail(ctx, 2026, "call_indirect target is not #addr");
    }
    if (expr->data.call_indirect.param_count !=
        expr->data.call_indirect.arg_count)
      return fail(ctx, 2027, "call_indirect signature arity mismatch");
    for (i = 0; i < expr->data.call_indirect.arg_count; i++) {
      L1Expr *arg = expr->data.call_indirect.args[i];
      if (!verify_expr(ctx, arg, names)) return 0;
      if (!same_type(expr->data.call_indirect.param_tys[i],
                     infer_expr_type(arg)) &&
          !(arg->kind == EXPR_CONST &&
            expr->data.call_indirect.param_tys[i]->kind == TY_BITS))
        return fail(ctx, 2028, "call_indirect argument %u type mismatch", i);
    }
    if (expr->data.call_indirect.fn_ptr->kind == EXPR_PROC_ADDR) {
      L1Subroutine *target = find_subroutine(
          ctx->module,
          expr->data.call_indirect.fn_ptr->data.proc_addr.fn_name);
      if (!target ||
          !same_type(target->ret_ty, expr->data.call_indirect.ret_ty) ||
          target->param_count != expr->data.call_indirect.param_count)
        return fail(ctx, 2029, "call_indirect direct target signature mismatch");
      for (i = 0; i < target->param_count; i++)
        if (!same_type(target->param_tys[i],
                       expr->data.call_indirect.param_tys[i]))
          return fail(ctx, 2029,
                      "call_indirect direct target signature mismatch");
    }
    return 1;
  case EXPR_PRIMITIVE:
    for (i = 0; i < expr->data.primitive.operand_count; i++)
      if (!verify_expr(ctx, expr->data.primitive.operands[i], names)) return 0;
    /* Legacy textual primitives do not carry a result annotation.  This is
       still a concrete inference rule, not an unknown-type escape hatch:
       a homogeneous primitive inherits its resolved operand type. */
    if (!expr->data.primitive.result_ty && expr->data.primitive.operand_count) {
      L1Type *candidate = infer_expr_type(expr->data.primitive.operands[0]);
      int homogeneous = candidate != NULL;
      for (i = 1; homogeneous && i < expr->data.primitive.operand_count; i++)
        homogeneous = same_type(candidate,
                                infer_expr_type(expr->data.primitive.operands[i]));
      if (homogeneous)
        expr->data.primitive.result_ty = candidate;
    }
    return 1;
  case EXPR_ALLOCA:
    if (!expr->data.alloca.byte_size &&
        (!expr->data.alloca.element_ty ||
         expr->data.alloca.element_ty->kind == TY_UNIT ||
         expr->data.alloca.element_ty->kind == TY_NEVER))
      return fail(ctx, 2036, "#alloca element has no physical size");
    return 1;
  case EXPR_CONST: case EXPR_STRING:
    return 1;
  }
  return fail(ctx, 2099, "unsupported expression kind %d", (int)expr->kind);
}

static int expression_is_condition(L1Expr *expr) {
  L1Type *type;
  if (!expr) return 0;
  if (expr->kind == EXPR_CONST)
    return expr->data.const_val == 0 || expr->data.const_val == 1;
  if (expr->kind == EXPR_EQ || expr->kind == EXPR_NE || expr->kind == EXPR_LT ||
      expr->kind == EXPR_LE || expr->kind == EXPR_GT || expr->kind == EXPR_GE ||
      expr->kind == EXPR_SLT || expr->kind == EXPR_SLE ||
      expr->kind == EXPR_SGT || expr->kind == EXPR_SGE ||
      expr->kind == EXPR_ULT || expr->kind == EXPR_ULE ||
      expr->kind == EXPR_UGT || expr->kind == EXPR_UGE ||
      expr->kind == EXPR_FEQ || expr->kind == EXPR_FLT)
    return 1;
  type = infer_expr_type(expr);
  return type && type->kind == TY_BITS && type->width == 1;
}

static int value_type_compatible(L1Type *type, L1Expr *value) {
  L1Type *actual;
  if (!type) return 0;
  if (type->kind == TY_UNIT)
    return value == NULL ||
           (value && value->kind == EXPR_EVAL && value->data.eval.ret_ty &&
            value->data.eval.ret_ty->kind == TY_UNIT);
  if (!value) return 0;
  /* Textual #load deliberately carries no redundant result annotation.  Its
     result is contextually typed by the binding/return/store contract that
     consumes it. */
  if (value->kind == EXPR_LOAD && !value->data.load.ty)
    value->data.load.ty = type;
  if (value->kind == EXPR_CONST && type->kind == TY_BITS) return 1;
  /* The source-level Meta layer uses `0 as addr` for a null handle.  Keep
     the physical IR address type pointer-shaped while allowing that one
     untyped literal in a typed address position. */
  if (value->kind == EXPR_CONST && type->kind == TY_ADDR)
    return value->data.const_val == 0;
  actual = infer_expr_type(value);
  /* Integer literals and arithmetic are width-polymorphic in the text parser;
     a resolved value (variable or parameter) is not. */
  if (type->kind == TY_BITS && actual && actual->kind == TY_BITS &&
      value->kind != EXPR_VAR && value->kind != EXPR_ARG) return 1;
  return same_type(type, actual);
}

static int verify_block(VerifyContext *ctx, L1Block *block, const Name *incoming,
                        const LoopScope *loops) {
  const Name *names = incoming;
  int terminated = 0;
  for (L1Instruction *inst = block ? block->body : NULL; inst; inst = inst->next) {
    Name binding;
    ctx->current_line = inst->line;
    ctx->current_column = inst->column;
    if (terminated)
      return fail(ctx, 2010, "instruction follows structured terminator in `%s`",
                  ctx->sub->name);
    switch (inst->kind) {
    case INST_LET: case INST_SET: {
      const char *name = inst->kind == INST_LET ? inst->data.let.name : inst->data.set.name;
      L1Type **binding_ty = inst->kind == INST_LET ? &inst->data.let.ty : &inst->data.set.ty;
      L1Expr *value = inst->kind == INST_LET ? inst->data.let.val : inst->data.set.val;
      if (!verify_expr(ctx, value, names)) return 0;
      if (inst->kind == INST_LET && name_exists(names, name))
        return fail(ctx, 2011, "duplicate binding `%%%s` in `%s`", name, ctx->sub->name);
      /* Older text omitted the annotation.  Accept it at the parser boundary
         only when the expression gives us a concrete type, then canonical
         printing writes that type back out. */
      if (!*binding_ty)
        *binding_ty = infer_expr_type(value);
      if (!*binding_ty)
        return fail(ctx, 2015, "binding `%%%s` needs an explicit type", name);
      if (!value_type_compatible(*binding_ty, value))
        return fail(ctx, 2016, "binding `%%%s` does not match its declared type", name);
      binding.value = name; binding.ty = *binding_ty; binding.next = names; names = &binding;
      /* The binding node only needs to live through the recursive verification below;
         instruction lists are verified linearly, so recurse via the remaining list. */
      if (inst->next) {
        L1Block rest = {.body = inst->next};
        return verify_block(ctx, &rest, names, loops);
      }
      return 1;
    }
    case INST_STORE:
      if (!verify_expr(ctx, inst->data.store.val, names) ||
          !verify_expr(ctx, inst->data.store.dest, names)) return 0;
      if (!inst->data.store.store_ty)
        inst->data.store.store_ty = infer_expr_type(inst->data.store.val);
      if (!inst->data.store.store_ty)
        return fail(ctx, 2017, "#store requires an explicit physical type");
      if (!value_type_compatible(
              inst->data.store.store_ty, inst->data.store.val))
        return fail(ctx, 2018, "#store value does not match its physical type");
      {
        L1Type *dest_ty = infer_expr_type(inst->data.store.dest);
        if (!dest_ty || dest_ty->kind != TY_ADDR)
          return fail(ctx, 2019, "#store destination is not #addr");
      }
      break;
    case INST_CALL:
      if (!verify_expr(ctx, inst->data.call_inst.expr, names)) return 0;
      break;
    case INST_IF:
      if (!verify_expr(ctx, inst->data.if_stmt.condition, names)) return 0;
      if (!expression_is_condition(inst->data.if_stmt.condition))
        return fail(ctx, 2012, "#if condition in `%s` is not #bits<1>", ctx->sub->name);
      if (!verify_block(ctx, inst->data.if_stmt.then_body, names, loops) ||
          (inst->data.if_stmt.else_body &&
           !verify_block(ctx, inst->data.if_stmt.else_body, names, loops))) return 0;
      break;
    case INST_LOOP: {
      LoopScope scope = {.label = inst->data.loop.label, .parent = loops};
      if (!verify_block(ctx, inst->data.loop.body, names, &scope)) return 0;
      break;
    }
    case INST_BREAK: case INST_CONTINUE:
      if (!loop_exists(loops, inst->data.jump.label))
        return fail(ctx, 2013, "%s outside its target loop in `%s`",
                    inst->kind == INST_BREAK ? "#break" : "#continue", ctx->sub->name);
      terminated = 1;
      break;
    case INST_RETURN:
      if (!verify_expr(ctx, inst->data.ret.val, names) && inst->data.ret.val) return 0;
      if (!value_type_compatible(ctx->sub->ret_ty, inst->data.ret.val))
        return fail(ctx, 2014, "return type mismatch in `%s`", ctx->sub->name);
      terminated = 1;
      break;
    }
  }
  return 1;
}

int lainir_verify_module(L1Subroutine *head, const char *entry_name,
                         L1Diagnostic *diagnostic) {
  VerifyContext ctx = {.module = head, .diagnostic = diagnostic};
  if (diagnostic) memset(diagnostic, 0, sizeof(*diagnostic));
  if (entry_name && !find_subroutine(head, entry_name))
    return fail(&ctx, 2020, "entry procedure `%s` does not exist", entry_name);
  for (L1Subroutine *sub = head; sub; sub = sub->next) {
    for (L1Subroutine *other = head; other != sub; other = other->next)
      if (strcmp(other->name, sub->name) == 0)
        return fail(&ctx, 2021, "duplicate procedure `%s`", sub->name);
    if (sub->is_extern) continue;
    ctx.sub = sub;
    if (!sub->blocks)
      return fail(&ctx, 2022, "procedure `%s` has no body", sub->name);
    for (L1Block *block = sub->blocks; block; block = block->next)
      if (!verify_block(&ctx, block, NULL, NULL)) return 0;
  }
  return 1;
}
