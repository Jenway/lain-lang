/* 展开驱动内部接口：只给宿主与专项验收用，不进 include/ 的对外头。 */
#ifndef LAINMETA_EXPAND_DRIVER_H
#define LAINMETA_EXPAND_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "lain/meta/ast_v1.h"
#include "lain/meta/expand.h"

typedef struct LainAstOutput LainAstOutput;

/* 输入节点解析：把 AstIn/Source 段的引用读成节点。输出段（3）由驱动自己从
 * AstOut 可信副本回读，不经这里——两条路必须分开，否则驱动会以为自己有映射地址。 */
typedef uint32_t (*LainExpandResolve)(void *, LainAstRef, LainAstNode *);
typedef uint32_t (*LainExpandChild)(void *, LainAstRef, uint64_t, LainAstRef *);

/* 处理器：按 expand.h 的请求/结果帧给出判定，返回 LAIN_EXPAND_* 之一。
 * 返回 REPLACE 时它已经用请求里的 expansion_id 往 AstOut 追加了替换子树。 */
typedef uint64_t (*LainExpandHandler)(void *, const LainExpandRequest *,
                                      LainExpandResult *);

typedef struct {
  uint64_t forms;         /* 驱动访问过的形式数 */
  uint64_t handler_calls; /* 实际问过处理器的次数 */
  uint64_t replacements;  /* 采纳的替换数 */
  uint64_t deferred;      /* 报了 DEFER 的形式数（本轮保持原样） */
  uint64_t errors;
  uint64_t deepest;       /* 到达的最深展开层数 */
  uint64_t nodes_before, nodes_after;
} LainExpandReport;

typedef struct {
  const LainExpandLimits *limits;
  LainExpandResolve resolve;
  void *resolve_context;
  LainExpandChild child;
  void *child_context;
  LainExpandHandler handler;
  void *handler_context;
  LainAstRef root; /* 输入树的根引用（reader 给出的覆盖全文的组） */
} LainExpandRun;

/* 走一遍输入树，把判定落到 AstOut：替换子树逐个提交，未采纳的判定一律回滚到
 * 自己的检查点，最后用重建后的根引用提交最外层事务。返回 0 = 成功。 */
uint32_t lainmeta_expand_drive(LainAstOutput *out, const LainExpandRun *run,
                               LainExpandReport *report);

#endif /* LAINMETA_EXPAND_DRIVER_H */
