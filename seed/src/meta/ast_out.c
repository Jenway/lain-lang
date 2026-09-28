/* 语言无关的追加 Arena。映射副本可写，可信副本不映射；全部修改只走能力。
 * 可信副本、事务与列表登记一并计入配额，映射期间从不 realloc。
 * 引用只能指向先前完整节点，因而正常构造天然无环；提交比较整片已用字节，
 * 不依赖可写 Header 的计数或水位。 */
#include "ast_out.h"
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t id, count, tail, slices; } Transaction;
typedef struct { uint64_t offset, count; } Slice;
struct LainAstOutput {
  unsigned char *data, *trusted;
  LainAstArenaHeader header;
  Transaction *transactions;
  Slice *slices;
  uint64_t *expansion_ids;
  uint64_t depth, next_id, slice_count, visits, handler_calls;
  uint64_t replacement_refs, expansion_depth, charged;
  LainExpandLimits limits;
  LainVmQuota *quota;
  void *context;
  LainAstResolve resolve;
};

static void publish(LainAstOutput *o) {
  memcpy(o->trusted, &o->header, sizeof(o->header));
  memcpy(o->data, &o->header, sizeof(o->header));
}
static uint32_t charge(LainAstOutput *o, uint64_t n) {
  if (n > o->limits.max_visits - o->visits) return LAIN_AST_ERR_BUDGET;
  o->visits += n;
  return 0;
}
uint32_t lain_ast_output_charge(LainAstOutput *o, uint64_t kind, uint64_t n) {
  uint64_t *used;
  uint64_t limit;
  if (!o || !n) return LAIN_AST_ERR_PROTOCOL;
  if (kind == 1) return charge(o, n);
  if (kind == 2) {
    used = &o->handler_calls; limit = o->limits.max_handler_calls;
  } else if (kind == 3) {
    used = &o->replacement_refs; limit = o->limits.max_replacement_refs;
  } else if (kind == 4) {
    if (o->expansion_depth == o->limits.max_depth)
      return LAIN_AST_ERR_BUDGET;
    o->expansion_depth++;
    return 0;
  } else if (kind == 5) {
    if (n != 1 || !o->expansion_depth) return LAIN_AST_ERR_PROTOCOL;
    o->expansion_depth--;
    return 0;
  } else {
    return LAIN_AST_ERR_PROTOCOL;
  }
  if (n > limit - *used) return LAIN_AST_ERR_BUDGET;
  *used += n;
  return 0;
}
uint32_t lain_ast_output_check(LainAstOutput *o, uint64_t off, uint64_t len) {
  uint32_t rc;
  if (!o) return LAIN_AST_ERR_SEGMENT;
  if (off > o->header.used_bytes || len > o->header.used_bytes - off)
    return LAIN_AST_ERR_RANGE;
  rc = charge(o, len / 8 + (len % 8 != 0));
  if (rc) return rc;
  return memcmp(o->data + off, o->trusted + off, (size_t)len)
             ? LAIN_AST_ERR_PUBLISHED : 0;
}
LainAstOutput *lain_ast_output_new(const LainExpandLimits *l, LainVmQuota *q,
                                 void *ctx, LainAstResolve resolve,
                                 uint32_t *error) {
  LainAstOutput *o;
  uint64_t storage, tail;
  *error = LAIN_AST_ERR_CAPACITY;
  if (!l || !resolve || !l->max_depth || !l->max_visits ||
      !l->max_handler_calls || !l->max_replacement_refs ||
      !l->ast_out_node_capacity || l->ast_out_capacity < 64 ||
      l->ast_out_capacity > LAIN_AST_OFFSET_MASK ||
      l->ast_out_capacity > SIZE_MAX / 2 ||
      l->ast_out_node_capacity > (l->ast_out_capacity - 64) / 56 ||
      l->max_depth > SIZE_MAX / sizeof(Transaction)) return NULL;
  tail = 64 + l->ast_out_node_capacity * 56;
  storage = l->ast_out_capacity * 2;
  if (l->max_depth > (UINT64_MAX - storage) / sizeof(Transaction)) return NULL;
  storage += l->max_depth * sizeof(Transaction);
  /* 切片最多每八字节一条，登记空间按同一固定容量预留。 */
  if (l->ast_out_capacity / 8 > (UINT64_MAX - storage) / sizeof(Slice))
    return NULL;
  storage += l->ast_out_capacity / 8 * sizeof(Slice);
  if (l->ast_out_node_capacity > (UINT64_MAX - storage) / sizeof(uint64_t))
    return NULL;
  storage += l->ast_out_node_capacity * sizeof(uint64_t);
  if (sizeof(*o) > UINT64_MAX - storage) return NULL;
  storage += sizeof(*o);
  *error = (uint32_t)lainvm_quota_charge(q, storage);
  if (*error) return NULL;
  o = calloc(1, sizeof(*o));
  if (o) {
    o->data = calloc(1, (size_t)l->ast_out_capacity);
    o->trusted = calloc(1, (size_t)l->ast_out_capacity);
    o->transactions = calloc((size_t)l->max_depth, sizeof(Transaction));
    o->slices = calloc((size_t)(l->ast_out_capacity / 8), sizeof(Slice));
    o->expansion_ids = calloc((size_t)l->ast_out_node_capacity, sizeof(uint64_t));
  }
  if (!o || !o->data || !o->trusted || !o->transactions || !o->slices ||
      !o->expansion_ids) {
    if (o) { free(o->data); free(o->trusted); free(o->transactions);
             free(o->slices); free(o->expansion_ids); free(o); }
    (void)lainvm_quota_release(q, storage);
    *error = 2;
    return NULL;
  }
  o->quota = q; o->charged = storage; o->limits = *l;
  o->context = ctx; o->resolve = resolve;
  o->header = (LainAstArenaHeader){LAIN_AST_MAGIC, 1, 56, 3, 0,
      l->ast_out_capacity, tail, 64, 0, l->ast_out_node_capacity, 0};
  publish(o);
  *error = 0;
  return o;
}
void lain_ast_output_free(LainAstOutput *o) {
  if (!o) return;
  free(o->data); free(o->trusted); free(o->transactions); free(o->slices);
  free(o->expansion_ids);
  (void)lainvm_quota_release(o->quota, o->charged);
  free(o);
}
bool lain_ast_output_view(const LainAstOutput *o, LainAstArenaView *v) {
  if (!o || !v) return false;
  *v = (LainAstArenaView){o->data, o->header.capacity_bytes,
                         o->header.used_bytes, 3, o->header.root};
  return true;
}
static uint32_t top(LainAstOutput *o, uint64_t id) {
  return (!o || !o->depth || !id ||
          o->transactions[o->depth - 1].id != id)
             ? LAIN_AST_ERR_TRANSACTION : 0;
}
uint32_t lain_ast_output_begin(LainAstOutput *o, uint64_t *id) {
  uint32_t rc;
  *id = 0;
  if (!o) return LAIN_AST_ERR_SEGMENT;
  if (o->depth == o->limits.max_depth) return LAIN_AST_ERR_BUDGET;
  if (o->next_id == UINT64_MAX) return LAIN_AST_ERR_TRANSACTION;
  rc = lain_ast_output_check(o, 0, 64);
  if (rc) return rc;
  *id = ++o->next_id;
  o->transactions[o->depth++] = (Transaction){*id, o->header.node_count,
                                            o->header.used_bytes, o->slice_count};
  return 0;
}
uint32_t lain_ast_output_release(LainAstOutput *o, uint64_t id) {
  uint32_t rc = top(o, id);
  if (rc) return rc;
  /* 最外层必须 commit 或 rollback，不能丢失尚未发布的事务。 */
  if (o->depth == 1) return LAIN_AST_ERR_TRANSACTION;
  o->depth--;
  return 0;
}
uint32_t lain_ast_output_rollback(LainAstOutput *o, uint64_t id) {
  Transaction t;
  uint64_t at, len;
  uint32_t rc = top(o, id);
  if (rc) return rc;
  t = o->transactions[--o->depth];
  at = 64 + t.count * 56; len = (o->header.node_count - t.count) * 56;
  memset(o->data + at, 0, (size_t)len);
  memset(o->trusted + at, 0, (size_t)len);
  memset(o->expansion_ids + t.count, 0,
         (size_t)(o->header.node_count - t.count) * sizeof(uint64_t));
  len = o->header.used_bytes - t.tail;
  memset(o->data + t.tail, 0, (size_t)len);
  memset(o->trusted + t.tail, 0, (size_t)len);
  o->header.node_count = t.count; o->header.used_bytes = t.tail;
  o->slice_count = t.slices;
  publish(o);
  return 0;
}
static uint32_t room(LainAstOutput *o, uint64_t bytes, bool node,
                     uint64_t *at) {
  uint32_t rc;
  if (!o || !o->depth) return LAIN_AST_ERR_TRANSACTION;
  rc = lain_ast_output_check(o, 0, 64);
  if (rc) return rc;
  if (node && o->header.node_count == o->header.node_capacity)
    return LAIN_AST_ERR_CAPACITY;
  *at = bytes ? (o->header.used_bytes + 7) & ~UINT64_C(7)
              : o->header.used_bytes;
  if (*at > o->header.capacity_bytes ||
      bytes > o->header.capacity_bytes - *at) return LAIN_AST_ERR_CAPACITY;
  return 0;
}
static uint32_t origin_node(LainAstOutput *o, LainAstRef origin, LainAstNode *n) {
  memset(n, 0, sizeof(*n));
  if (origin) {
    LainAstNode src;
    uint32_t rc = o->resolve(o->context, origin, &src);
    if (rc) return rc;
    n->origin = origin; n->source_span = src.source_span;
    n->source_length = src.source_length;
  }
  return 0;
}
static void write_bytes(LainAstOutput *o, uint64_t at, const void *p, uint64_t n) {
  if (!n) return;
  memcpy(o->trusted + at, p, (size_t)n);
  memcpy(o->data + at, o->trusted + at, (size_t)n);
}
static void write_node(LainAstOutput *o, const LainAstNode *n,
                       uint64_t expansion_id, LainAstRef *ref) {
  uint64_t index = o->header.node_count++;
  uint64_t at = 64 + index * 56;
  write_bytes(o, at, n, sizeof(*n));
  o->expansion_ids[index] = expansion_id;
  *ref = lain_ast_ref(3, at);
  publish(o);
}
uint32_t lain_ast_output_token(LainAstOutput *o, const void *text, uint64_t len,
                              LainAstRef origin, uint64_t expansion_id,
                              LainAstRef *out) {
  uint64_t at;
  LainAstNode n;
  uint32_t rc;
  *out = 0;
  rc = room(o, len, true, &at);
  if (rc) return rc;
  rc = origin_node(o, origin, &n);
  if (rc) return rc;
  n.kind = LAIN_AST_TOKEN; n.text = len ? lain_ast_ref(3, at) : 0;
  n.text_length = len;
  write_bytes(o, at, text, len);
  o->header.used_bytes = at + len;
  write_node(o, &n, expansion_id, out);
  return 0;
}
uint32_t lain_ast_output_refs(LainAstOutput *o, const LainAstRef *refs,
                             uint64_t count, LainAstRef *out) {
  uint64_t at, i;
  uint32_t rc;
  *out = 0;
  if (count > UINT64_MAX / 8) return LAIN_AST_ERR_RANGE;
  rc = room(o, count * 8, false, &at);
  if (rc) return rc;
  rc = charge(o, count);
  if (rc) return rc;
  for (i = 0; i < count; i++) {
    LainAstNode n;
    LainAstRef ref;
    memcpy(&ref, (const unsigned char *)refs + i * 8, 8);
    rc = o->resolve(o->context, ref, &n);
    if (rc) return rc;
  }
  if (!count) return 0;
  /* 登记大小受实际八字节分配量约束，永远不超过预留表。 */
  write_bytes(o, at, refs, count * 8);
  o->slices[o->slice_count++] = (Slice){at, count};
  o->header.used_bytes = at + count * 8;
  *out = lain_ast_ref(3, at); publish(o);
  return 0;
}
uint32_t lain_ast_output_group(LainAstOutput *o, uint64_t delimiter,
                              LainAstRef slice, uint64_t count,
                              LainAstRef origin, uint64_t expansion_id,
                              LainAstRef *out) {
  uint64_t at, i;
  LainAstNode n;
  uint32_t rc;
  *out = 0;
  rc = room(o, 0, true, &at);
  if (rc) return rc;
  if (count > UINT32_MAX || (delimiter != 0 && delimiter != '(' &&
      delimiter != '[' && delimiter != '{')) return LAIN_AST_ERR_PROTOCOL;
  if (!count) { if (slice) return LAIN_AST_ERR_RANGE; }
  else {
    if (lain_ast_segment(slice) != 3) return LAIN_AST_ERR_SEGMENT;
    for (i = 0; i < o->slice_count; i++)
      if (o->slices[i].offset == lain_ast_offset(slice) &&
          o->slices[i].count == count) break;
    if (i == o->slice_count) return LAIN_AST_ERR_RANGE;
    rc = lain_ast_output_check(o, lain_ast_offset(slice), count * 8);
    if (rc) return rc;
  }
  rc = origin_node(o, origin, &n);
  if (rc) return rc;
  n.kind = LAIN_AST_GROUP; n.delimiter = (uint8_t)delimiter;
  n.child_count = (uint32_t)count;
  n.children_offset = count ? lain_ast_offset(slice) : 0;
  write_node(o, &n, expansion_id, out);
  return 0;
}

