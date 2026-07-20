// compiler/vm_compat.h -- compatibility shims for old sexp-shaped FFI code.
//
// This header lets native_runtime.c and builder_ffi.c depend on vm_api.h
// instead of including a concrete Scheme implementation header.  The names are
// intentionally old Chibi-style names so the large FFI surface can migrate in
// small, reviewable steps.

#ifndef VM_COMPAT_H
#define VM_COMPAT_H

#include "compiler/vm_api.h"

typedef vm_value *sexp;
typedef int sexp_sint_t;

#define SEXP_CPOINTER 0
#define SEXP_FALSE ((sexp)vm_false())
#define SEXP_TRUE  ((sexp)vm_true())
#define SEXP_NULL  ((sexp)vm_null())
#define SEXP_VOID  ((sexp)vm_void())

#define sexp_exceptionp(v) vm_is_exception((vm_value *)(v))
#define sexp_pairp(v) vm_is_pair((vm_value *)(v))
#define sexp_nullp(v) vm_is_null((vm_value *)(v))
#define sexp_stringp(v) vm_is_string((vm_value *)(v))
#define sexp_symbolp(v) vm_is_symbol((vm_value *)(v))
#define sexp_procedurep(v) vm_is_procedure((vm_value *)(v))
#define sexp_fixnump(v) vm_is_fixnum((vm_value *)(v))
#define sexp_integerp(v) vm_is_integer((vm_value *)(v))

#define sexp_car(v) ((sexp)vm_car((vm_value *)(v)))
#define sexp_cdr(v) ((sexp)vm_cdr((vm_value *)(v)))
#define sexp_string_data(v) vm_string_data((vm_value *)(v))
#define sexp_symbol_to_string(ctx, sym) \
  ((sexp)vm_make_string((vm_context *)(ctx), \
                        vm_symbol_name((vm_context *)(ctx), (vm_value *)(sym)), -1))
#define sexp_unbox_fixnum(v) ((sexp_sint_t)vm_fixnum_value((vm_value *)(v)))
#define sexp_string_length(v) vm_string_length((vm_value *)(v))
#define sexp_uint_value(v) vm_uint_value((vm_value *)(v))

#define sexp_cons(ctx, a, b) \
  ((sexp)vm_cons((vm_context *)(ctx), (vm_value *)(a), (vm_value *)(b)))
#define sexp_c_string(ctx, s, len) \
  ((sexp)vm_make_string((vm_context *)(ctx), (const char *)(s), (int)(len)))
#define sexp_make_integer(ctx, n) \
  ((sexp)vm_make_integer((vm_context *)(ctx), (int64_t)(n)))
#define sexp_make_fixnum(n) ((sexp)vm_make_fixnum((int)(n)))
#define sexp_list1(ctx, a) \
  ((sexp)vm_list1((vm_context *)(ctx), (vm_value *)(a)))
#define sexp_list2(ctx, a, b) \
  ((sexp)vm_list2((vm_context *)(ctx), (vm_value *)(a), (vm_value *)(b)))

#define sexp_make_cpointer(ctx, tag, ptr, owner, flags) \
  ((sexp)vm_make_cpointer((vm_context *)(ctx), (void *)(ptr)))
#define sexp_cpointer_value(v) vm_cpointer_value((vm_value *)(v))

#define sexp_context_env(ctx) ((sexp)vm_context_env((vm_context *)(ctx)))
#define sexp_eval_string(ctx, code, len, env) \
  ((sexp)vm_eval_string((vm_context *)(ctx), (vm_value *)(env), (const char *)(code)))
#define sexp_apply(ctx, proc, args) \
  ((sexp)vm_apply((vm_context *)(ctx), (vm_value *)(proc), (vm_value *)(args)))
#define sexp_env_ref(ctx, env, sym, fallback) \
  ((sexp)vm_env_ref((vm_context *)(ctx), (vm_value *)(env), (vm_value *)(sym)))
#define sexp_intern(ctx, name, len) \
  ((sexp)vm_intern((vm_context *)(ctx), (const char *)(name)))

#define sexp_read_from_string(ctx, s, len) \
  ((sexp)vm_read_from_string((vm_context *)(ctx), (const char *)(s), (int)(len)))
#define sexp_user_exception(ctx, self, msg, irritants) \
  ((sexp)vm_raise_user_exception((vm_context *)(ctx), (const char *)(msg)))

#define sexp_print_exception(ctx, exn, port) \
  vm_print_exception((vm_context *)(ctx), (vm_value *)(exn))
#define sexp_current_error_port(ctx) ((sexp)vm_error_port((vm_context *)(ctx)))

#endif
