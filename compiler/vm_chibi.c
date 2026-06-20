#include "vm_chibi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void vm_chibi_set_module_path_from_cwd(void) {
  char cwd[2048];
  if (!getcwd(cwd, sizeof(cwd)))
    return;

  char project_root[2048];
  strcpy(project_root, cwd);
  char *p = strstr(project_root, "/compiler");
  if (p)
    *p = '\0';

  char module_path[2048];
  snprintf(module_path, sizeof(module_path),
           "%s/bootstrap/chibi-scheme/lib", project_root);
  setenv("CHIBI_MODULE_PATH", module_path, 1);
}

sexp vm_chibi_create_context(void) {
  sexp ctx = sexp_make_eval_context(NULL, NULL, NULL, 0, 0);
  sexp_load_standard_env(ctx, NULL, SEXP_SEVEN);
  return ctx;
}

sexp vm_chibi_env(sexp ctx) {
  return sexp_context_env(ctx);
}

int vm_chibi_is_exception(sexp value) {
  return sexp_exceptionp(value);
}

void vm_chibi_print_exception(sexp ctx, sexp value) {
  sexp_print_exception(ctx, value, sexp_current_error_port(ctx));
  fprintf(stderr, "\n");
}

void vm_chibi_check(sexp ctx, sexp value) {
  if (vm_chibi_is_exception(value)) {
    vm_chibi_print_exception(ctx, value);
    exit(1);
  }
}

void vm_chibi_import_base(sexp ctx, sexp env) {
  sexp result = sexp_eval_string(
    ctx,
    "(import (scheme base) (scheme cxr) (scheme load))",
    -1,
    env);
  vm_chibi_check(ctx, result);
}

void vm_chibi_eval_string(sexp ctx, sexp env, const char *code) {
  sexp result = sexp_eval_string(ctx, code, -1, env);
  vm_chibi_check(ctx, result);
}

void vm_chibi_load_file(sexp ctx, sexp env, const char *path) {
  FILE *file = fopen(path, "r");
  if (!file) {
    fprintf(stderr, "Warning: cannot open file: %s\n", path);
    return;
  }

  fseek(file, 0, SEEK_END);
  long len = ftell(file);
  fseek(file, 0, SEEK_SET);

  char *buf = malloc(len + 1);
  if (!buf) {
    fprintf(stderr, "OOM reading %s\n", path);
    exit(1);
  }

  fread(buf, 1, len, file);
  buf[len] = '\0';
  fclose(file);

  int wrapped_len = len + 9;
  char *wrapped = malloc(wrapped_len);
  if (!wrapped) {
    fprintf(stderr, "OOM wrapping %s\n", path);
    exit(1);
  }

  sprintf(wrapped, "(begin %s)", buf);
  free(buf);

  sexp result = sexp_eval_string(ctx, wrapped, -1, env);
  vm_chibi_check(ctx, result);
  free(wrapped);
}

int vm_chibi_load_first_available(sexp ctx, sexp env, const char **paths) {
  for (int i = 0; paths[i]; i++) {
    FILE *test = fopen(paths[i], "r");
    if (!test)
      continue;
    fclose(test);
    vm_chibi_load_file(ctx, env, paths[i]);
    return 1;
  }
  return 0;
}

sexp vm_chibi_env_ref(sexp ctx, sexp env, const char *name) {
  return sexp_env_ref(ctx, env, sexp_intern(ctx, name, -1), SEXP_FALSE);
}
