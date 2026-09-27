/* lainmeta/tree.h 的实现：两遍扫描生成平铺 AstIn Arena。
 *
 * 与旧实现的区别只有一个，但是根本性的：节点不再各自 malloc、孩子不再各自一块，
 * 而是**一整块** `64 + 56*N + 8*E` 字节。第一遍数出 N 和 E 并检查语法，第二遍
 * 原地填。把这块内存映射给 Meta 读就是零拷贝——不需要再把树复制成别的形状。
 *
 * 扫描规则（token_end / closing / 注释 / 字符串 / 深度限制）与旧实现逐字一致：
 * 迁移不许顺手改语言。
 *
 * 孩子列表怎么攒：解析期只需要**一份**栈式暂存——所有当前打开的组的孩子引用连成
 * 一排，每个组记自己的起点。这是规范允许的解析临时栈，也是唯一一块不在最终
 * Arena 里的临时存储（Arena 恰好 `64 + 56*N + 8*E` 字节，没有空地给暂存）。
 * 组闭合时把这一段搬到追加区（此后只读），暂存随即退掉。所以解析期不需要任何
 * "每个组一份 malloc 孩子数组"——这是与旧实现的关键差别。
 *
 * 槽位分配（这是本文件最容易写错的地方，三条规矩一起看）：
 *   1. 根恒占 0 号槽。它覆盖全文、最后才填，所以把 0 号槽留给它，其余节点从 1
 *      号起发号。0 号槽不可能是任何节点的孩子，所以"孩子必须指向已发布的槽"
 *      这条不变式照旧成立。
 *   2. **取号只发生在 parse_group 里，两遍都走**。计数那一遍没有 fill_token /
 *      fill_group，若不显式推号，两遍的槽位序列就会错开。fill_* 只接收已经取好
 *      的槽位号，自己绝不再取号。
 *   3. 非根组的槽位是 1..G，词的槽位紧随其后 G+1..G+L。
 */
#include "lainmeta/tree.h"

#include <stdlib.h>
#include <string.h>

/* 根的槽位恒为 0。 */
#define LAIN_TREE_ROOT_SLOT 0u

struct LainMetaTree {
  const char *source;
  uint32_t source_length;
  uint32_t source_index;
  unsigned char *arena;
  uint64_t arena_bytes;
  uint16_t segment_id;
  uint16_t text_segment; /* 令牌文本所在的段（Source），与本 Arena 的段号不同 */
  LainAstRef root;
  LainVmQuota *quota;
};

/* 第一遍的产物。第二遍按同样顺序填，两遍的编号必须一样。 */
typedef struct {
  uint64_t node_total;  /* N：含根 */
  uint64_t edge_total;  /* E：全部组的孩子引用总数 */
  uint64_t group_total; /* G：组数（含根） */
  uint64_t leaf_total;  /* L：词数 */
} LainMetaScan;

typedef struct {
  LainMetaTree *tree;
  unsigned char *arena; /* NULL = 第一遍（只数） */
  uint64_t capacity;
  uint64_t nodes_start; /* = LAIN_AST_HEADER_SIZE，8 字节对齐 */
  uint64_t nodes_bytes; /* 节点预留区长度 = 56*N */
  uint64_t append_at;   /* 追加区发布水位 */
  uint64_t next_group;  /* 下一个非根组的槽位 */
  uint64_t next_leaf;   /* 下一个词的槽位 */
  uint64_t node_at;     /* 已发布节点数（第二遍） */
  uint64_t slot;        /* 当前正在构建的节点的槽位 */
  /* 第二遍里非语法类失败的类别（临时栈扩容失败、填充自检失败）。`parse_group`
   * 只会回 true/false，类别靠这一格带出去；第一遍不建栈，所以恒为 OK。 */
  LainMetaTreeStatus fail;
  LainAstRef last;      /* 最后一个已发布节点的引用 */
  LainMetaScan *scan;
  /* 解析临时栈：所有当前打开的组的孩子引用连成一排。用完即释放并归还账目。 */
  LainAstRef *stack;
  uint64_t stack_used;  /* 已用条数 */
  uint64_t stack_cap;   /* 容量条数 */
  uint64_t stack_bytes; /* 已计账字节 */
} LainMetaBuilder;

