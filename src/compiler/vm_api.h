/** vm_api.h — Virtual Machine Abstraction Layer
 *
 * All Lain compiler code uses ONLY these types and functions.
 * No #include <chibi/eval.h> anywhere except vm_chibi.c.
 *
 * vm_value  — opaque VM value (sexp on Chibi, tagged ptr on Chez, etc.)
 * vm_context — opaque VM execution context
 *
 * To swap VMs: write a new vm_<backend>.c + ffi_<backend>.c,
 * drop the old ones, recompile. Nothing else changes.
 */

#ifndef VM_API_H
#define VM_API_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════
 * Opaque types
 * ═══════════════════════════════════════════════════════════════ */

typedef void vm_value;
typedef void vm_context;

/* ═══════════════════════════════════════════════════════════════
 * Context lifecycle
 * ═══════════════════════════════════════════════════════════════ */

vm_context *vm_create(void);                  /* sexp_make_eval_context + load_standard_env */
vm_value   *vm_context_env(vm_context *ctx);  /* sexp_context_env */
void        vm_set_module_path(const char *path);  /* set CHIBI_MODULE_PATH (or equiv) */

/* ═══════════════════════════════════════════════════════════════
 * Evaluation
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_eval_string(vm_context *ctx, vm_value *env, const char *code);
vm_value *vm_apply(vm_context *ctx, vm_value *proc, vm_value *args);
vm_value *vm_load_file(vm_context *ctx, vm_value *env, const char *path);
int       vm_load_first(vm_context *ctx, vm_value *env, const char **paths);
void      vm_import_base(vm_context *ctx, vm_value *env);
void      vm_check(vm_context *ctx, vm_value *v);  /* abort if exception */

/* ═══════════════════════════════════════════════════════════════
 * Symbol interning & env lookup
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_intern(vm_context *ctx, const char *name);
vm_value *vm_env_ref(vm_context *ctx, vm_value *env, vm_value *sym);

/* ═══════════════════════════════════════════════════════════════
 * Type predicates
 * ═══════════════════════════════════════════════════════════════ */

int vm_is_exception(vm_value *v);
int vm_is_pair(vm_value *v);
int vm_is_null(vm_value *v);
int vm_is_string(vm_value *v);
int vm_is_symbol(vm_value *v);
int vm_is_procedure(vm_value *v);
int vm_is_fixnum(vm_value *v);
int vm_is_integer(vm_value *v);

/* ═══════════════════════════════════════════════════════════════
 * Accessors
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_car(vm_value *pair);
vm_value *vm_cdr(vm_value *pair);
const char *vm_string_data(vm_value *str);
const char *vm_symbol_name(vm_context *ctx, vm_value *sym);  /* symbol → string */
int64_t     vm_fixnum_value(vm_value *fixnum);
int         vm_string_length(vm_value *str);
int         vm_list_length(vm_value *list);
uint32_t    vm_uint_value(vm_value *v);

/* ═══════════════════════════════════════════════════════════════
 * Constructors
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_cons(vm_context *ctx, vm_value *car, vm_value *cdr);
vm_value *vm_make_string(vm_context *ctx, const char *s, int len);
vm_value *vm_make_integer(vm_context *ctx, int64_t n);
vm_value *vm_make_fixnum(int n);
vm_value *vm_list1(vm_context *ctx, vm_value *a);
vm_value *vm_list2(vm_context *ctx, vm_value *a, vm_value *b);

/* ═══════════════════════════════════════════════════════════════
 * C pointer boxing (for passing L1 IR nodes to Scheme)
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_make_cpointer(vm_context *ctx, void *ptr);
void     *vm_cpointer_value(vm_value *v);

/* ═══════════════════════════════════════════════════════════════
 * GC pinning
 * ═══════════════════════════════════════════════════════════════ */

void vm_preserve(vm_context *ctx, vm_value *v);
void vm_release(vm_context *ctx, vm_value *v);

/* ═══════════════════════════════════════════════════════════════
 * String ports (for building output in Scheme)
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_open_output_string(vm_context *ctx);
void      vm_close_port(vm_context *ctx, vm_value *port);
vm_value *vm_get_output_string(vm_context *ctx, vm_value *port);
void      vm_write(vm_context *ctx, vm_value *obj, vm_value *port);

/* ═══════════════════════════════════════════════════════════════
 * Strings
 * ═══════════════════════════════════════════════════════════════ */

char *vm_to_c_string(vm_value *str);              /* caller must free */
vm_value *vm_read_from_string(vm_context *ctx, const char *s, int len);

/* ═══════════════════════════════════════════════════════════════
 * Errors
 * ═══════════════════════════════════════════════════════════════ */

void      vm_print_exception(vm_context *ctx, vm_value *exn);
vm_value *vm_user_exception(vm_context *ctx, const char *msg);

/* ═══════════════════════════════════════════════════════════════
 * Error port
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_error_port(vm_context *ctx);

/* ═══════════════════════════════════════════════════════════════
 * Well-known constants
 * ═══════════════════════════════════════════════════════════════ */

vm_value *vm_false(void);
vm_value *vm_true(void);
vm_value *vm_null(void);
vm_value *vm_void(void);

/* ═══════════════════════════════════════════════════════════════
 * FFI registration (backend-specific .c file uses vm_register_ffi
 * to expose C functions to Scheme, wrapping the Chibi/Chez FFI ABI)
 * ═══════════════════════════════════════════════════════════════ */

typedef vm_value *(*vm_ffi_fn)(vm_context *ctx, vm_value *self,
                               int nargs, vm_value *args);

void vm_register_ffi(vm_context *ctx, vm_value *env,
                     const char *scheme_name, int arity, vm_ffi_fn fn);

#endif /* VM_API_H */
