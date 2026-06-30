// lain_ast.h — LAIN-AST 拓扑树：5 种形态，零语义
//
// C 极简 Parser 只产出这 5 种拓扑节点。
// 语义解释完全交给 Scheme Meta 层 (canonicalize.scm → lower.scm)。

#ifndef LAIN_AST_H
#define LAIN_AST_H

#include <stdint.h>
#include <stddef.h>

// ── 节点形态 ────────────────────────────────────────────────────────────

typedef enum {
    AST_ATOM,       // 叶子: 标识符、字面量、运算符 token
    AST_INFIX,      // 二元: left op right
    AST_PREFIX,     // 一元前缀: op operand  (e.g. -, *, &)
    AST_POSTFIX,    // 一元后缀: operand op  (e.g. ++, --)
    AST_GROUP,      // 括号: ( expr )
} AstNodeKind;

// ── 不透明句柄 ──────────────────────────────────────────────────────────
// 跨 FFI 传递时永远用 AstNodeId，绝不暴露 AstNode*。

typedef uint32_t AstNodeId;

#define AST_NULL ((AstNodeId)0)

// ── 节点结构 (Arena 内部使用) ───────────────────────────────────────────

typedef struct {
    AstNodeKind kind;
    AstNodeId   left;       // INFIX/PREFIX/POSTFIX/GROUP: 第一个子节点
    AstNodeId   right;      // INFIX: 右子节点; GROUP: 保留
    AstNodeId   op;         // INFIX/PREFIX: 运算符节点; 其余: AST_NULL
    AstNodeId   next;       // GROUP 内: 下一个兄弟节点; 其余: AST_NULL
    uint32_t    line;
    uint32_t    col;
    const char *text;       // ATOM: interned token 文本; 其余: NULL
} AstNode;

// ── Arena 管理器 ────────────────────────────────────────────────────────

#define AST_ARENA_INIT_CAP 1024

typedef struct {
    AstNode *nodes;
    uint32_t count;
    uint32_t capacity;
    // 字符串 intern 表
    const char **strings;
    uint32_t     string_count;
    uint32_t     string_capacity;
} AstArena;

// ── Arena API ───────────────────────────────────────────────────────────

// 初始化 arena (分配在 caller 的栈或堆上)
void ast_arena_init(AstArena *a);

// 释放 arena 内部缓冲区
void ast_arena_destroy(AstArena *a);

// 分配一个新节点，返回句柄
AstNodeId ast_alloc(AstArena *a, AstNodeKind kind,
                     uint32_t line, uint32_t col);

// 设置节点字段
void ast_set_left(AstArena *a, AstNodeId id, AstNodeId left);
void ast_set_right(AstArena *a, AstNodeId id, AstNodeId right);
void ast_set_op(AstArena *a, AstNodeId id, AstNodeId op);
void ast_set_next(AstArena *a, AstNodeId id, AstNodeId next);
void ast_set_text(AstArena *a, AstNodeId id, const char *text);

// 查询节点 (返回内部指针，arena 释放后失效)
const AstNode *ast_get(const AstArena *a, AstNodeId id);

// 字符串 intern: 相同内容返回相同指针
const char *ast_intern(AstArena *a, const char *start, uint32_t len);

// 节点总数
uint32_t ast_count(const AstArena *a);

#endif // LAIN_AST_H