/* 分配一块计账内存。**失败类别要带出去**：账户拒了是 QUOTA，malloc 失败是 ALLOC ——
 * 两者对宿主的含义完全不同，不能都笼统地叫「没内存」。 */
static void *tree_alloc(LainVmQuota *quota, size_t bytes,
                        LainMetaTreeStatus *why) {
  void *memory;
  if (lainvm_quota_charge(quota, bytes) != 0) {
    *why = LAINMETA_TREE_ERR_QUOTA;
    return NULL;
  }
  memory = malloc(bytes);
  if (!memory) {
    (void)lainvm_quota_release(quota, bytes);
    *why = LAINMETA_TREE_ERR_ALLOC;
  }
  return memory;
}

static void tree_release(LainVmQuota *quota, void *memory, uint64_t bytes) {
  if (!memory) return;
  free(memory);
  (void)lainvm_quota_release(quota, bytes);
}

/* 临时栈扩容：翻倍式增长，先过账户再 realloc。失败类别同样要分开报。 */
static bool stack_reserve(LainMetaBuilder *builder, uint64_t want,
                          LainMetaTreeStatus *why) {
  uint64_t next;
  LainAstRef *grown;
  uint64_t old_bytes;
  uint64_t new_bytes;
  if (want <= builder->stack_cap) return true;
  next = builder->stack_cap ? builder->stack_cap : 64u;
  while (next < want) {
    uint64_t doubled = next * 2u;
    if (doubled <= next) {
      *why = LAINMETA_TREE_ERR_QUOTA; /* 尺寸算到溢出了：当资源拒绝 */
      return false;
    }
    next = doubled;
  }
  if (next > (uint64_t)(size_t)-1 / sizeof(*grown)) {
    *why = LAINMETA_TREE_ERR_QUOTA;
    return false;
  }
  old_bytes = builder->stack_bytes;
  new_bytes = next * (uint64_t)sizeof(*grown);
  if (lainvm_quota_charge(builder->tree->quota, new_bytes - old_bytes) != 0) {
    *why = LAINMETA_TREE_ERR_QUOTA;
    return false;
  }
  grown = (LainAstRef *)realloc(builder->stack, (size_t)new_bytes);
  if (!grown) {
    (void)lainvm_quota_release(builder->tree->quota, new_bytes - old_bytes);
    *why = LAINMETA_TREE_ERR_ALLOC;
    return false;
  }
  builder->stack = grown;
  builder->stack_cap = next;
  builder->stack_bytes = new_bytes;
  return true;
}

static void stack_drop(LainMetaBuilder *builder) {
  tree_release(builder->tree->quota, builder->stack, builder->stack_bytes);
  builder->stack = NULL;
  builder->stack_used = 0;
  builder->stack_cap = 0;
  builder->stack_bytes = 0;
}

static unsigned char closing(unsigned char open) {
  if (open == '(') return ')';
  if (open == '[') return ']';
  if (open == '{') return '}';
  return 0;
}

static bool word(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c >= 128;
}

static uint32_t token_end(const char *text, uint32_t length, uint32_t at) {
  unsigned char c = (unsigned char)text[at];
  uint32_t next = at + 1u;
  if (word(c)) {
    while (next < length && word((unsigned char)text[next])) next++;
  } else if (c == '"') {
    while (next < length) {
      if (text[next] == '\\' && next + 1u < length) {
        next += 2u;
        continue;
      }
      if (text[next++] == '"') break;
    }
  } else if (next < length && ((c == ':' && text[next] == ':') ||
                               (c == '-' && text[next] == '>') ||
                               (c == '.' && text[next] == '.'))) {
    next++;
  }
  return next;
}

/* 本 Arena 内的引用（root、孩子列表项）用本段段号。 */
static LainAstRef encode(LainMetaTree *tree, uint64_t offset) {
  return lain_ast_ref(tree->segment_id, offset);
}

/* 文本引用用文本段段号（默认与源段相同，宿主随后用 set_segment 定下真实值）。 */
static LainAstRef encode_text(LainMetaTree *tree, uint64_t offset) {
  return lain_ast_ref(tree->text_segment, offset);
}

/* 取号。**两遍都要调用**（见文件头规矩 2）。 */
static uint64_t take_slot(LainMetaBuilder *builder, bool is_group) {
  return is_group ? builder->next_group++ : builder->next_leaf++;
}

