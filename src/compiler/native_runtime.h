// compiler/native_runtime.h — Forward declarations for @foreign C functions
// Included before the bootstrap-generated C code to satisfy implicit declarations.

#ifndef NATIVE_RUNTIME_H
#define NATIVE_RUNTIME_H

#include <stdint.h>
#include <stddef.h>
#include "lainir_exec.h"

typedef void (*native_foreign_registrar)(
  void *user_data,
  const char *name,
  int arity,
  void *fn);

// Native compiler entry points
uint32_t compile(const void *input_path, const void *output_path);
uint32_t compile_interface(const void *input_path, const void *output_path);
uint32_t compile_l1(const void *input_path, const void *output_path);
int32_t native_build_with_funcs(const char *root_path, const char *output_path,
    uint32_t (*compile_fn)(const void*, const void*),
    uint32_t (*interface_fn)(const void*, const void*));

// byte reading (used by lexer.lain)
uint8_t read_byte_at(const uint8_t *ptr, size_t offset);

// file I/O
const uint8_t *native_read_file(const char *path);
uint32_t native_file_len(void);

// token grouping
void *native_lex_and_group(const uint8_t *src, uint32_t len);

// scheme integration
void native_register_core_ffi(
  void *ctx,
  void *env,
  native_foreign_registrar registrar,
  void *user_data);
void *native_init_scheme(void);
// Registers only physical host capabilities required by a precompiled Lain
// compiler artifact.  It deliberately does not load Scheme Meta sources.
void *native_init_compiler_artifact_host(void);
int32_t native_run_pipeline(void *ctx, void *root_group);
LainirExecStatus native_execute_compiler_artifact(
  void *ctx,
  const char *artifact_text,
  const char *entry_name,
  const LainirValue *args,
  uint32_t arg_count,
  LainirValue *result_out,
  L1Diagnostic *diagnostic);
// Interpreter-only transition mode: imports are elaborated from Lain source
// into the same in-memory L1 module.  Normal compilation continues to use
// interface artifacts and does not acquire source-module policy in C.
void native_set_interpret_source_linking(int enabled);

// code generation helpers
void native_emit_module_to_file(void *subs, const char *output_path);
void native_emit_l1_module(const char *output_path);
void native_emit_interface(const char *output_path);
void *native_get_subroutines(void);
void native_declare_module(const char *name);
void native_declare_signature(const char *name);
void native_mark_export(const char *name);
int native_has_explicit_exports(void);
int native_is_export_marked(const char *name);

// Legacy interface transport. Meta owns visibility, nominal identity,
// semantic field types, and semantic function signatures; the C host only
// retains those already-decided records until interface serialization.
void native_reset_interface_metadata(void);
void native_declare_interface_type(
    const char *name, const char *identity, uint32_t size, uint32_t align,
    uint32_t field_count, const char *const *field_names,
    const char *const *field_types, const uint32_t *field_offsets);
void native_declare_interface_function(
    const char *name, uint32_t param_count,
    const char *const *param_types, const char *ret_type);

// environment
const char *native_getenv(const char *name);

// module prefix (for @foreign(lain) name mangling)
void native_set_source_path(const char *path);
const char *native_get_module_prefix(void);

uint32_t compile_impl(void *ctx, void *root_group, void *output_path);

// command-line arguments
void native_set_args(int argc, char **argv);
int32_t native_get_arg_count(void);
const char *native_get_arg(int32_t idx);

#endif // NATIVE_RUNTIME_H
