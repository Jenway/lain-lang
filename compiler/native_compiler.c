#include "native_runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <alloca.h>

static void run_pipeline_for(const void* input_path) {
    native_set_source_path(input_path);
    const uint8_t* src = (const uint8_t*)native_read_file(input_path);
    uint32_t len = native_file_len();
    void* root_group = native_lex_and_group(src, len);
    void* ctx = native_init_scheme();
    native_run_pipeline(ctx, root_group);
}

uint32_t compile(const void* arg0, const void* arg1) {
    run_pipeline_for(arg0);
    void* subs = native_get_subroutines();
    native_emit_module_to_file(subs, arg1);
    return 0;
}

uint32_t compile_l1(const void* arg0, const void* arg1) {
    run_pipeline_for(arg0);
    native_emit_l1_module(arg1);
    return 0;
}

uint32_t compile_manifest(const void* arg0, const void* arg1) {
    run_pipeline_for(arg0);
    native_emit_manifest(arg1);
    return 0;
}

uint32_t main(uint32_t argc, char **argv) {
    native_set_args(argc, argv);
    
    int emit_l1 = 0;
    int emit_manifest = 0;
    int build_mode = 0;
    const char *input_path = NULL;
    const char *output_path = NULL;
    
    if (argc >= 4 && strcmp(argv[1], "--build") == 0) {
        build_mode = 1;
        input_path = argv[2];
        output_path = argv[3];
    } else if (argc >= 4 && strcmp(argv[1], "--emit-l1") == 0) {
        emit_l1 = 1;
        input_path = argv[2];
        output_path = argv[3];
    } else if (argc >= 4 && strcmp(argv[1], "--emit-manifest") == 0) {
        emit_manifest = 1;
        input_path = argv[2];
        output_path = argv[3];
    } else if (argc >= 3) {
        input_path = argv[1];
        output_path = argv[2];
    } else {
        printf("Usage: %s [--emit-l1|--emit-manifest] <input.lain> <output>\n", argv[0]);
        return 1;
    }
    
    if (build_mode) return native_build_with_funcs(input_path, output_path, compile, compile_manifest);
    if (emit_l1) return compile_l1(input_path, output_path);
    if (emit_manifest) return compile_manifest(input_path, output_path);
    return compile(input_path, output_path);
}