static uint64_t slot_offset(const LainMetaBuilder *builder, uint64_t slot) {
  return builder->nodes_start + slot * (uint64_t)LAIN_AST_NODE_STRIDE;
}

/* 先减后加式的区间检查：区间必须整个落在已分配字节内。 */
static bool region_fits(const LainMetaBuilder *builder, uint64_t base,
                        uint64_t bytes) {
  if (base > builder->capacity) return false;
  if (bytes > builder->capacity - base) return false;
  return true;
}

/* Header 是映射格式的一部分：逐字段写出（零初始化 + 赋值），不写结构体的填充。 */
static void write_header(LainMetaTree *tree, uint64_t capacity,
                         uint64_t used_bytes, uint64_t node_count,
                         uint64_t node_capacity, LainAstRef root) {
  LainAstArenaHeader header;
  memset(&header, 0, sizeof(header));
  header.magic = LAIN_AST_MAGIC;
  header.abi_version = (uint16_t)LAIN_AST_ABI_VERSION;
  header.node_stride = (uint16_t)LAIN_AST_NODE_STRIDE;
  header.segment_id = tree->segment_id;
  header.flags = 0;
  header.capacity_bytes = capacity;
  header.used_bytes = used_bytes;
  header.nodes_offset = LAIN_AST_HEADER_SIZE;
  header.node_count = node_count;
  header.node_capacity = node_capacity;
  header.root = root;
  memcpy(tree->arena, &header, sizeof(header));
}

static void fill_token(LainMetaBuilder *builder, uint64_t slot, uint64_t at,
                       uint64_t end) {
  LainAstNode node;
  uint64_t offset = slot_offset(builder, slot);
  memset(&node, 0, sizeof(node));
  node.kind = (uint8_t)LAIN_AST_TOKEN;
  node.delimiter = 0;
  node.flags = 0;
  node.child_count = 0;
  node.text = encode_text(builder->tree, at);
  node.text_length = end - at; /* 不含 NUL；0/0 是合法空文本 */
  node.children_offset = 0;
  node.origin = LAIN_AST_REF_NONE;
  node.source_span = node.text;
  node.source_length = node.text_length;
  memcpy(builder->arena + offset, &node, sizeof(node));
  builder->last = encode(builder->tree, offset);
  /* node_at 由 parse_group 统一推进——fill_* 只管写节点，不参与计数。 */
}

/* 组闭合。`scratch_start` 是进括号时记下的临时栈起点，孩子已经连续攒在
 * stack[scratch_start, scratch_start + count) 里。`is_root` 时 delimiter 恒为 0、
 * 跨度覆盖全文（顶层的括号是它**孩子**的分隔符，不是根自己的）。 */
static bool fill_group(LainMetaBuilder *builder, unsigned char open,
                       uint64_t node_start, uint64_t group_end,
                       uint64_t scratch_start, uint32_t count, uint64_t slot,
                       bool is_root) {
  LainAstNode node;
  uint64_t offset;
  uint64_t children_at = 0;
  if (count) {
    uint64_t bytes = (uint64_t)count * 8u;
    children_at = builder->append_at;
    if (!region_fits(builder, children_at, bytes)) return false;
    memcpy(builder->arena + children_at, builder->stack + scratch_start,
           (size_t)bytes);
    builder->append_at += bytes;
  }
  offset = slot_offset(builder, slot);
  memset(&node, 0, sizeof(node));
  node.kind = (uint8_t)LAIN_AST_GROUP;
  node.delimiter = is_root ? 0u : open;
  node.flags = 0;
  node.child_count = count;
  node.text = encode_text(builder->tree, node_start);
  node.text_length =
      open ? group_end - node_start : (uint64_t)builder->tree->source_length;
  node.children_offset = children_at;
  node.origin = LAIN_AST_REF_NONE;
  node.source_span = node.text;
  node.source_length = node.text_length;
  memcpy(builder->arena + offset, &node, sizeof(node));
  builder->last = encode(builder->tree, offset);
  return true;
}

