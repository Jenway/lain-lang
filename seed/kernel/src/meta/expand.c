/* 宏展开驱动：走输入 Arena，按 expand.h 的帧协议调用处理器，把判定落到 AstOut。
 *
 * 四条纪律：
 *   1. 先立事务再问处理器：处理器要往 AstOut 追加替换子树，事务和水位必须已经存在，
 *      expansion_id 也才能随请求下发（节点追加时记的就是它）。
 *   2. 未采纳的判定（NO_MATCH / DEFER / ERROR）一律回滚到自己的检查点，只有 REPLACE
 *      留下节点。回滚是游标复位，不遍历释放。
 *   3. 替换子树还要继续下探：宏的产物里可能有别的宏，因此替换结果按 depth+1 再走一遍；
 *      输入子树按同一 depth 下探，展开层数由 max_depth 收住。
 *   4. 孩子变了才重建组：替换是局部的，没被动的组原样引用回去，不复制。
 *
 * DEFER 在本期只记账不改树：依赖未满足的形式原样留下，由调度器在 Elaborate 之前再跑
 * 一次驱动；驱动不替它编造「稍后必成」的次序。
 */
#include "expand_driver.h"

#include <stdlib.h>
#include <string.h>

#include "ast_out.h"

typedef struct {
  const LainExpandRun *run;
  LainAstOutput *out;
  LainExpandReport *report;
} ExpandPass;

static uint32_t node_of(ExpandPass *p, LainAstRef ref, LainAstNode *node) {
  if (lain_ast_segment(ref) == LAIN_AST_OUT)
    return lain_ast_output_node(p->out, ref, node);
  return p->run->resolve(p->run->resolve_context, ref, node);
}

static uint32_t child_of(ExpandPass *p, LainAstRef parent, uint64_t index,
                         LainAstRef *out) {
  if (lain_ast_segment(parent) == LAIN_AST_OUT)
    return lain_ast_output_child(p->out, parent, index, out);
  return p->run->child(p->run->child_context, parent, index, out);
}

/* 用给定的孩子列表重建一个组。origin 指向被重建的原节点，源码跨度随之继承。 */
static uint32_t rebuild(ExpandPass *p, LainAstRef origin, uint8_t delimiter,
                        const LainAstRef *kids, uint32_t count,
                        LainAstRef *out) {
  LainAstRef slice = 0;
  uint32_t rc = lain_ast_output_refs(p->out, kids, count, &slice);
  if (rc) return rc;
  return lain_ast_output_group(p->out, delimiter, slice, count, origin, 0, out);
}

static uint32_t expand_form(ExpandPass *p, LainAstRef ref, uint32_t depth,
                            LainAstRef *result, bool *changed) {
  LainAstNode node;
  LainAstRef *kids;
  uint64_t txn = 0, verdict;
  uint32_t rc, i;
  bool kids_changed = false;

  *result = ref;
  *changed = false;
  rc = node_of(p, ref, &node);
  if (rc) return rc;
  p->report->forms++;
  if (depth > p->report->deepest) p->report->deepest = depth;
  if (node.kind == LAIN_AST_TOKEN) return 0;

  /* 1. 先问处理器：宏看到的是原始子树，替换结果由它自己构造。 */
  if (depth < p->run->limits->max_depth) {
    LainExpandRequest request;
    LainExpandResult answer;
    rc = lain_ast_output_begin(p->out, &txn);
    if (rc) return rc;
    memset(&request, 0, sizeof(request));
    memset(&answer, 0, sizeof(answer));
    request.abi_version = 1;
    request.parent = ref;
    request.expansion_id = txn;
    rc = lain_ast_output_charge(p->out, 2, 1);
    if (rc) {
      (void)lain_ast_output_rollback(p->out, txn);
      return rc;
    }
    p->report->handler_calls++;
    verdict = p->run->handler(p->run->handler_context, &request, &answer);
    if (verdict == LAIN_EXPAND_REPLACE) {
      if (!answer.replacement) {
        (void)lain_ast_output_rollback(p->out, txn);
        return LAIN_AST_ERR_PROTOCOL;
      }
      rc = expand_form(p, answer.replacement, depth + 1, result, changed);
      if (rc) {
        (void)lain_ast_output_rollback(p->out, txn);
        return rc;
      }
      rc = lain_ast_output_release(p->out, txn);
      if (rc) return rc;
      p->report->replacements++;
      *changed = true;
      return 0;
    }
    rc = lain_ast_output_rollback(p->out, txn);
    if (rc) return rc;
    if (verdict == LAIN_EXPAND_ERROR) {
      p->report->errors++;
      return answer.diagnostic ? (uint32_t)answer.diagnostic : LAIN_AST_ERR_STAGE;
    }
    if (verdict == LAIN_EXPAND_DEFER) {
      p->report->deferred++;
      return 0;
    }
  }

  /* 2. 没被替换：孩子逐个下探，任何一个变了就重建本组。 */
  if (!node.child_count) return 0;
  kids = (LainAstRef *)malloc((size_t)node.child_count * sizeof(LainAstRef));
  if (!kids) return LAIN_AST_ERR_CAPACITY;
  for (i = 0; i < node.child_count; i++) {
    LainAstRef child, moved;
    bool child_changed = false;
    rc = child_of(p, ref, i, &child);
    if (!rc) rc = expand_form(p, child, depth, &moved, &child_changed);
    if (rc) {
      free(kids);
      return rc;
    }
    kids[i] = moved;
    if (child_changed) kids_changed = true;
  }
  if (kids_changed) {
    rc = rebuild(p, ref, node.delimiter, kids, node.child_count, result);
    free(kids);
    if (rc) return rc;
    *changed = true;
    return 0;
  }
  free(kids);
  return 0;
}

uint32_t lainmeta_expand_drive(LainAstOutput *out, const LainExpandRun *run,
                               LainExpandReport *report) {
  LainExpandReport scratch;
  LainAstArenaView view;
  ExpandPass pass;
  LainAstRef root = 0;
  uint64_t outer = 0;
  uint32_t rc;
  bool changed = false;

  if (!out || !run || !run->handler || !run->child || !run->resolve ||
      !run->limits || !run->root)
    return LAIN_AST_ERR_PROTOCOL;
  if (!report) report = &scratch;
  memset(report, 0, sizeof(*report));
  if (!lain_ast_output_view(out, &view)) return LAIN_AST_ERR_PROTOCOL;
  report->nodes_before = view.used;
  pass.run = run;
  pass.out = out;
  pass.report = report;

  rc = lain_ast_output_begin(out, &outer);
  if (rc) return rc;
  rc = expand_form(&pass, run->root, 0, &root, &changed);
  if (rc) {
    (void)lain_ast_output_rollback(out, outer);
    return rc;
  }
  if (!changed) {
    (void)lain_ast_output_rollback(out, outer);
  } else {
    rc = lain_ast_output_commit(out, outer, root);
    if (rc) {
      (void)lain_ast_output_rollback(out, outer);
      return rc;
    }
  }
  if (lain_ast_output_view(out, &view)) report->nodes_after = view.used;
  return 0;
}
