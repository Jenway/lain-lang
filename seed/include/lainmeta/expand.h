/* Expand v1 的调用帧协议与任务预算；不携带语言关键字或宿主地址。 */
#ifndef LAINMETA_EXPAND_H
#define LAINMETA_EXPAND_H
#include "lainmeta/ast_v1.h"

typedef struct {
  uint64_t abi_version;
  LainAstRef parent;
  uint64_t cursor, syntax_env, scope, context_kind, expansion_id;
} LainExpandRequest;
typedef struct {
  uint64_t status, consumed;
  LainAstRef replacement;
  uint64_t replacement_count, diagnostic, dependency;
} LainExpandResult;
enum { LAIN_EXPAND_NO_MATCH, LAIN_EXPAND_REPLACE,
       LAIN_EXPAND_DEFER, LAIN_EXPAND_ERROR };
typedef struct {
  uint64_t max_depth, max_visits, max_handler_calls, max_replacement_refs;
  uint64_t ast_out_capacity, ast_out_node_capacity;
} LainExpandLimits;
_Static_assert(sizeof(LainExpandRequest) == 56, "展开请求 56 字节");
_Static_assert(sizeof(LainExpandResult) == 48, "展开结果 48 字节");
_Static_assert(offsetof(LainExpandRequest, expansion_id) == 48, "请求字段偏移");
_Static_assert(offsetof(LainExpandResult, dependency) == 40, "结果字段偏移");
_Static_assert(offsetof(LainExpandRequest, parent) == 8, "parent 偏移");
_Static_assert(offsetof(LainExpandRequest, cursor) == 16, "cursor 偏移");
_Static_assert(offsetof(LainExpandRequest, syntax_env) == 24, "syntax_env 偏移");
_Static_assert(offsetof(LainExpandRequest, scope) == 32, "scope 偏移");
_Static_assert(offsetof(LainExpandRequest, context_kind) == 40, "context_kind 偏移");
_Static_assert(offsetof(LainExpandResult, consumed) == 8, "consumed 偏移");
_Static_assert(offsetof(LainExpandResult, replacement) == 16, "replacement 偏移");
_Static_assert(offsetof(LainExpandResult, replacement_count) == 24, "replacement_count 偏移");
_Static_assert(offsetof(LainExpandResult, diagnostic) == 32, "diagnostic 偏移");
#endif