/* 一个组。第一遍只数节点与孩子引用并检查错误，第二遍填；两遍都取槽位号。 */
static bool parse_group(LainMetaBuilder *builder, uint32_t *at,
                        unsigned char open, uint32_t depth,
                        uint32_t *error_offset) {
  LainMetaTree *tree = builder->tree;
  uint64_t scratch_start = builder->stack_used;
  uint32_t count = 0;
  uint32_t start = *at;
  bool matched = false;
  uint64_t group_slot;
  /* 组的槽位在**进入**时取（先序），并存进局部变量：递归会改写 builder->slot，
   * 用公用字段就会被孩子的号覆盖。0 号槽 = 最外层那个组 = 根。 */
  group_slot = take_slot(builder, true);
  if (depth > 256) goto error;
  /* 只有**真的有开括号**时才跳过它。根（open = 0）没有开括号可跳：跳过首字节会
   * 把源码的第一个字符当成组外内容，于是 `(a b)` 数出 4 个节点而不是 3 个
   * ——旧实现的判据就是 open 非零。 */
  if (open) ++*at;
  while (*at < tree->source_length) {
    unsigned char c = (unsigned char)tree->source[*at];
    uint32_t end;
    if (c <= 32) {
      ++*at;
      continue;
    }
    if (c == '/' && *at + 1u < tree->source_length &&
        tree->source[*at + 1u] == '/') {
      *at += 2u;
      while (*at < tree->source_length && tree->source[*at] != '\n') ++*at;
      continue;
    }
    if (c == '/' && *at + 1u < tree->source_length &&
        tree->source[*at + 1u] == '*') {
      uint32_t comment_start = *at;
      *at += 2u;
      while (*at + 1u < tree->source_length &&
             !(tree->source[*at] == '*' && tree->source[*at + 1u] == '/'))
        ++*at;
      if (*at + 1u >= tree->source_length) {
        *at = comment_start;
        goto error;
      }
      *at += 2u;
      continue;
    }
    if (c == ')' || c == ']' || c == '}') {
      if (!open || c != closing(open)) goto error;
      ++*at;
      matched = true;
      break;
    }
    if (closing(c)) {
      if (!parse_group(builder, at, c, depth + 1u, error_offset)) return false;
    } else {
      end = token_end(tree->source, tree->source_length, *at);
      builder->slot = take_slot(builder, false);
      if (builder->arena) {
        fill_token(builder, builder->slot, *at, end);
        builder->node_at++; /* 已发布一个词 */
      } else {
        builder->scan->node_total++;
        builder->scan->leaf_total++;
      }
      *at = end;
    }
    if (builder->arena) {
      /* 暂存：把这个孩子引用接到本组暂存段的末尾。扩容失败要记类别（配额还是
       * 分配），不能让它退化成「语法错」。 */
      if (!stack_reserve(builder, scratch_start + (uint64_t)count + 1u,
                         &builder->fail))
        goto error;
      builder->stack[scratch_start + count] = builder->last;
      builder->stack_used = scratch_start + (uint64_t)count + 1u;
    }
    count++;
  }
  if (open && !matched) goto error;
  if (builder->arena) {
    if (!fill_group(builder, open, start, *at, scratch_start, count, group_slot,
                    false)) {
      /* 填充阶段写不下：这是**实现自检**失败，不是源码语法错。 */
      builder->fail = LAINMETA_TREE_ERR_INTERNAL;
      goto error;
    }
    builder->node_at++; /* 已发布一个组 */
  } else {
    builder->scan->edge_total += count;
    builder->scan->node_total++;
    builder->scan->group_total++;
  }
  builder->stack_used = scratch_start; /* 本组的暂存段到此结束 */
  return true;
error:
  if (error_offset) *error_offset = *at;
  return false;
}

/* 两遍扫描 + 一次分配。返回**失败类别**：OK = 成功，SYNTAX = 语法错
 *（error_offset 已填），QUOTA / ALLOC = 这一次分配被账户拒了 / malloc 失败，
 * INTERNAL = 两遍扫描对不上或发布前自检没过（**不是**输入的问题，不冒充语法错）。 */
