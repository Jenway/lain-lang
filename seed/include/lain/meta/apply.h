/* 单点编译期求值：LAINIR 请求由本层交给 LAINVM 执行。 */
#ifndef LAINMETA_APPLY_H
#define LAINMETA_APPLY_H

/* 通用 apply：入口是模块内的普通 `#proc`，实参是常量值数组。
 * 拒码段 9340–9349；设计与契约见 docs/03-lain-meta.md。 */

#include <stdbool.h>
#include <stdint.h>

#include "lain/ir/core.h"
#include "lain/vm/caps.h"
#include "lain/vm/quota.h"

typedef struct {
  /* NULL = 不授予宿主能力。非 NULL 时**调用方必须已经冻结**这张表；
   * apply 只借用、不修改也不冻结它，表的生命周期由调用方负责。 */
  LainVmCaps *caps;
  uint32_t max_call_depth;
  uint64_t stack_bytes;
  uint64_t fuel;
  uint64_t quota_bytes; /* 0 = 不限额 */
} LainMetaApplyLimits;

typedef struct {
  L1TypeKind kind;
  uint32_t width;
  uint64_t bits;
} LainMetaApplyValue;

/* ---------------------------------------------------------------------------
 * 字节块结果（编译期值里的「不带地址的字节序列」）
 *
 * 由 apply 分配、调用方用 lainmeta_apply_bytes_free 释放；不带地址，按值拷出。
 * 空块用 data == NULL、length == 0 表示。
 *
 * `lainmeta_apply_proc_bytes` 可把 VM 内一段经 VSpace 校验的固定长度范围在执行空间销毁前
 * 复制出来；普通 `lainmeta_apply_proc` 仍只交付标量。Meta 宿主 ABI 尚未接入此结果通道。
 * ------------------------------------------------------------------------- */
#define LAINMETA_APPLY_BYTES_MAX ((uint64_t)1024u * 1024u) /* 1 MiB */

typedef struct {
  uint8_t *data;
  uint64_t length;
} LainMetaApplyBytes;

/* 分配 length 字节的块。length 为 0 或超过 LAINMETA_APPLY_BYTES_MAX 时拒 9345，
 * out 被清零；分配失败写 2028（与其它越界/坏输入一样，不交付部分结果）。 */
bool lainmeta_apply_bytes_alloc(uint64_t length, LainMetaApplyBytes *out,
                           L1Diagnostic *diag);
/* 释放并清零；对空块和 NULL 都是安全的。 */
void lainmeta_apply_bytes_free(LainMetaApplyBytes *bytes);
/* 校验一个字节块：非空、length 不超上限、data/length 一致。
 * 非法时写 9346 并返回 false（不修改 bytes）。 */
bool lainmeta_apply_bytes_check(const LainMetaApplyBytes *bytes, L1Diagnostic *diag);
/* 改动 length，仅用于测试「结构被改坏后仍能被 check 拒掉」；
 * 不重新分配，也不改 data。 */
void lainmeta_apply_bytes_set_len(LainMetaApplyBytes *bytes, uint64_t length);

typedef enum {
  LAINMETA_APPLY_RESULT_NONE = 0,   /* 失败或未交付 */
  LAINMETA_APPLY_RESULT_SCALAR = 1, /* 标量：kind/width/bits */
  LAINMETA_APPLY_RESULT_BYTES = 2,  /* 字节块：由 lainmeta_apply_proc_bytes 交付 */
} LainMetaApplyResultKind;

typedef struct {
  LainMetaApplyResultKind kind;
  LainMetaApplyValue scalar; /* LAINMETA_APPLY_RESULT_SCALAR */
  LainMetaApplyBytes bytes;  /* LAINMETA_APPLY_RESULT_BYTES */
  uint64_t fuel_used;    /* 执行阶段已消耗的 VM 步数；失败结果也保留此计数 */
} LainMetaApplyResult;

/* 输入模块须已验证。操作数是此调用的编译期常量；结果不能是地址。
 * 每次调用建立独立空间和预算。失败写稳定诊断码，不交付部分结果。 */
/* 通用 apply：在独立 VSpace 里执行模块中名为 entry 的普通 `#proc`。
 *
 * 实参按值传，数量与类型必须与过程签名逐项一致，不允许 `#addr` 实参。
 * 结果必须是标量且不含地址；字节结果须显式使用 lainmeta_apply_proc_bytes。
 *
 * 拒码：9340 入口不存在、9341 实参个数不符、9342 实参类型不符、
 *       9343 实参含 #addr、9344 结果含 #addr；执行期沿用折叠段的 93xx。
 * 任何失败都不交付部分值：out 的 kind/scalar/bytes 先清零并保持为空；
 * fuel_used 是计量侧带信息，执行阶段失败时仍报告已经消耗的 VM 步数。
 * limits 为 NULL 时按「不给能力、不限配额、默认栈与调用深度」处理。
 * 错误位置属于生成的模块文本；今天只给码与文本，行列留 0（未知）。 */
bool lainmeta_apply_proc(const char *module_text, const char *entry,
                    const LainMetaApplyValue *args, uint32_t arg_count,
                    const LainMetaApplyLimits *limits, LainMetaApplyResult *out,
                    L1Diagnostic *diag);

/* 显式导出固定长度的按值字节结果。入口必须返回 #addr；地址须指向本次
 * apply 独立 VSpace 中可读的完整范围。宿主在销毁该空间前复制字节，地址本身
 * 不会逃逸。byte_length 为 1..LAINMETA_APPLY_BYTES_MAX，否则拒 9345。 */
bool lainmeta_apply_proc_bytes(const char *module_text, const char *entry,
                          const LainMetaApplyValue *args, uint32_t arg_count,
                          uint64_t byte_length,
                          const LainMetaApplyLimits *limits, LainMetaApplyResult *out,
                          L1Diagnostic *diag);

/* 将一份调用方持有的字节块作为只读 #addr 实参映射到本次 apply 的独立 VSpace。
 * args[byte_arg_index] 必须标记 TY_ADDR；其余实参仍按标量逐项检查。字节块在
 * 执行前复制，计入 apply 配额，执行结束撤销映射并释放副本。长度超限拒 9345，
 * 空块或坏字节块拒 9346，指向非 #addr 形参拒 9342，额外 #addr 参数仍拒 9343，
 * 映射资源不足拒 9348；配额不足沿用 1044。 */
bool lainmeta_apply_proc_with_bytes_arg(
    const char *module_text, const char *entry, const LainMetaApplyValue *args,
    uint32_t arg_count, uint32_t byte_arg_index,
    const LainMetaApplyBytes *byte_arg, const LainMetaApplyLimits *limits,
    LainMetaApplyResult *out, L1Diagnostic *diag);

#endif
