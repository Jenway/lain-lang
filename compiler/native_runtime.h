// compiler/native_runtime.h — Forward declarations for @foreign C functions
// Included before the bootstrap-generated C code to satisfy implicit declarations.

#ifndef NATIVE_RUNTIME_H
#define NATIVE_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

// main.lain compiled functions (bootstrap doesn't forward-declare them)
uint32_t compile(const void *input_path, const void *output_path);

// byte reading (used by lexer.lain)
uint8_t read_byte_at(const uint8_t *ptr, size_t offset);

// file I/O
const uint8_t *native_read_file(const char *path);
uint32_t native_file_len(void);

// token grouping
void *native_lex_and_group(const uint8_t *src, uint32_t len);

// Token tree → Scheme S-expression (eliminates cursor FFI)
void *native_lex_to_sexp(void *ctx, const uint8_t *src, uint32_t len);

// scheme integration
void *native_init_scheme(void);
int32_t native_run_pipeline(void *ctx, void *root_group);

// codegen (codegen_c.lain wrappers call these)
void native_emit_module_to_file(void *subs, const char *output_path);
void native_emit_l1_module(const char *output_path);
void *native_get_subroutines(void);

// environment
const char *native_getenv(const char *name);

uint32_t compile_impl(void *ctx, void *root_group, void *output_path);

// command-line arguments
void native_set_args(int argc, char **argv);
int32_t native_get_arg_count(void);
const char *native_get_arg(int32_t idx);

#endif // NATIVE_RUNTIME_H
