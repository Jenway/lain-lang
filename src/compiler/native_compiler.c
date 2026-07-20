#include "native_runtime.h"
#include "builder_ffi.h"
#include "structured_unit.h"
#include "version.h"
#include "lainir_exec.h"
#include "lainast/lain_ast.h"
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ast_indent(FILE *out, int n) {
    for (int i = 0; i < n; i++) fputc(' ', out);
}

static const char *ast_node_text(const AstArena *arena, AstNodeId id) {
    const AstNode *node = ast_get(arena, id);
    return node && node->text ? node->text : "";
}

static const char *ast_group_name(const AstArena *arena, const AstNode *node) {
    const char *op = ast_node_text(arena, node->op);
    if (strcmp(op, "(") == 0) return "paren";
    if (strcmp(op, "{") == 0) return "brace";
    if (strcmp(op, "[") == 0) return "bracket";
    return "root";
}

static void ast_print_escaped(FILE *out, const char *text) {
    fputc('"', out);
    for (const char *p = text; p && *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', out);
            fputc(*p, out);
        } else if (*p == '\n') {
            fputs("\\n", out);
        } else if (*p == '\r') {
            fputs("\\r", out);
        } else if (*p == '\t') {
            fputs("\\t", out);
        } else {
            fputc(*p, out);
        }
    }
    fputc('"', out);
}

static void ast_dump_node(FILE *out, const AstArena *arena, AstNodeId id, int depth);

static void ast_dump_children(FILE *out, const AstArena *arena, AstNodeId first, int depth) {
    AstNodeId curr = first;
    while (curr != AST_NULL) {
        const AstNode *node = ast_get(arena, curr);
        ast_dump_node(out, arena, curr, depth);
        curr = node ? node->next : AST_NULL;
    }
}

static void ast_dump_node(FILE *out, const AstArena *arena, AstNodeId id, int depth) {
    const AstNode *node = ast_get(arena, id);
    if (!node) return;

    switch (node->kind) {
    case AST_ATOM:
        ast_indent(out, depth);
        fputs("(atom ", out);
        ast_print_escaped(out, node->text ? node->text : "");
        fputs(")\n", out);
        break;
    case AST_GROUP:
        ast_indent(out, depth);
        fprintf(out, "(group %s\n", ast_group_name(arena, node));
        ast_dump_children(out, arena, node->left, depth + 2);
        ast_indent(out, depth);
        fputs(")\n", out);
        break;
    case AST_PREFIX:
        ast_indent(out, depth);
        fputs("(prefix ", out);
        ast_print_escaped(out, ast_node_text(arena, node->op));
        fputc('\n', out);
        ast_dump_node(out, arena, node->left, depth + 2);
        ast_indent(out, depth);
        fputs(")\n", out);
        break;
    case AST_POSTFIX:
        ast_indent(out, depth);
        fputs("(postfix ", out);
        ast_print_escaped(out, ast_node_text(arena, node->op));
        fputc('\n', out);
        ast_dump_node(out, arena, node->left, depth + 2);
        if (node->right != AST_NULL) ast_dump_node(out, arena, node->right, depth + 2);
        ast_indent(out, depth);
        fputs(")\n", out);
        break;
    case AST_INFIX: {
        const char *op = ast_node_text(arena, node->op);
        ast_indent(out, depth);
        if (strcmp(op, " ") == 0) {
            fputs("(juxt\n", out);
        } else {
            fputs("(infix ", out);
            ast_print_escaped(out, op);
            fputc('\n', out);
        }
        ast_dump_node(out, arena, node->left, depth + 2);
        ast_dump_node(out, arena, node->right, depth + 2);
        ast_indent(out, depth);
        fputs(")\n", out);
        break;
    }
    }
}