static LainMetaTreeStatus parse_input(LainMetaTree *tree,
                                      uint32_t *error_offset) {
  LainMetaScan scan;
  LainMetaBuilder builder;
  uint64_t nodes_bytes, edges_bytes, total;
  uint64_t count_nodes, count_edges, count_groups, count_leaves;
  LainAstRef root_ref;
  LainMetaTreeStatus fail = LAINMETA_TREE_ERR_SYNTAX;
  memset(&scan, 0, sizeof(scan));
  memset(&builder, 0, sizeof(builder));
  builder.tree = tree;
  builder.scan = &scan;
  builder.nodes_start = LAIN_AST_HEADER_SIZE;
  builder.fail = LAINMETA_TREE_OK;
  /* node_at 数"第二遍已发布的槽"，第一遍不发布任何节点，所以必须清零。 */
  builder.node_at = 0;
  builder.next_group = 0;
  builder.next_leaf = 0;
  builder.last = LAIN_AST_REF_NONE;

  /* 第一遍：数 N（含根）、E、G（含根）、L，并检查语法。第一遍不建 Arena，
   * 所以这里只可能出语法错 —— 临时栈与 Arena 都还没动。 */
  {
    uint32_t at = 0;
    if (!parse_group(&builder, &at, 0, 0, error_offset))
      return builder.fail != LAINMETA_TREE_OK ? builder.fail
                                              : LAINMETA_TREE_ERR_SYNTAX;
  }
  if (scan.node_total == 0) return LAINMETA_TREE_ERR_SYNTAX; /* 根节点至少一个 */
  count_nodes = scan.node_total;
  count_edges = scan.edge_total;
  count_groups = scan.group_total; /* 含根 */
  count_leaves = scan.leaf_total;
  /* 计数自相矛盾是实现问题，不是源码问题。 */
  if (count_groups + count_leaves != count_nodes)
    return LAINMETA_TREE_ERR_INTERNAL;
  if (count_groups == 0) return LAINMETA_TREE_ERR_INTERNAL;

  /* 尺寸算不下：这是**资源**拒绝（配额一类），不是语法错。 */
  if (count_nodes >
      (UINT64_MAX - LAIN_AST_HEADER_SIZE) / (uint64_t)LAIN_AST_NODE_STRIDE)
    return LAINMETA_TREE_ERR_QUOTA;
  nodes_bytes = count_nodes * (uint64_t)LAIN_AST_NODE_STRIDE;
  if (count_edges > (UINT64_MAX - LAIN_AST_HEADER_SIZE - nodes_bytes) / 8u)
    return LAINMETA_TREE_ERR_QUOTA;
  edges_bytes = count_edges * 8u;
  total = LAIN_AST_HEADER_SIZE + nodes_bytes + edges_bytes;
  if (total > (uint64_t)LAIN_AST_OFFSET_MASK) return LAINMETA_TREE_ERR_QUOTA;
  if (total > (uint64_t)(size_t)-1) return LAINMETA_TREE_ERR_QUOTA;
  tree->arena = (unsigned char *)tree_alloc(tree->quota, (size_t)total, &fail);
  if (!tree->arena) return fail;
  tree->arena_bytes = total;

  /* 第二遍：按同样的顺序、同样的取号序列填进准确预分配的 Arena。 */
  scan.node_total = 0;
  scan.edge_total = 0;
  builder.arena = tree->arena;
  builder.capacity = total;
  builder.nodes_bytes = nodes_bytes;
  builder.append_at = LAIN_AST_HEADER_SIZE + nodes_bytes;
  /* 取号规则（两遍一致）：组按先序拿 0..G-1，词在组号全部发完之后拿 G..G+L-1。
   * 根就是最外层那个组，占 0 号槽——不额外留槽，也不另填一次。 */
  builder.next_group = 0;
  builder.next_leaf = count_groups;
  builder.node_at = 0;
  builder.last = LAIN_AST_REF_NONE;
  memset(tree->arena, 0, (size_t)total);
  {
    uint32_t at = 0;
    if (!parse_group(&builder, &at, 0, 0, error_offset)) {
      /* 失败扩容保留旧栈；所有失败出口都归还它的实际存储和配额。 */
      stack_drop(&builder);
      /* 第二遍才可能遇到临时栈扩容失败（第一遍不建栈）。这时 error_offset 是错的，
       * 不能报成语法错 —— 要用 builder.fail 里那一类。 */
      if (builder.fail != LAINMETA_TREE_OK) {
        if (error_offset) *error_offset = 0;
        return builder.fail;
      }
      return LAINMETA_TREE_ERR_SYNTAX;
    }
  }
  /* 根由 parse_group 按普通组发布（它是先序遍历里的第一个组，所以占 0 号槽）。
   * 这里只把它的**形状**改成"根"：delimiter 恒为 0、跨度覆盖全文。顶层的括号是它
   * 孩子的分隔符，所以 `(a b)` 仍是无分隔符根下的一个孩子组。 */
  {
    uint64_t root_off = slot_offset(&builder, LAIN_TREE_ROOT_SLOT);
    LainAstNode root_node;
    memcpy(&root_node, tree->arena + root_off, sizeof(root_node));
    root_node.delimiter = 0;
    root_node.text = encode_text(tree, 0);
    root_node.text_length = (uint64_t)tree->source_length;
    memcpy(tree->arena + root_off, &root_node, sizeof(root_node));
  }
  /* 两个游标都是"下一个空号"：组走完停在 G，词走完停在 G+L。 */
  if (builder.node_at != count_nodes ||
      builder.next_group != count_groups ||
      builder.next_leaf != count_groups + count_leaves) {
    /* 两遍的取号序列对不上：计数与填充分家了。不许发布半成品。 */
    stack_drop(&builder);
    if (error_offset) *error_offset = 0;
    return LAINMETA_TREE_ERR_INTERNAL;
  }

  /* 结果自检：每个引用都必须落在已发布的节点槽上，孩子列表不得与节点预留区
   * 重叠，也不得越出已分配水位。这是"发布前最后一道门"，失败就不发布 —— 这是
   * **实现自检**，不是语法错，所以报 INTERNAL 而不是拿语法错盖过去。 */
  {
    uint64_t i;
    for (i = 0; i < count_nodes; i++) {
      uint64_t off = slot_offset(&builder, i);
      LainAstNode node;
      memcpy(&node, tree->arena + off, sizeof(node));
      if (node.kind != LAIN_AST_TOKEN && node.kind != LAIN_AST_GROUP) {
        goto internal;
      }
      if (node.kind == LAIN_AST_TOKEN) {
        if (node.child_count != 0 || node.children_offset != 0) {
          goto internal;
        }
        if (node.text_length &&
            lain_ast_segment(node.text) != tree->text_segment)
          goto internal;
        if (node.text_length && lain_ast_offset(node.text) + node.text_length >
                                    (uint64_t)tree->source_length)
          goto internal;
      } else {
        if (node.delimiter && !closing(node.delimiter)) goto internal;
        if (node.child_count == 0) {
          if (node.children_offset != 0) goto internal;
        } else {
          uint64_t bytes = (uint64_t)node.child_count * 8u;
          uint32_t k;
          if (!node.children_offset) goto internal;
          if (!region_fits(&builder, node.children_offset, bytes)) {
            goto internal;
          }
          if (node.children_offset < LAIN_AST_HEADER_SIZE + nodes_bytes)
            goto internal; /* 不得与节点预留区重叠 */
          for (k = 0; k < node.child_count; k++) {
            LainAstRef child;
            memcpy(&child, tree->arena + node.children_offset + k * 8u, 8u);
            if (lain_ast_segment(child) != tree->segment_id) {
              goto internal;
            }
            if (lain_ast_offset(child) < LAIN_AST_HEADER_SIZE) goto internal;
            if (lain_ast_offset(child) >=
                LAIN_AST_HEADER_SIZE +
                    count_nodes * (uint64_t)LAIN_AST_NODE_STRIDE)
              {
                goto internal; /* 不许指向未发布的槽 */
              }
            if ((lain_ast_offset(child) - LAIN_AST_HEADER_SIZE) %
                    LAIN_AST_NODE_STRIDE !=
                0)
              goto internal;
          }
        }
      }
    }
  }
  root_ref = encode(tree, slot_offset(&builder, LAIN_TREE_ROOT_SLOT));
  tree->root = root_ref;
  /* Header.node_count 是**全部**已发布槽（含根）；node_at 只数非根，所以这里
   * 传 count_nodes。 */
  write_header(tree, total, builder.append_at, count_nodes, count_nodes,
               root_ref);
  stack_drop(&builder);
  return LAINMETA_TREE_OK;

internal:
  stack_drop(&builder);
  if (error_offset) *error_offset = 0;
  return LAINMETA_TREE_ERR_INTERNAL;
}

