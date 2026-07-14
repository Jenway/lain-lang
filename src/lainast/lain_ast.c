// lain_ast.c — LAIN-AST Arena 实现

#include "lain_ast.h"
#include <stdlib.h>
#include <string.h>

void ast_arena_init(AstArena *a) {
    a->capacity = AST_ARENA_INIT_CAP;
    a->count = 1; // 0 保留为 AST_NULL
    a->nodes = (AstNode *)calloc(a->capacity, sizeof(AstNode));

    a->string_capacity = 256;
    a->string_count = 0;
    a->strings = (const char **)calloc(a->string_capacity, sizeof(const char *));
}

void ast_arena_destroy(AstArena *a) {
    // 释放 intern 的字符串
    for (uint32_t i = 0; i < a->string_count; i++)
        free((void *)a->strings[i]);
    free(a->strings);
    free(a->nodes);
    a->nodes = NULL;
    a->count = 0;
    a->capacity = 0;
}

static void ensure_capacity(AstArena *a, uint32_t needed) {
    if (needed < a->capacity) return;
    uint32_t new_cap = a->capacity * 2;
    while (new_cap < needed) new_cap *= 2;
    a->nodes = (AstNode *)realloc(a->nodes, new_cap * sizeof(AstNode));
    memset(a->nodes + a->capacity, 0, (new_cap - a->capacity) * sizeof(AstNode));
    a->capacity = new_cap;
}

AstNodeId ast_alloc(AstArena *a, AstNodeKind kind,
                     uint32_t line, uint32_t col) {
    ensure_capacity(a, a->count + 1);
    AstNodeId id = a->count++;
    AstNode *n = &a->nodes[id];
    memset(n, 0, sizeof(AstNode));
    n->kind = kind;
    n->line = line;
    n->col = col;
    return id;
}

void ast_set_left(AstArena *a, AstNodeId id, AstNodeId left) {
    a->nodes[id].left = left;
}

void ast_set_right(AstArena *a, AstNodeId id, AstNodeId right) {
    a->nodes[id].right = right;
}

void ast_set_op(AstArena *a, AstNodeId id, AstNodeId op) {
    a->nodes[id].op = op;
}

void ast_set_next(AstArena *a, AstNodeId id, AstNodeId next) {
    a->nodes[id].next = next;
}

void ast_set_text(AstArena *a, AstNodeId id, const char *text) {
    a->nodes[id].text = text;
}

const AstNode *ast_get(const AstArena *a, AstNodeId id) {
    if (id == AST_NULL || id >= a->count) return NULL;
    return &a->nodes[id];
}

const char *ast_intern(AstArena *a, const char *start, uint32_t len) {
    // 线性扫描已有字符串 (小规模 AST 足够快)
    for (uint32_t i = 0; i < a->string_count; i++) {
        if (strncmp(a->strings[i], start, len) == 0 &&
            a->strings[i][len] == '\0')
            return a->strings[i];
    }
    // 新字符串: 复制并追加 NUL
    if (a->string_count >= a->string_capacity) {
        a->string_capacity *= 2;
        a->strings = (const char **)realloc(
            a->strings, a->string_capacity * sizeof(const char *));
    }
    char *copy = (char *)malloc(len + 1);
    memcpy(copy, start, len);
    copy[len] = '\0';
    a->strings[a->string_count++] = copy;
    return copy;
}

uint32_t ast_count(const AstArena *a) {
    // Slot 0 is the permanent AST_NULL sentinel, not a syntax node.  Keep
    // the public count aligned with the observable ast.node-* domain.
    return a->count > 0 ? a->count - 1 : 0;
}
