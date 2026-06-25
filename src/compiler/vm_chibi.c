/** vm_chibi.c — Chibi-Scheme backend for vm_api.h
 *
 * The ONLY file in the compiler that #includes <chibi/eval.h>.
 * All vm_* functions here cast between void* and Chibi's sexp type.
 */

#include "vm_api.h"
#include <chibi/eval.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════
 * Context lifecycle
 * ═══════════════════════════════════════════════════════════════ */

void vm_set_module_path(const char *path) {
  setenv("CHIBI_MODULE_PATH", path, 1);
}

vm_context *vm_create(void) {
  sexp ctx = sexp_make_eval_context(NULL, NULL, NULL, 0, 0);
  sexp_load_standard_env(ctx, NULL, SEXP_SEVEN);
  return (vm_context *)ctx;
}

vm_value *vm_context_env(vm_context *ctx) {
  return (vm_value *)sexp_context_env((sexp)ctx);
}

/* ═══════════════════════════════════════════════════════════════
 * Evaluation
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_eval_string(vm_context *ctx, vm_value *env, const char *code) {
  return (vm_value *)sexp_eval_string((sexp)ctx, code, -1, (sexp)env);
}

vm_value *vm_apply(vm_context *ctx, vm_value *proc, vm_value *args) {
  return (vm_value *)sexp_apply((sexp)ctx, (sexp)proc, (sexp)args);
}

vm_value *vm_load_file(vm_context *ctx, vm_value *env, const char *path) {
  sexp c = (sexp)ctx;
  FILE *file = fopen(path, "r");
  if (!file) {
    fprintf(stderr, "Warning: cannot open file: %s\n", path);
    return (vm_value *)SEXP_VOID;
  }
  fseek(file, 0, SEEK_END);
  long len = ftell(file);
  fseek(file, 0, SEEK_SET);
  char *buf = malloc(len + 1);
  if (!buf) { fprintf(stderr, "OOM reading %s\n", path); exit(1); }
  fread(buf, 1, len, file);
  buf[len] = '\0';
  fclose(file);

  int wrapped_len = len + 9;
  char *wrapped = malloc(wrapped_len);
  if (!wrapped) { fprintf(stderr, "OOM wrapping %s\n", path); exit(1); }
  sprintf(wrapped, "(begin %s)", buf);
  free(buf);

  sexp result = sexp_eval_string(c, wrapped, -1, (sexp)env);
  free(wrapped);
  return (vm_value *)result;
}

int vm_load_first(vm_context *ctx, vm_value *env, const char **paths) {
  sexp c = (sexp)ctx;
  sexp e = (sexp)env;
  for (int i = 0; paths[i]; i++) {
    FILE *test = fopen(paths[i], "r");
    if (!test) continue;
    fclose(test);
    sexp result = (sexp)vm_load_file(ctx, env, paths[i]);
    if (sexp_exceptionp(result)) return 0;
    return 1;
  }
  return 0;
}

void vm_import_base(vm_context *ctx, vm_value *env) {
  sexp result = sexp_eval_string(
    (sexp)ctx,
    "(import (scheme base) (scheme cxr) (scheme load))",
    -1, (sexp)env);
  vm_check(ctx, (vm_value *)result);
}

void vm_check(vm_context *ctx, vm_value *v) {
  sexp value = (sexp)v;
  if (sexp_exceptionp(value)) {
    sexp_print_exception((sexp)ctx, value, sexp_current_error_port((sexp)ctx));
    fprintf(stderr, "\n");
    exit(1);
  }
}

/* ═══════════════════════════════════════════════════════════════
 * Symbol interning & env lookup
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_intern(vm_context *ctx, const char *name) {
  return (vm_value *)sexp_intern((sexp)ctx, name, -1);
}

vm_value *vm_env_ref(vm_context *ctx, vm_value *env, vm_value *sym) {
  return (vm_value *)sexp_env_ref((sexp)ctx, (sexp)env, (sexp)sym, SEXP_FALSE);
}

/* ═══════════════════════════════════════════════════════════════
 * Type predicates
 * ═══════════════════════════════════════════════════════════════ */

int vm_is_exception(vm_value *v) { return sexp_exceptionp((sexp)v); }
int vm_is_pair(vm_value *v)      { return sexp_pairp((sexp)v); }
int vm_is_null(vm_value *v)      { return sexp_nullp((sexp)v); }
int vm_is_string(vm_value *v)    { return sexp_stringp((sexp)v); }
int vm_is_symbol(vm_value *v)    { return sexp_symbolp((sexp)v); }
int vm_is_procedure(vm_value *v) { return sexp_procedurep((sexp)v); }
int vm_is_fixnum(vm_value *v)    { return sexp_fixnump((sexp)v); }
int vm_is_integer(vm_value *v)   { return sexp_integerp((sexp)v); }