uint32_t lain_ast_output_expansion(LainAstOutput *o, LainAstRef ref,
                                   uint64_t *out) {
  uint64_t off, index;
  uint32_t rc;
  if (!out) return LAIN_AST_ERR_PROTOCOL;
  *out = 0;
  if (!o || lain_ast_segment(ref) != LAIN_AST_OUT)
    return LAIN_AST_ERR_SEGMENT;
  off = lain_ast_offset(ref);
  if (off < LAIN_AST_HEADER_SIZE ||
      (off - LAIN_AST_HEADER_SIZE) % LAIN_AST_NODE_STRIDE != 0)
    return LAIN_AST_ERR_SLOT;
  index = (off - LAIN_AST_HEADER_SIZE) / LAIN_AST_NODE_STRIDE;
  if (index >= o->header.node_count) return LAIN_AST_ERR_SLOT;
  rc = lain_ast_output_check(o, off, LAIN_AST_NODE_STRIDE);
  if (rc) return rc;
  *out = o->expansion_ids[index];
  return 0;
}
uint32_t lain_ast_output_commit(LainAstOutput *o, uint64_t id, LainAstRef root) {
  LainAstNode n;
  uint64_t i, j;
  uint32_t rc = top(o, id);
  if (rc) return rc;
  if (o->depth != 1) return LAIN_AST_ERR_TRANSACTION;
  rc = lain_ast_output_check(o, 0, o->header.used_bytes);
  if (rc) return rc;
  /* 追加时有效的跨段目标在提交时仍须存活。逐个登记切片检查，不沿 DAG
   * 重复路径递归；构造顺序已保证输出节点的边只能指向更早节点。
   * 此检查覆盖孤立的已分配节点和来源边，工作量也消耗不可回滚预算。 */
  for (i = 0; i < o->header.node_count; i++) {
    rc = charge(o, 1);
    if (rc) return rc;
    memcpy(&n, o->trusted + 64 + i * 56, sizeof(n));
    if (n.origin) {
      LainAstNode origin;
      rc = o->resolve(o->context, n.origin, &origin);
      if (rc) return rc;
    }
  }
  for (i = 0; i < o->slice_count; i++) {
    for (j = 0; j < o->slices[i].count; j++) {
      LainAstRef child;
      rc = charge(o, 1);
      if (rc) return rc;
      memcpy(&child, o->trusted + o->slices[i].offset + j * 8, 8);
      rc = o->resolve(o->context, child, &n);
      if (rc) return rc;
    }
  }
  rc = o->resolve(o->context, root, &n);
  if (rc) return rc;
  if (n.kind != LAIN_AST_GROUP || n.delimiter != 0)
    return LAIN_AST_ERR_GRAPH;
  o->header.root = root;
  o->depth = 0; publish(o);
  return 0;
}