LainMetaTree *lainmeta_tree_parse_classified(uint32_t source_index,
                                             const char *text, uint32_t length,
                                             uint32_t *error_offset,
                                             LainVmQuota *quota,
                                             LainMetaTreeStatus *status_out) {
  LainMetaTree *tree;
  LainMetaTreeStatus status;
  if (error_offset) *error_offset = 0;
  if (status_out) *status_out = LAINMETA_TREE_OK;
  if (!text && length) {
    if (status_out) *status_out = LAINMETA_TREE_ERR_SYNTAX;
    return NULL;
  }
  tree = (LainMetaTree *)calloc(1, sizeof(*tree));
  if (!tree) {
    if (status_out) *status_out = LAINMETA_TREE_ERR_ALLOC;
    return NULL;
  }
  tree->source = text;
  tree->source_length = length;
  tree->source_index = source_index;
  tree->quota = quota;
  tree->segment_id = LAIN_AST_SOURCE; /* 宿主随后用 set_segment 补真实段号 */
  tree->text_segment = LAIN_AST_SOURCE;
  status = parse_input(tree, error_offset);
  if (status != LAINMETA_TREE_OK) {
    /* 失败不发布半成品：Arena 归账、结构释放。 */
    lainmeta_tree_free(tree);
    if (status_out) *status_out = status;
    return NULL;
  }
  return tree;
}