static char *read_source_file(const char *path, uint32_t *len_out) {
    FILE *file = fopen(path, "rb");
    char *buffer;
    long len;

    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    len = ftell(file);
    if (len < 0 || (unsigned long)len > UINT32_MAX) {
        fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    buffer = (char *)malloc((size_t)len + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    if (fread(buffer, 1, (size_t)len, file) != (size_t)len) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    fclose(file);
    buffer[len] = '\0';
    *len_out = (uint32_t)len;
    return buffer;
}

static int file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

static char *artifact_next_to_executable(const char *argv0) {
    const char *slash = strrchr(argv0 ? argv0 : "", '/');
    const char *backslash = strrchr(argv0 ? argv0 : "", '\\');
    const char *separator = slash;
    const char *name = "stage2_compiler.l1";
    size_t directory_len;
    char *path;
    if (!separator || (backslash && backslash > separator)) separator = backslash;
    if (!separator) return NULL;
    directory_len = (size_t)(separator - argv0 + 1);
    path = malloc(directory_len + strlen(name) + 1);
    if (!path) return NULL;
    memcpy(path, argv0, directory_len);
    strcpy(path + directory_len, name);
    return path;
}

static char *find_default_compiler_artifact(const char *argv0) {
    const char *from_env = getenv("LAIN_COMPILER_ARTIFACT");
    const char *candidates[] = {
        "build/core-self-hosting/stage2_compiler.l1",
        "zig-out/bin/stage2_compiler.l1",
        "stage2_compiler.l1",
        NULL,
    };
    char *beside;
    if (from_env && from_env[0] && file_exists(from_env))
        return strdup(from_env);
    beside = artifact_next_to_executable(argv0);
    if (beside && file_exists(beside)) return beside;
    free(beside);
    for (size_t i = 0; candidates[i]; i++)
        if (file_exists(candidates[i])) return strdup(candidates[i]);
    return NULL;
}

static int write_text_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    size_t length;
    size_t written;
    if (!file) {
        fprintf(stderr, "failed to open %s\n", path);
        return 0;
    }
    length = strlen(text ? text : "");
    written = fwrite(text, 1, length, file);
    if (fclose(file) != 0 || written != length) {
        fprintf(stderr, "failed to write %s\n", path);
        return 0;
    }
    return 1;
}

static int artifact_call_values(
    void *ctx, const char *artifact, const char *entry,
    LainirValue *args, uint32_t arg_count, LainirValue *result) {
    L1Diagnostic diagnostic;
    LainirExecStatus status = native_execute_compiler_artifact(
        ctx, artifact, entry, args, arg_count, result, &diagnostic);
    if (status == LAINIR_EXEC_OK) return 1;
    fprintf(stderr, "[compiler artifact error %d] %s\n",
            diagnostic.code,
            diagnostic.message[0] ? diagnostic.message :
                                    "artifact execution failed");
    return 0;
}

static int artifact_call_handle(
    void *ctx, const char *artifact, const char *entry,
    uint32_t handle, LainirValue *result) {
    LainirValue arg = lainir_value_bits(handle, 32);
    return artifact_call_values(ctx, artifact, entry, &arg, 1, result);
}

static uint32_t run_artifact_entry(
    const char *artifact_path, const char *entry) {
    uint32_t artifact_length = 0;
    char *artifact = read_source_file(artifact_path, &artifact_length);
    void *ctx = NULL;
    LainirValue result = lainir_value_unit();
    uint32_t status = 1;
    (void)artifact_length;
    if (!artifact) {
        fprintf(stderr, "failed to read compiler artifact %s\n", artifact_path);
        return 1;
    }
    ctx = native_init_compiler_artifact_host();
    if (!ctx) {
        fprintf(stderr, "failed to initialize compiler artifact host\n");
        free(artifact);
        return 1;
    }
    if (!artifact_call_values(ctx, artifact, entry, NULL, 0, &result))
        goto cleanup;
    if (result.kind == LAINIR_VALUE_BITS) {
        printf("%lld\n", (long long)result.as.bits);
        status = 0;
        goto cleanup;
    }
    if (result.kind == LAINIR_VALUE_STRING) {
        printf("%s\n", result.as.string ? result.as.string : "");
        status = 0;
        goto cleanup;
    }
    fprintf(stderr, "compiler artifact entry %s returned no printable value\n",
        entry);
cleanup:
    if (result.kind == LAINIR_VALUE_STRING) free((void *)result.as.string);
    free(artifact);
    return status;
}

static int artifact_result_i32(
    void *ctx, const char *artifact, const char *entry,
    uint32_t result_handle, int32_t *value_out) {
    LainirValue value = lainir_value_unit();
    if (!artifact_call_handle(ctx, artifact, entry, result_handle, &value))
        return 0;
    if (value.kind != LAINIR_VALUE_BITS) {
        if (value.kind == LAINIR_VALUE_STRING) free((void *)value.as.string);
        fprintf(stderr, "compiler result projection %s did not return i32\n", entry);
        return 0;
    }
    *value_out = (int32_t)value.as.bits;
    return 1;
}

static uint32_t compile_inputs_with_artifact(
    const char *artifact_path, const char *output_path, int mode,
    int input_count, char **input_paths) {
    uint32_t artifact_length = 0;
    char *artifact = read_source_file(artifact_path, &artifact_length);
    uint32_t paths = 0;
    uint32_t sources = 0;
    uint32_t request = 0;
    uint32_t result_handle = 0;
    uint32_t status = 1;
    void *ctx = NULL;
    LainirValue compiled = lainir_value_unit();
    LainirValue result = lainir_value_unit();
    int32_t outcome = 0;
    uint32_t ast_stores_before = native_ast_store_live_count();
    (void)artifact_length;
    if (!artifact) {
        fprintf(stderr, "failed to read compiler artifact %s\n", artifact_path);
        return 1;
    }
    ctx = native_init_compiler_artifact_host();
    if (!ctx) {
        fprintf(stderr, "failed to initialize compiler artifact host\n");
        free(artifact);
        return 1;
    }
    paths = structured_compiler_storage_new(2);
    sources = structured_compiler_storage_new(2);
    request = structured_compiler_storage_new(1);
    if (!paths || !sources || !request ||
        !structured_compiler_storage_reserve(paths, (uint32_t)input_count) ||
        !structured_compiler_storage_reserve(sources, (uint32_t)input_count) ||
        !structured_compiler_storage_reserve(request, 4)) {
        fprintf(stderr, "failed to allocate structured compiler request\n");
        goto cleanup;
    }
    for (int i = 0; i < input_count; ++i) {
        uint32_t source_length = 0;
        char *source = read_source_file(input_paths[i], &source_length);
        (void)source_length;
        if (!source) {
            fprintf(stderr, "failed to read %s\n", input_paths[i]);
            goto cleanup;
        }
        if (!structured_compiler_storage_set_string(
                paths, (uint32_t)i, input_paths[i]) ||
            !structured_compiler_storage_set_string(
                sources, (uint32_t)i, source)) {
            fprintf(stderr, "failed to store workspace input %s\n", input_paths[i]);
            free(source);
            goto cleanup;
        }
        free(source);
    }
    if (!structured_compiler_storage_set_i32(request, 0, mode) ||
        !structured_compiler_storage_set_i32(request, 1, (int32_t)paths) ||
        !structured_compiler_storage_set_i32(request, 2, (int32_t)sources) ||
        !structured_compiler_storage_set_i32(request, 3, input_count)) {
        fprintf(stderr, "failed to initialize structured compiler request\n");
        goto cleanup;
    }
    if (!artifact_call_handle(ctx, artifact, "compiler_compile", request, &compiled))
        goto cleanup;
    if (native_ast_store_live_count() != ast_stores_before) {
        fprintf(stderr, "compiler_compile leaked syntax store ownership\n");
        goto cleanup;
    }
    if (compiled.kind != LAINIR_VALUE_BITS || compiled.as.bits == 0) {
        fprintf(stderr, "compiler_compile did not return a result handle\n");
        goto cleanup;
    }
    result_handle = (uint32_t)compiled.as.bits;
    if (!artifact_result_i32(
            ctx, artifact, "compiler_result_outcome", result_handle, &outcome))
        goto cleanup;
    if (outcome == 0) {
        LainirValue message = lainir_value_unit();
        LainirValue path = lainir_value_unit();
        int32_t code_value = 1;
        int32_t start = 0;
        int32_t end = 0;
        (void)artifact_result_i32(ctx, artifact,
            "compiler_result_diagnostic_code", result_handle, &code_value);
        (void)artifact_result_i32(ctx, artifact,
            "compiler_result_diagnostic_start", result_handle, &start);
        (void)artifact_result_i32(ctx, artifact,
            "compiler_result_diagnostic_end", result_handle, &end);
        (void)artifact_call_handle(ctx, artifact,
            "compiler_result_diagnostic_message", result_handle, &message);
        (void)artifact_call_handle(ctx, artifact,
            "compiler_result_diagnostic_path", result_handle, &path);
        fprintf(stderr, "Lain compiler error %d%s%s at %d..%d: %s\n",
                code_value,
                path.kind == LAINIR_VALUE_STRING && path.as.string &&
                        path.as.string[0] ? " in " : "",
                path.kind == LAINIR_VALUE_STRING && path.as.string
                    ? path.as.string : "",
                start, end,
                message.kind == LAINIR_VALUE_STRING && message.as.string
                    ? message.as.string : "");
        if (message.kind == LAINIR_VALUE_STRING) free((void *)message.as.string);
        if (path.kind == LAINIR_VALUE_STRING) free((void *)path.as.string);
        goto cleanup;
    }
    if (!artifact_call_handle(
            ctx, artifact, "compiler_result_l1_text", result_handle, &result))
        goto cleanup;
    if (result.kind != LAINIR_VALUE_STRING || !result.as.string ||
        result.as.string[0] == '\0') {
        fprintf(stderr, "successful compiler result contains no L1 text\n");
        goto cleanup;
    }
    if (!write_text_file(output_path, result.as.string)) goto cleanup;
    status = 0;

cleanup:
    if (result.kind == LAINIR_VALUE_STRING) free((void *)result.as.string);
    if (result_handle) {
        LainirValue destroyed = lainir_value_unit();
        (void)artifact_call_handle(
            ctx, artifact, "compiler_result_destroy", result_handle, &destroyed);
        if (destroyed.kind == LAINIR_VALUE_STRING)
            free((void *)destroyed.as.string);
    }
    if (request) (void)structured_compiler_storage_destroy(request);
    if (paths) (void)structured_compiler_storage_destroy(paths);
    if (sources) (void)structured_compiler_storage_destroy(sources);
    free(artifact);
    return status;
}

static uint32_t compile_file_with_artifact(
    const char *artifact_path, const char *input_path,
    const char *output_path) {
    char *inputs[1] = {(char *)input_path};
    return compile_inputs_with_artifact(
        artifact_path, output_path, 1, 1, inputs);
}

static uint32_t compile_workspace_with_artifact(
    const char *artifact_path, const char *output_path,
    int input_count, char **input_paths) {
    return compile_inputs_with_artifact(
        artifact_path, output_path, 2, input_count, input_paths);
}

static int32_t run_pipeline_for(const void* input_path) {
    native_set_interpret_source_linking(0);
    native_set_source_path(input_path);
    const uint8_t* src = (const uint8_t*)native_read_file(input_path);
    uint32_t len = native_file_len();
    void* root_group = native_lex_and_group(src, len);
    void* ctx = native_init_scheme();
    return native_run_pipeline(ctx, root_group);
}

uint32_t compile(const void* arg0, const void* arg1) {
    int32_t result = run_pipeline_for(arg0);
    if (result != 0) return result;
    void* subs = native_get_subroutines();
    native_emit_module_to_file(subs, arg1);
    return 0;
}

uint32_t compile_l1(const void* arg0, const void* arg1) {
    int32_t result = run_pipeline_for(arg0);
    if (result != 0) return result;
    native_emit_l1_module(arg1);
    return 0;
}

uint32_t compile_interface(const void* arg0, const void* arg1) {
    int32_t result = run_pipeline_for(arg0);
    if (result != 0) return result;
    native_emit_interface(arg1);
    return 0;
}

static uint32_t interpret_lain(const char *input_path, const char *entry_name) {
    native_set_interpret_source_linking(1);
    native_set_source_path(input_path);
    const uint8_t *src = (const uint8_t *)native_read_file(input_path);
    uint32_t len = native_file_len();
    void *root_group = native_lex_and_group(src, len);
    vm_context *ctx = (vm_context *)native_init_scheme();
    vm_value *result = vm_false();
    LainirExecRequest request;
    if (native_run_pipeline(ctx, root_group) != 0) return 1;
    request.entry_name = entry_name;
    request.args = vm_null();
    if (lainir_exec_request(ctx, vm_context_env(ctx), &request, &result) !=
        LAINIR_EXEC_OK) {
        fprintf(stderr, "[interpret ERROR] ");
        vm_print_exception(ctx, result);
        return 1;
    }
    if (vm_is_integer(result) || vm_is_fixnum(result)) {
        int64_t value = vm_is_fixnum(result) ? vm_fixnum_value(result)
                                             : (int64_t)vm_uint_value(result);
        printf("%lld\n", (long long)value);
        return 0;
    }
    if (result == vm_void() || result == vm_null()) {
        puts("unit");
        return 0;
    }
    fprintf(stderr, "interpreted entry returned an unsupported value\n");
    return 1;
}

uint32_t compile_ast(const void* arg0, const void* arg1) {
    const char *input_path = (const char *)arg0;
    const char *output_path = (const char *)arg1;
    uint32_t len = 0;
    char *source = read_source_file(input_path, &len);
    FILE *out;
    AstArena arena;
    AstNodeId root;

    if (!source) {
        fprintf(stderr, "failed to read %s\n", input_path);
        return 1;
    }

    out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "failed to open %s\n", output_path);
        free(source);
        return 1;
    }

    ast_arena_init(&arena);
    root = ast_parse(&arena, source, len);
    ast_dump_node(out, &arena, root, 0);
    ast_arena_destroy(&arena);
    fclose(out);
    free(source);
    return 0;
}

