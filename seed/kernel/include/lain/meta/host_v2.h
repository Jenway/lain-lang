/* Lain-Meta 宿主层：只给结构化原语，不给内存布局。
 *
 * 宿主只交出 u32 不透明句柄与一小组查询/构造原语，节点字段永远由宿主读，
 * guest 拿到的是值，不存在跨 C 与 Lain 的结构体镜像。
 *
 * 句柄编码（高 4 位类别，低 28 位编号）：
 *   1 = AstIn 节点  ((src_id << 20) | slot)     slot 即 Arena 里的节点下标
 *   2 = AstOut 节点 (节点下标)
 *   3 = Sym         (符号表编号)
 *   4 = IR 模块
 *   5 = IR 区域     ((mod << 20) | (region_index + 1))
 * 源码编号是裸的 0 基序号，与句柄类别不冲突：源码编号只出现在
 * source_root / source_import 的参数与返回里。
 */
#ifndef LAINMETA_HOST_V2_H
#define LAINMETA_HOST_V2_H

#include <stdbool.h>
#include <stdint.h>

#include "lain/meta/apply.h"
#include "lain/meta/expand.h"
#include "lain/meta/sym.h"
#include "lain/vm/caps.h"
#include "lain/vm/quota.h"
#include "lain/vm/space.h"

typedef struct LainMetaHostV2 LainMetaHostV2;

/* 原语返回 0 表示成功，非 0 是稳定诊断码；result_out 只在 0 时有效。 */
enum {
  LAINMETA_V2_OK = 0,
  LAINMETA_V2_ERR_ARG = 9501,       /* 参数个数或取值不合法 */
  LAINMETA_V2_ERR_RANGE = 9502,     /* guest 地址不在授权的可读/可写区段 */
  LAINMETA_V2_ERR_HANDLE = 9503,    /* 句柄类别或编号不被接受 */
  LAINMETA_V2_ERR_STATE = 9504,     /* 当前阶段不允许这个调用 */
  LAINMETA_V2_ERR_CAPACITY = 9505,  /* 宿主侧固定容量用尽 */
  LAINMETA_V2_ERR_SOURCE = 9506,    /* 源码读取或解析失败 */
  LAINMETA_V2_ERR_SYM = 9507,       /* 符号不存在 */
  LAINMETA_V2_ERR_TX = 9508,        /* AstOut 事务状态不符 */
  LAINMETA_V2_ERR_IR = 9509,        /* IR 构造失败 */
  LAINMETA_V2_ERR_APPLY = 9510,     /* apply 拒绝或执行失败 */
  LAINMETA_V2_ERR_MEM = 9511,       /* 工作区分配失败 */
};

#define LAINMETA_V2_MAX_SOURCES 32u
#define LAINMETA_V2_MAX_DIAGS 256u
#define LAINMETA_V2_MAX_MODULES 8u
#define LAINMETA_V2_MAX_MESSAGE 192u

typedef struct {
  uint32_t level;
  uint32_t source;
  uint64_t offset;
  uint64_t length;
  char message[LAINMETA_V2_MAX_MESSAGE];
} LainMetaV2Diag;

LainMetaHostV2 *lainv2_host_new(void);
void lainv2_host_free(LainMetaHostV2 *host);

/* 地址校验与配额：两者都由驱动在登记源码之前接上。 */
void lainv2_host_attach_space(LainMetaHostV2 *host, LainVmSpace *space);
void lainv2_host_attach_quota(LainMetaHostV2 *host, LainVmQuota *quota);
/* 符号表：不调用则用 4096 符号 / 256 KiB 池的默认表。必须在登记源码之前调用。 */
bool lainv2_host_init_symbols(LainMetaHostV2 *host, uint32_t symbols,
                             uint32_t bytes);
/* AstOut 预算；不调用则不开事务注入。 */
bool lainv2_host_init_ast_out(LainMetaHostV2 *host,
                             const LainExpandLimits *limits);
/* apply 预算。 */
void lainv2_host_set_apply_limits(LainMetaHostV2 *host,
                                 const LainMetaApplyLimits *limits);
/* OWNED 工作区：meta_alloc 从这里切。一次分配、不再扩容。 */
bool lainv2_host_alloc_workspace(LainMetaHostV2 *host, uint64_t bytes,
                                uint64_t alignment);
/* 登记一份源码（文本由宿主借用，须活到 host 释放）。成功时 *index_out 是源码编号。 */
bool lainv2_host_add_source(LainMetaHostV2 *host, const char *path,
                           const char *text, uint32_t length,
                           uint32_t *index_out);

/* 宿主侧结果：最终产物文本、诊断、计数。 */
const char *lainv2_host_output(const LainMetaHostV2 *host, uint32_t *length);
uint32_t lainv2_host_diag_count(const LainMetaHostV2 *host);
bool lainv2_host_diag_at(const LainMetaHostV2 *host, uint32_t index,
                        LainMetaV2Diag *out);
const L1Module *lainv2_host_module(const LainMetaHostV2 *host, uint32_t handle);
/* 把全部原语登记成 LAINVM_CAP_FUNCTION。 */
int lainv2_host_register(LainMetaHostV2 *host, LainVmCaps *caps);

#endif /* LAINMETA_HOST_V2_H */
