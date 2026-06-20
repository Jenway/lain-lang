// compiler/native_runtime.h — Forward declarations for @foreign C functions
// Included before the bootstrap-generated C code to satisfy implicit declarations.

#ifndef NATIVE_RUNTIME_H
#define NATIVE_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

// Native compiler entry points
uint32_t compile(const void *input_path, const void *output_path);
// Legacy compatibility alias for --emit-manifest.
uint32_t compile_manifest(const void *input_path, const void *output_path);
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
void native_push_token(const uint8_t *src, int32_t kind,
                       size_t start, size_t len, int64_t int_val);
void *native_finish_grouping(void);

// Token tree → Scheme S-expression (eliminates cursor FFI)
void *native_lex_to_sexp(void *ctx, const uint8_t *src, uint32_t len);

// scheme integration
void *native_init_scheme(void);
int32_t native_run_pipeline(void *ctx, void *root_group);

// code generation helpers
void native_emit_module_to_file(void *subs, const char *output_path);
void native_emit_l1_module(const char *output_path);
void native_emit_interface(const char *output_path);
// Legacy compatibility alias for old manifest terminology.
void native_emit_manifest(const char *output_path);
void *native_get_subroutines(void);
void native_declare_module(const char *name);
void native_declare_signature(const char *name);
void native_mark_export(const char *name);
int native_has_explicit_exports(void);
int native_is_export_marked(const char *name);

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
