/* AstOut 与展开共用的一份预算。
 *
 * 深度、访问次数、处理器调用、替换引用、树容量与节点容量都在这里。驱动在
 * 登记能力之前把它交给宿主；AstOut 的每次记账都从这里扣。
 */
#ifndef LAINMETA_EXPAND_H
#define LAINMETA_EXPAND_H

#include <stdint.h>

typedef struct {
  uint64_t max_depth, max_visits, max_handler_calls, max_replacement_refs;
  uint64_t ast_out_capacity, ast_out_node_capacity;
} LainExpandLimits;

#endif /* LAINMETA_EXPAND_H */
