/* 单点编译期求值：LAINIR 请求由本层交给 LAINVM 执行。 */
#ifndef LAINAPPLY_APPLY_H
#define LAINAPPLY_APPLY_H

/* 通用 apply（步骤 3）：入口是模块内的普通 `#proc`，实参是常量值数组。
 * 拒码段 9340–9349；设计与契约见 docs/compile-time.md。 */

#include <stdbool.h>
#include <stdint.h>

#include "lainir/core.h"
#include "lainvm/caps.h"
#include "lainvm/quota.h"

typedef struct {
  /* NULL = 不授予宿主能力。非 NULL 时**调用方必须已经冻结**这张表；
   * apply/fold 只借用、不修改也不冻结它，表的生命周期由调用方负责。 */
  LainVmCaps *caps;
  uint32_t max_call_depth;
  uint64_t stack_bytes;
  uint64_t fuel;
  uint64_t quota_bytes; /* 0 = 不限额 */
} LainApplyLimits;

typedef struct {
  L1TypeKind kind;
  uint32_t width;
  uint64_t bits;
} LainApplyValue;

/* ---------------------------------------------------------------------------
 * 字节块结果（编译期值里的「不带地址的字节序列」）
 *
 * 由 apply 分配、调用方用 lainapply_bytes_free 释放；不带地址，按值拷出。
 * 空块用 data == NULL、length == 0 表示。
 *
 * **第一版只提供类型与分配/释放/校验**：从 VM 内存导出字节块的路径尚未实现，
 * 因此 lainapply_proc 今天只会交付 LAINAPPLY_RESULT_SCALAR（见 docs/compile-time.md）。
 * ------------------------------------------------------------------------- */
#define LAINAPPLY_BYTES_MAX ((uint64_t)1024u * 1024u) /* 1 MiB */

typedef struct {
  uint8_t *data;
  uint64_t length;
} LainApplyBytes;

/* 分配 length 字节的块。length 为 0 或超过 LAINAPPLY_BYTES_MAX 时拒 9345，
 * out 被清零；分配失败写 2028（与其它越界/坏输入一样，不交付部分结果）。 */
bool lainapply_bytes_alloc(uint64_t length, LainApplyBytes *out,
                           L1Diagnostic *diag);
/* 释放并清零；对空块和 NULL 都是安全的。 */
void lainapply_bytes_free(LainApplyBytes *bytes);
/* 校验一个字节块：非空、length 不超上限、data/length 一致。
 * 非法时写 9346 并返回 false（不修改 bytes）。 */
bool lainapply_bytes_check(const LainApplyBytes *bytes, L1Diagnostic *diag);
/* 改动 length，仅用于测试「结构被改坏后仍能被 check 拒掉」；
 * 不重新分配，也不改 data。 */
void lainapply_bytes_set_len(LainApplyBytes *bytes, uint64_t length);

typedef enum {
  LAINAPPLY_RESULT_NONE = 0,   /* 失败或未交付 */
  LAINAPPLY_RESULT_SCALAR = 1, /* 标量：kind/width/bits */
  LAINAPPLY_RESULT_BYTES = 2,  /* 字节块：第一版未实现（9345 之外无路径） */
} LainApplyResultKind;

typedef struct {
  LainApplyResultKind kind;
  LainApplyValue scalar; /* LAINAPPLY_RESULT_SCALAR */
  LainApplyBytes bytes;  /* LAINAPPLY_RESULT_BYTES */
} LainApplyResult;

/* 输入模块须已验证。操作数是此调用的编译期常量；结果不能是地址。
 * 每次调用建立独立空间和预算。失败写稳定诊断码，不交付部分结果。 */
bool lainapply_call(const L1Module *module, const L1Inst *inst,
                    const L1Operand *operands, const LainApplyLimits *limits,
                    LainApplyValue *out, L1Diagnostic *diag);
/* 可选执行账目快照，在执行资源释放后写入；提前拒绝时为零账目。 */
bool lainapply_call_report(const L1Module *module, const L1Inst *inst,
                           const L1Operand *operands, const LainApplyLimits *limits,
                           LainApplyValue *out, L1Diagnostic *diag,
                           LainVmQuota *quota_out);

/* 完整 LAINIR 文本中的 entry 是查询包装过程：其体仅有一条带结果的
 * #eval 调用，接着 #return 该结果。entry 名字指定唯一求值位置。 */
bool lainapply_text(const char *text, const char *entry,
                    const LainApplyLimits *limits, LainApplyValue *out,
                    L1Diagnostic *diag);

/* 通用 apply：在独立 VSpace 里执行模块中名为 entry 的普通 `#proc`。
 *
 * 实参按值传，数量与类型必须与过程签名逐项一致，不允许 `#addr` 实参。
 * 结果必须是标量且不含地址；字节块路径第一版未实现。
 *
 * 拒码：9340 入口不存在、9341 实参个数不符、9342 实参类型不符、
 *       9343 实参含 #addr、9344 结果含 #addr；执行期沿用折叠段的 93xx。
 * 任何失败都不交付部分结果：out 先被清零，失败后仍是空的。
 * limits 为 NULL 时按「不给能力、不限配额、默认栈与调用深度」处理。
 * 错误位置属于生成的模块文本；今天只给码与文本，行列留 0（未知）。 */
bool lainapply_proc(const char *module_text, const char *entry,
                    const LainApplyValue *args, uint32_t arg_count,
                    const LainApplyLimits *limits, LainApplyResult *out,
                    L1Diagnostic *diag);

#endif
