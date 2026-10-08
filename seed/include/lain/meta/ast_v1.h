/* LAINAST v1：平铺 Arena 的共享布局、引用编码与固定拒码。
 *
 * 这是**唯一**的布局来源：seed 的 reader（(src/meta/tree.c）、宿主（src/meta/host.c）
 * 与验收程序都必须 include 它，禁止各自再写一份结构定义。规范见
 * docs/spec-ast-and-macro-expansion.md 第 1 节。
 *
 * 三条不可动摇的约束：
 *   1. Header 与节点是**映射格式**，不是 C 对象的解引用目标：异字节序主机要逐字段
 *      编解码，禁止 packed 解引用。这里的结构体只是布局描述 + 静态断言。
 *   2. 引用高 16 位是段号、低 48 位是段内字节偏移。段号 0 禁用；引用 0 是 NONE。
 *      编码前检查字段范围，**禁止截断大偏移**。
 *   3. 段身份在 AST 能力边界校验：任何跨段读取都要在所属段内先减后加地验证区间，
 *      再交给 VSpace 检查。引用只在所属编译任务存活期内有效，没有跨任务代际字段。
 */
#ifndef LAINMETA_AST_V1_H
#define LAINMETA_AST_V1_H

#include <stddef.h>
#include <stdint.h>

/* --- 引用编码 -------------------------------------------------------------- */

typedef uint64_t LainAstRef;

#define LAIN_AST_REF_NONE UINT64_C(0)
#define LAIN_AST_OFFSET_MASK UINT64_C(0x0000FFFFFFFFFFFF)
#define LAIN_AST_SEGMENT_SHIFT 48

/* 默认主源码 Source=1、AstIn=2、任务 AstOut=3。源码编号与段号是不同概念：
 * 额外源码按注册顺序依次拿 Source/AstIn 对 4/5、6/7……，编号在任务存活期不回收。
 * 第一期没有 AstOut 消费者，编号 3 仍然保留，不挪作他用。 */
enum {
  LAIN_AST_SOURCE = 1,
  LAIN_AST_IN = 2,
  LAIN_AST_OUT = 3,
};

/* 引用 = 段号 << 48 | 偏移。段号为 0 的引用一律非法（0 是 NONE）。 */
static inline LainAstRef lain_ast_ref(uint16_t segment_id, uint64_t offset) {
  return ((uint64_t)segment_id << LAIN_AST_SEGMENT_SHIFT) | offset;
}

static inline uint16_t lain_ast_segment(LainAstRef ref) {
  return (uint16_t)(ref >> LAIN_AST_SEGMENT_SHIFT);
}

static inline uint64_t lain_ast_offset(LainAstRef ref) {
  return ref & LAIN_AST_OFFSET_MASK;
}

/* --- 节点与 Header 布局 ---------------------------------------------------- */

enum { LAIN_AST_TOKEN = 1, LAIN_AST_GROUP = 2 };

#define LAIN_AST_MAGIC UINT32_C(0x5453414C) /* 小端字节为 LAST */
#define LAIN_AST_ABI_VERSION 1u
#define LAIN_AST_NODE_STRIDE 56u
#define LAIN_AST_HEADER_SIZE 64u

typedef struct {
  uint8_t kind;     /* 1 = Token，2 = Group；其余值非法 */
  uint8_t delimiter;/* Group：0、(、[、{；Token 必须为 0 */
  uint16_t flags;   /* v1 必须为 0 —— 不是保留位，是「不许有语义」 */
  uint32_t child_count;
  LainAstRef text;      /* 文本字节所在引用；空文本为 0 */
  uint64_t text_length; /* 不含 NUL */
  uint64_t children_offset; /* 本 Arena 局部偏移，指向 child_count 个连续引用 */
  LainAstRef origin;        /* 来源节点引用；原始节点为 0 */
  LainAstRef source_span;   /* 原始源码跨度起点；未知为 0 */
  uint64_t source_length;
} LainAstNode;

typedef struct {
  uint32_t magic;
  uint16_t abi_version;
  uint16_t node_stride;
  uint32_t segment_id; /* 必须与绑定段号相等 */
  uint32_t flags;      /* 必须为 0 */
  uint64_t capacity_bytes;
  uint64_t used_bytes; /* 已分配最高结束偏移，含全部预留节点槽 */
  uint64_t nodes_offset;
  uint64_t node_count;
  uint64_t node_capacity;
  LainAstRef root;
} LainAstArenaHeader;

