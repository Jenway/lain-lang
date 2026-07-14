#include "native_runtime.h"
#include "lainir_exec.h"
#include "lainast/lain_ast.h"
#include <stdint.h>
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
    if (len < 0) {
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
    
    int emit_l1 = 0;
    int emit_interface = 0;
    int emit_ast = 0;
    int interpret = 0;
    const char *input_path = NULL;
    const char *output_path = NULL;
    
    if (argc >= 4 && strcmp(argv[1], "--interpret") == 0) {
        interpret = 1;
        input_path = argv[2];
        output_path = argv[3];
    } else if (argc >= 4 && strcmp(argv[1], "--emit-l1") == 0) {
        emit_l1 = 1;
        input_path = argv[2];
        output_path = argv[3];
    } else if (argc >= 4 && strcmp(argv[1], "--emit-interface") == 0) {
        emit_interface = 1;
        input_path = argv[2];
        output_path = argv[3];
    } else if (argc >= 4 && strcmp(argv[1], "--emit-ast") == 0) {
        emit_ast = 1;
        input_path = argv[2];
        output_path = argv[3];
    } else if (argc >= 3) {
        input_path = argv[1];
        output_path = argv[2];
    } else {
        printf("Usage: %s [--interpret|--emit-ast|--emit-l1|--emit-interface] <input.lain> <output-or-entry>\n", argv[0]);
        return 1;
    }
    
    if (interpret) return interpret_lain(input_path, output_path);
    if (emit_ast) return compile_ast(input_path, output_path);
    if (emit_l1) return compile_l1(input_path, output_path);
    if (emit_interface) return compile_interface(input_path, output_path);
    return compile(input_path, output_path);
}
