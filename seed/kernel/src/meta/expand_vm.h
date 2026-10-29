/* 展开驱动与 VM 的接线：把 Meta 处理器当成沙箱里跑的普通过程。
 *
 * 这一层不决定任何语言形式，只负责一次调用的物理搬运：
 *   - 请求帧的标量按固定顺序绑到入口过程的参数槽（Wire 寄存器）；
 *   - 替换子树由处理器用请求里的 expansion_id 往 AstOut 追加，宿主窗口是现成的；
 *   - 结果帧 48 字节走宿主建立的可写外部映射，处理器用 #store 写进去；
 *   - 一次调用 = 一次独立激活：新建 TCB、跑完即回收。
 *
 * 处理器不得阻塞。真阻塞了不会释放（端点还持有它），只搁置并计入 stats。
 */
#ifndef LAINMETA_EXPAND_VM_H
#define LAINMETA_EXPAND_VM_H

#include <stdbool.h>
#include <stdint.h>

#include "lain/ir/codes.h"
#include "expand_driver.h"
#include "lain/vm/engine.h"
#include "lain/vm/quota.h"
#include "lain/vm/space.h"
#include "lain/vm/tcb.h"

/* 适配器自己的拒绝码，占用 Meta 段尾部，不与 ast_v1.h 的 9401-9413 重号。
 * 这些码只出现在宿主侧（Meta 程序看不到），所以不进跨语言契约。 */
enum {
  LAINMETA_EXPAND_VM_ERR_NO_SPACE = 9430,
  LAINMETA_EXPAND_VM_ERR_NO_WINDOW = 9431,
  LAINMETA_EXPAND_VM_ERR_WINDOW_MAP = 9432,
  LAINMETA_EXPAND_VM_ERR_NO_IMAGE = 9433,
  LAINMETA_EXPAND_VM_ERR_TCB = 9434,
  LAINMETA_EXPAND_VM_ERR_START = 9435,
  LAINMETA_EXPAND_VM_ERR_EXHAUSTED = 9436,
  LAINMETA_EXPAND_VM_ERR_PROTOCOL = 9437,
  LAINMETA_EXPAND_VM_ERR_PARKED_FULL = 9438,
  LAINMETA_EXPAND_VM_ERR_TRAPPED = 9439,
};

_Static_assert(LAINMETA_EXPAND_VM_ERR_NO_SPACE > LAINIR_CODES_META_BASE &&
                   LAINMETA_EXPAND_VM_ERR_TRAPPED < LAINIR_CODES_META_LIMIT,
               "适配器码必须留在 Meta 段内");

/* 参数槽的固定顺序。改这里等于改跨边界契约，Meta 侧要同步。 */
enum {
  LAINMETA_EXPAND_ARG_ABI = 0,
  LAINMETA_EXPAND_ARG_PARENT,
  LAINMETA_EXPAND_ARG_CURSOR,
  LAINMETA_EXPAND_ARG_SYNTAX_ENV,
  LAINMETA_EXPAND_ARG_SCOPE,
  LAINMETA_EXPAND_ARG_CONTEXT_KIND,
  LAINMETA_EXPAND_ARG_EXPANSION_ID,
  LAINMETA_EXPAND_ARG_RESULT_WINDOW,
  LAINMETA_EXPAND_ARG_COUNT,
};

/* 结果窗口的下标（48 字节 = 6 个机器字）。 */
enum {
  LAINMETA_EXPAND_SLOT_STATUS = 0,
  LAINMETA_EXPAND_SLOT_CONSUMED,
  LAINMETA_EXPAND_SLOT_REPLACEMENT,
  LAINMETA_EXPAND_SLOT_REPLACEMENT_COUNT,
  LAINMETA_EXPAND_SLOT_DIAGNOSTIC,
  LAINMETA_EXPAND_SLOT_DEPENDENCY,
  LAINMETA_EXPAND_SLOT_COUNT,
};

_Static_assert(LAINMETA_EXPAND_SLOT_COUNT * 8 == sizeof(LainExpandResult),
               "结果帧与窗口槽数不符");

#define LAINMETA_EXPAND_PARKED_MAX 8u

typedef struct {
  LainVmSpace *space;
  LainVmImage *image; /* 处理器模块，必须活得比本上下文久 */
  LainVmQuota *quota; /* NULL = 不限额 */
  const char *entry;  /* 处理器入口过程名（模块内 link_name） */
  void *window;       /* 宿主侧结果帧缓冲，48 字节，8 字节对齐 */
  uintptr_t window_addr; /* 同一块缓冲在 space 里的地址 */
  LainVmRegionHandle window_handle;
  uint64_t fuel; /* 单次调用的步数预算；0 = 默认 100000 */
  uint32_t max_call_depth;
  uint64_t owner;  /* 写进区段与 TCB 的诊断字段 */
  uint64_t next_id;
  /* 统计 */
  uint64_t calls, trapped, exhausted, blocked, protocol_errors;
  LainVmTcb *parked[LAINMETA_EXPAND_PARKED_MAX];
  uint32_t parked_count;
} LainMetaVmHandlerContext;

/* 把结果窗口映射成可写外部区段并记录地址。成功返回 0。
 * 镜像与入口名不在这里给：它们随调用方装配（处理器的选择是 Meta 程序的事）。 */
uint64_t lainmeta_vm_handler_init(LainMetaVmHandlerContext *context,
                                  LainVmSpace *space, void *window,
                                  uint32_t max_call_depth);
void lainmeta_vm_handler_release(LainMetaVmHandlerContext *context);

/* 符合 LainExpandHandler 的处理器实现，可直接塞进 LainExpandRun。 */
uint64_t lainmeta_vm_handler(void *context, const LainExpandRequest *request,
                             LainExpandResult *result);

#endif /* LAINMETA_EXPAND_VM_H */