_Static_assert(sizeof(LainAstNode) == 56, "节点必须是 56 字节");
_Static_assert(sizeof(LainAstArenaHeader) == 64, "头必须是 64 字节");

/* 字段偏移断言：规范把偏移当契约写死，改结构体就必须在这里失败。 */
_Static_assert(offsetof(LainAstNode, kind) == 0, "kind 偏移");
_Static_assert(offsetof(LainAstNode, delimiter) == 1, "delimiter 偏移");
_Static_assert(offsetof(LainAstNode, flags) == 2, "flags 偏移");
_Static_assert(offsetof(LainAstNode, child_count) == 4, "child_count 偏移");
_Static_assert(offsetof(LainAstNode, text) == 8, "text 偏移");
_Static_assert(offsetof(LainAstNode, text_length) == 16, "text_length 偏移");
_Static_assert(offsetof(LainAstNode, children_offset) == 24,
               "children_offset 偏移");
_Static_assert(offsetof(LainAstNode, origin) == 32, "origin 偏移");
_Static_assert(offsetof(LainAstNode, source_span) == 40, "source_span 偏移");
_Static_assert(offsetof(LainAstNode, source_length) == 48, "source_length 偏移");

_Static_assert(offsetof(LainAstArenaHeader, magic) == 0, "magic 偏移");
_Static_assert(offsetof(LainAstArenaHeader, abi_version) == 4, "abi_version 偏移");
_Static_assert(offsetof(LainAstArenaHeader, node_stride) == 6, "node_stride 偏移");
_Static_assert(offsetof(LainAstArenaHeader, segment_id) == 8, "segment_id 偏移");
_Static_assert(offsetof(LainAstArenaHeader, flags) == 12, "flags 偏移");
_Static_assert(offsetof(LainAstArenaHeader, capacity_bytes) == 16,
               "capacity_bytes 偏移");
_Static_assert(offsetof(LainAstArenaHeader, used_bytes) == 24, "used_bytes 偏移");
_Static_assert(offsetof(LainAstArenaHeader, nodes_offset) == 32,
               "nodes_offset 偏移");
_Static_assert(offsetof(LainAstArenaHeader, node_count) == 40, "node_count 偏移");
_Static_assert(offsetof(LainAstArenaHeader, node_capacity) == 48,
               "node_capacity 偏移");
_Static_assert(offsetof(LainAstArenaHeader, root) == 56, "root 偏移");

/* --- 固定拒码（docs/spec-ast-and-macro-expansion.md 3.7） ------------------ */

enum {
  LAIN_AST_ERR_SEGMENT = 9401,  /* 未知/无效段号或任务绑定 */
  LAIN_AST_ERR_HEADER = 9402,   /* Header/版本/布局非法 */
  LAIN_AST_ERR_SLOT = 9403,     /* 节点引用错位、非节点槽或未发布 */
  LAIN_AST_ERR_RANGE = 9404,    /* 字节跨度/列表范围或尺寸溢出 */
  LAIN_AST_ERR_GRAPH = 9405,    /* AST/来源图非法或有环 */
  LAIN_AST_ERR_CAPACITY = 9406, /* AstOut 容量/节点容量不足 */
  LAIN_AST_ERR_TRANSACTION = 9407,
  LAIN_AST_ERR_PROTOCOL = 9408,
  LAIN_AST_ERR_SHAPE = 9409,
  LAIN_AST_ERR_BUDGET = 9410,
  LAIN_AST_ERR_STAGE = 9411,
  LAIN_AST_ERR_BINDING = 9412,
  LAIN_AST_ERR_PUBLISHED = 9413,
};

/* --- 宿主视图（不进入映射格式） -------------------------------------------- */

/* 宿主侧的 Arena 句柄：data/capacity 是真实分配，used 是已发布水位。
 * 映射只映射 [data, used)，**不拥有**存储、不重复扣账。 */
typedef struct {
  const void *data;
  uint64_t capacity;
  uint64_t used;
  uint16_t segment_id;
  LainAstRef root;
} LainAstArenaView;

/* 节点引用 → 段内偏移。调用方必须已经确认这是**本段**的节点引用。 */
static inline uint64_t lain_ast_node_offset(const LainAstArenaHeader *header,
                                            uint64_t index) {
  return header->nodes_offset + index * (uint64_t)LAIN_AST_NODE_STRIDE;
}

#endif /* LAINMETA_AST_V1_H */