int main(int argc, char **argv) {
    native_set_args(argc, argv);

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf(
            "lainc %s\n"
            "LAIN-IR schema %d\n"
            "Compiler API schema %d\n"
            "ModuleArtifact schema %d\n",
            LAIN_VERSION,
            LAIN_IR_SCHEMA_VERSION,
            LAIN_COMPILER_API_SCHEMA_VERSION,
            LAIN_MODULE_ARTIFACT_SCHEMA_VERSION);
        return 0;
    }

    int emit_l1 = 0;
    int bootstrap_emit_l1 = 0;
    int emit_workspace_l1 = 0;
    int emit_interface = 0;
    int emit_ast = 0;
    int interpret = 0;
    int artifact_run = 0;
    int arg_index = 1;
    const char *artifact_path = NULL;
    char *default_artifact = NULL;
    const char *input_path = NULL;
    const char *output_path = NULL;

    if (argc >= 3 && strcmp(argv[arg_index], "--artifact") == 0) {
        artifact_path = argv[arg_index + 1];
        arg_index += 2;
    }

    if (argc - arg_index >= 2 &&
               strcmp(argv[arg_index], "--artifact-run") == 0) {
        artifact_run = 1;
        input_path = argv[arg_index + 1];
    } else if (argc - arg_index >= 3 && strcmp(argv[arg_index], "--interpret") == 0) {
        interpret = 1;
        input_path = argv[arg_index + 1];
        output_path = argv[arg_index + 2];
    } else if (argc - arg_index >= 3 &&
               strcmp(argv[arg_index], "--emit-l1") == 0) {
        emit_l1 = 1;
        input_path = argv[arg_index + 1];
        output_path = argv[arg_index + 2];
    } else if (argc - arg_index >= 3 &&
               strcmp(argv[arg_index], "--bootstrap-emit-l1") == 0) {
        bootstrap_emit_l1 = 1;
        input_path = argv[arg_index + 1];
        output_path = argv[arg_index + 2];
    } else if (argc - arg_index >= 3 &&
               strcmp(argv[arg_index], "--emit-interface") == 0) {
        emit_interface = 1;
        input_path = argv[arg_index + 1];
        output_path = argv[arg_index + 2];
    } else if (argc - arg_index >= 3 &&
               strcmp(argv[arg_index], "--emit-ast") == 0) {
        emit_ast = 1;
        input_path = argv[arg_index + 1];
        output_path = argv[arg_index + 2];
    } else if (argc - arg_index >= 3 &&
               strcmp(argv[arg_index], "--emit-workspace-l1") == 0) {
        emit_workspace_l1 = 1;
        output_path = argv[arg_index + 1];
        arg_index += 2;
    } else if (argc - arg_index >= 2) {
        input_path = argv[arg_index];
        output_path = argv[arg_index + 1];
    } else {
        printf(
            "Usage:\n"
            "  %s --version\n"
            "  %s --emit-l1 <input.lain> <output.l1>\n"
            "  %s --emit-workspace-l1 <output.l1> <module.lain>...\n"
            "  %s --artifact <compiler.l1> --emit-l1 <input.lain> <output.l1>\n"
            "  %s --artifact <compiler.l1> --artifact-run <entry>\n"
            "  %s --bootstrap-emit-l1 <input.lain> <output.l1>\n"
            "  %s [--interpret|--emit-ast|--emit-interface] <input> <output-or-entry>\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    if (artifact_run) {
        if (!artifact_path) {
            fprintf(stderr, "--artifact-run requires --artifact <compiler.l1>\n");
            return 1;
        }
        return run_artifact_entry(artifact_path, input_path);
    }
    if (interpret) return interpret_lain(input_path, output_path);
    if (emit_ast) return compile_ast(input_path, output_path);
    if (bootstrap_emit_l1) return compile_l1(input_path, output_path);
    if (emit_l1 || emit_workspace_l1) {
        if (!artifact_path) {
            default_artifact = find_default_compiler_artifact(argv[0]);
            artifact_path = default_artifact;
        }
        if (!artifact_path) {
            fprintf(stderr,
                "self-hosted compiler artifact not found; run `zig build` "
                "or set LAIN_COMPILER_ARTIFACT\n");
            return 1;
        }
        if (emit_l1) {
            uint32_t result = compile_file_with_artifact(
                artifact_path, input_path, output_path);
            free(default_artifact);
            return result;
        }
        if (arg_index >= argc) {
            fprintf(stderr, "--emit-workspace-l1 requires at least one input\n");
            free(default_artifact);
            return 1;
        }
        uint32_t result = compile_workspace_with_artifact(
            artifact_path, output_path, argc - arg_index, argv + arg_index);
        free(default_artifact);
        return result;
    }
    if (emit_interface) return compile_interface(input_path, output_path);
    return compile(input_path, output_path);
}
