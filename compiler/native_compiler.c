#include "native_runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <alloca.h>

uint32_t main(uint32_t argc, char **argv) {
block_0:
    native_set_args(argc, argv);
    if (native_get_arg_count() < 3) {
        // Usage: lainc <input.lain> <output.c>
        return 1;
    }
    return compile(native_get_arg(1), native_get_arg(2));
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