/* ═══════════════════════════════════════════════════════════════
 * Accessors
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_car(vm_value *pair) {
  return (vm_value *)sexp_car((sexp)pair);
}
vm_value *vm_cdr(vm_value *pair) {
  return (vm_value *)sexp_cdr((sexp)pair);
}
const char *vm_string_data(vm_value *str) {
  return sexp_string_data((sexp)str);
}
const char *vm_symbol_name(vm_context *ctx, vm_value *sym) {
  return sexp_string_data(sexp_symbol_to_string((sexp)ctx, (sexp)sym));
}
int64_t vm_fixnum_value(vm_value *fixnum) {
  return (int64_t)sexp_unbox_fixnum((sexp)fixnum);
}
int vm_string_length(vm_value *str) {
  return sexp_string_length((sexp)str);
}
int vm_list_length(vm_value *list) {
  sexp l = (sexp)list;
  int len = 0;
  while (sexp_pairp(l)) { len++; l = sexp_cdr(l); }
  return len;
}
uint32_t vm_uint_value(vm_value *v) {
  return sexp_uint_value((sexp)v);
}

/* ═══════════════════════════════════════════════════════════════
 * Constructors
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_cons(vm_context *ctx, vm_value *car, vm_value *cdr) {
  return (vm_value *)sexp_cons((sexp)ctx, (sexp)car, (sexp)cdr);
}
vm_value *vm_make_string(vm_context *ctx, const char *s, int len) {
  return (vm_value *)sexp_c_string((sexp)ctx, s, len);
}
vm_value *vm_make_integer(vm_context *ctx, int64_t n) {
  return (vm_value *)sexp_make_integer((sexp)ctx, n);
}
vm_value *vm_make_fixnum(int n) {
  return (vm_value *)sexp_make_fixnum(n);
}
vm_value *vm_list1(vm_context *ctx, vm_value *a) {
  return (vm_value *)sexp_list1((sexp)ctx, (sexp)a);
}
vm_value *vm_list2(vm_context *ctx, vm_value *a, vm_value *b) {
  return (vm_value *)sexp_list2((sexp)ctx, (sexp)a, (sexp)b);
}

/* ═══════════════════════════════════════════════════════════════
 * C pointer boxing
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_make_cpointer(vm_context *ctx, void *ptr) {
  return (vm_value *)sexp_make_cpointer((sexp)ctx, SEXP_CPOINTER, ptr, SEXP_FALSE, 0);
}
void *vm_cpointer_value(vm_value *v) {
  return sexp_cpointer_value((sexp)v);
}

/* ═══════════════════════════════════════════════════════════════
 * GC pinning
 * ═══════════════════════════════════════════════════════════════ */

void vm_preserve(vm_context *ctx, vm_value *v) {
  sexp_preserve_object((sexp)ctx, (sexp)v);
}
void vm_release(vm_context *ctx, vm_value *v) {
  sexp_release_object((sexp)ctx, (sexp)v);
}

/* ═══════════════════════════════════════════════════════════════
 * String ports
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_open_output_string(vm_context *ctx) {
  return (vm_value *)sexp_open_output_string((sexp)ctx);
}
void vm_close_port(vm_context *ctx, vm_value *port) {
  sexp_close_port((sexp)ctx, (sexp)port);
}
vm_value *vm_get_output_string(vm_context *ctx, vm_value *port) {
  return (vm_value *)sexp_get_output_string((sexp)ctx, (sexp)port);
}
void vm_write(vm_context *ctx, vm_value *obj, vm_value *port) {
  sexp_write((sexp)ctx, (sexp)obj, (sexp)port);
}

/* ═══════════════════════════════════════════════════════════════
 * Strings
 * ═══════════════════════════════════════════════════════════════ */

char *vm_to_c_string(vm_value *str) {
  return strdup(sexp_string_data((sexp)str));
}
vm_value *vm_read_from_string(vm_context *ctx, const char *s, int len) {
  return (vm_value *)sexp_read_from_string((sexp)ctx, s, len);
}

/* ═══════════════════════════════════════════════════════════════
 * Errors
 * ═══════════════════════════════════════════════════════════════ */

void vm_print_exception(vm_context *ctx, vm_value *exn) {
  sexp_print_exception((sexp)ctx, (sexp)exn, sexp_current_error_port((sexp)ctx));
  fprintf(stderr, "\n");
}
vm_value *vm_user_exception(vm_context *ctx, const char *msg) {
  return (vm_value *)sexp_user_exception((sexp)ctx, NULL, msg, SEXP_NULL);
}

/* ═══════════════════════════════════════════════════════════════
 * Error port
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_error_port(vm_context *ctx) {
  return (vm_value *)sexp_current_error_port((sexp)ctx);
}

/* ═══════════════════════════════════════════════════════════════
 * Well-known constants
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_false(void) { return (vm_value *)SEXP_FALSE; }
vm_value *vm_true(void)  { return (vm_value *)SEXP_TRUE; }
vm_value *vm_null(void)  { return (vm_value *)SEXP_NULL; }
vm_value *vm_void(void)  { return (vm_value *)SEXP_VOID; }

/* ═══════════════════════════════════════════════════════════════
 * FFI registration (glue to Chibi's sexp_define_foreign)
 * ═══════════════════════════════════════════════════════════════ */

void vm_register_ffi(vm_context *ctx, vm_value *env,
                     const char *scheme_name, vm_ffi_fn fn) {
  sexp_define_foreign_aux((sexp)ctx, (sexp)env, scheme_name, -1, 0,
                          scheme_name, (sexp_proc1)fn, NULL);
}