LainMetaTree *lainmeta_tree_parse_with_quota(uint32_t source_index,
                                             const char *text, uint32_t length,
                                             uint32_t *error_offset,
                                             LainVmQuota *quota) {
  return lainmeta_tree_parse_classified(source_index, text, length, error_offset,
                                        quota, NULL);
}

LainMetaTree *lainmeta_tree_parse(uint32_t source_index, const char *text,
                                  uint32_t length, uint32_t *error_offset) {
  return lainmeta_tree_parse_classified(source_index, text, length, error_offset,
                                        NULL, NULL);
}

void lainmeta_tree_free(LainMetaTree *tree) {
  if (!tree) return;
  tree_release(tree->quota, tree->arena, tree->arena_bytes);
  free(tree);
}

bool lainmeta_tree_set_segment(LainMetaTree *tree, uint16_t segment_id,
                               uint16_t text_segment) {
  if (!tree || segment_id == 0 || text_segment == 0) return false;
  tree->segment_id = segment_id;
  tree->text_segment = text_segment;
  if (tree->arena) {
    /* Header 的 segment_id 与 root 引用的段号必须一起改，否则段身份对不上。 */
    LainAstArenaHeader header;
    memcpy(&header, tree->arena, sizeof(header));
    header.segment_id = segment_id;
    header.root = lain_ast_ref(segment_id, lain_ast_offset(header.root));
    memcpy(tree->arena, &header, sizeof(header));
    tree->root = header.root;
    {
      uint64_t i;
      /* 节点里的段内引用都要跟着改：**文本引用**与**孩子列表的每一项**都是
       * 「段号 + 段内偏移」。只改文本、漏改孩子列表，段身份就会半新半旧。 */
      for (i = 0; i < header.node_count; i++) {
        uint64_t off =
            header.nodes_offset + i * (uint64_t)LAIN_AST_NODE_STRIDE;
        LainAstNode node;
        uint64_t k;
        memcpy(&node, tree->arena + off, sizeof(node));
        /* 文本引用改到**文本段**（Source），不是本 Arena 的段。 */
        if (node.text)
          node.text = lain_ast_ref(text_segment, lain_ast_offset(node.text));
        if (node.source_span)
          node.source_span = lain_ast_ref(text_segment, lain_ast_offset(node.source_span));
        memcpy(tree->arena + off, &node, sizeof(node));
        for (k = 0; k < node.child_count; k++) {
          uint64_t at;
          LainAstRef child;
          if (k > (UINT64_MAX - node.children_offset) / 8u) break;
          at = node.children_offset + k * 8u;
          if (at > tree->arena_bytes || 8u > tree->arena_bytes - at) break;
          memcpy(&child, tree->arena + at, 8u);
          if (!child) continue;
          child = lain_ast_ref(segment_id, lain_ast_offset(child));
          memcpy(tree->arena + at, &child, 8u);
        }
      }
    }
  }
  return true;
}

bool lainmeta_tree_arena(const LainMetaTree *tree, LainAstArenaView *out) {
  LainAstArenaHeader header;
  if (!tree || !out) return false;
  if (!tree->arena || tree->arena_bytes < LAIN_AST_HEADER_SIZE) return false;
  memcpy(&header, tree->arena, sizeof(header));
  out->data = tree->arena;
  out->capacity = header.capacity_bytes;
  out->used = header.used_bytes;
  out->segment_id = tree->segment_id;
  out->root = tree->root;
  return true;
}
