/* AstOut 的可信存储实现接口，仅由宿主使用。 */
#ifndef LAINMETA_AST_OUT_INTERNAL_H
#define LAINMETA_AST_OUT_INTERNAL_H
#include "lain/meta/ast.h"
#include "lain/meta/expand.h"
#include "lain/vm/quota.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct LainAstOutput LainAstOutput;
typedef uint32_t (*LainAstResolve)(void *, LainAstRef, LainAstNode *);
LainAstOutput *lain_ast_output_new(const LainExpandLimits *, LainVmQuota *,
                                 void *, LainAstResolve, uint32_t *);
void lain_ast_output_free(LainAstOutput *);
bool lain_ast_output_view(const LainAstOutput *, LainAstArenaView *);
/* 只读计数：宿主把 apply 请求并入统一预算后，驱动要能核对实际扣账。
 * 两个函数都不改账目、不分配。 */
uint64_t lain_ast_output_handler_calls(const LainAstOutput *);
uint64_t lain_ast_output_visits(const LainAstOutput *);
uint32_t lain_ast_output_check(LainAstOutput *, uint64_t, uint64_t);
uint32_t lain_ast_output_charge(LainAstOutput *, uint64_t, uint64_t);
uint32_t lain_ast_output_begin(LainAstOutput *, uint64_t *);
uint32_t lain_ast_output_release(LainAstOutput *, uint64_t);
uint32_t lain_ast_output_rollback(LainAstOutput *, uint64_t);
uint32_t lain_ast_output_token(LainAstOutput *, const void *, uint64_t,
                              LainAstRef, uint64_t, LainAstRef *);
uint32_t lain_ast_output_refs(LainAstOutput *, const LainAstRef *, uint64_t,
                             LainAstRef *);
uint32_t lain_ast_output_group(LainAstOutput *, uint64_t, LainAstRef, uint64_t,
                              LainAstRef, uint64_t, LainAstRef *);
uint32_t lain_ast_output_commit(LainAstOutput *, uint64_t, LainAstRef);
uint32_t lain_ast_output_expansion(LainAstOutput *, LainAstRef, uint64_t *);
/* 读回一个**输出段**节点（段号 3）与它的孩子。驱动沿替换结果继续展开时用这两个：
 * 节点与孩子列表都在可信副本里，不必再经跨段解析。只接受段 3 的引用，
 * 段号不符返回 LAIN_AST_ERR_SEGMENT，槽位非法返回 LAIN_AST_ERR_SLOT，
 * 非组节点取孩子返回 LAIN_AST_ERR_SHAPE。 */
uint32_t lain_ast_output_node(const LainAstOutput *, LainAstRef, LainAstNode *);
uint32_t lain_ast_output_child(const LainAstOutput *, LainAstRef, uint64_t,
                               LainAstRef *);
/* 当前已发布的节点数（只读）。事务回滚后随之减少，宿主据此截断为节点下标
 * 平行的索引。 */
uint64_t lain_ast_output_node_count(const LainAstOutput *);
#endif
