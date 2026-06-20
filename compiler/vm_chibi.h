#ifndef VM_CHIBI_H
#define VM_CHIBI_H

#include <chibi/eval.h>

void vm_chibi_set_module_path_from_cwd(void);
sexp vm_chibi_create_context(void);
sexp vm_chibi_env(sexp ctx);
int vm_chibi_is_exception(sexp value);
void vm_chibi_print_exception(sexp ctx, sexp value);
void vm_chibi_check(sexp ctx, sexp value);
void vm_chibi_import_base(sexp ctx, sexp env);
void vm_chibi_eval_string(sexp ctx, sexp env, const char *code);
void vm_chibi_load_file(sexp ctx, sexp env, const char *path);
int vm_chibi_load_first_available(sexp ctx, sexp env, const char **paths);
sexp vm_chibi_env_ref(sexp ctx, sexp env, const char *name);

#endif
