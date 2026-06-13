#include "native_runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <alloca.h>

// Forward-declare compile_l1 (in this file)
uint32_t compile_l1(const void* input_path, const void* output_path);

uint32_t main(uint32_t argc, char **argv) {
    native_set_args(argc, argv);
    
    // Parse --emit-l1 flag
    int emit_l1 = 0;
    const char *input_path = NULL;
    const char *output_path = NULL;
    
    if (argc >= 4 && strcmp(argv[1], "--emit-l1") == 0) {
        emit_l1 = 1;
        input_path = argv[2];
        output_path = argv[3];
    } else if (argc >= 3) {
        input_path = argv[1];
        output_path = argv[2];
    } else {
        printf("Usage: %s [--emit-l1] <input.lain> <output>\n", argv[0]);
        return 1;
    }
    
    if (emit_l1) {
        return compile_l1(input_path, output_path);
    }
    return compile(input_path, output_path);
}

uint32_t compile(const void* arg0, const void* arg1) {
block_0:
    // Read source (returns pointer, sets internal length)
    const uint8_t* src = (const uint8_t*)native_read_file(arg0);
    // Get file length (must be called after native_read_file)
    uint32_t len = native_file_len();
    // Lex and group tokens
    void* root_group = native_lex_and_group(src, len);
    // Initialize Scheme (only once!)
    void* ctx = native_init_scheme();
    // Run the Scheme pipeline
    native_run_pipeline(ctx, root_group);
    // Get subroutines and emit to file
    void* subs = native_get_subroutines();
    native_emit_module_to_file(subs, arg1);
    return 0;
}

uint32_t compile_l1(const void* arg0, const void* arg1) {
block_0:
    // Read source (returns pointer, sets internal length)
    const uint8_t* src = (const uint8_t*)native_read_file(arg0);
    // Get file length (must be called after native_read_file)
    uint32_t len = native_file_len();
    // Lex and group tokens
    void* root_group = native_lex_and_group(src, len);
    // Initialize Scheme (only once!)
    void* ctx = native_init_scheme();
    // Run the Scheme pipeline
    native_run_pipeline(ctx, root_group);
    // Emit L1 IR instead of C code
    native_emit_l1_module(arg1);
    return 0;
}
