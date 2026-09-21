/* #eval 折叠：把「结果在编译期已知」的调用跑出来、替换成字面量。
 *
 * 这是编译器和 VM 之间那条唯一接缝的**驱动方**：
 *   LAINIR  声明（标注在调用上）
 *   LAINVM  执行（引擎就是那个执行者）
 *   这里    跑出来、替进用处、把调用删掉
 *
 * 位置：verify 之后、backend 之前。后端永远看不到 #eval——碰到就拒绝，
 * 那条拒绝正好是这一级的验收条件。
 *
 * 输入是 artifact（`L1Module`），输出是**新模块**：artifact 是 const，不改它。
 * 输出引用输入的名字与类型指针，所以**输入要活得比输出长**。
 *
 * 折叠不是「把调用换成常量定义」，是**常量传播**：结果替进所有用处，
 * 定义本身删掉。文档里那个例子（`%s = #eval table_size()` 之后
 * `#add(%n, %s)` 变成 `#add(%n, 64)`）就是这个意思。
 *
 * 折叠分**两遍**：先折调用形态（`#eval f(...)`），再 lowering 并执行 `#eval { ... }`
 * 块。块要按值捕获外围**编译期已知**的名字，而那些值来自第一遍，所以这个顺序是语义
 * 要求、不是优化。块读一个运行期才知道的名字时没有值可捕获：拒 **9323**，不静默带一个
 * 不存在的 frame 进临时根过程。（码段：9300-9318 调用形态与执行环境，9319-9323 块。）
 */
#ifndef LAINFOLD_FOLD_H
#define LAINFOLD_FOLD_H

#include <stdbool.h>
#include <stdint.h>

#include "lainir/build.h"
#include "lainir/core.h"
#include "lainvm/caps.h"
#include "lainvm/quota.h"

typedef struct LainFold LainFold;

/* 编译期执行的预算与能力由调用方给——和运行期一样，能力必须显式注入：
 * caps 传 NULL 就是一点宿主能力都没有（纯计算的 eval 照样能跑）。 */
LainFold *lainfold_new(LainVmCaps *caps, uint32_t max_call_depth,
                       uint64_t stack_bytes, uint64_t fuel);
void lainfold_free(LainFold *fold);

/* 设这次**编译期执行**的分配预算（字节；0 = 不限额，也是默认）。
 * 账户是执行级的：一次最外层执行一个，嵌套调用共享它，不同模块之间不重置。
 * 扣费点是真正承诺底层存储的地方（栈按整块容量在 admit 时扣一次；宿主暂存区与
 * 输出扩容由 lainmeta_host_attach_quota 挂上同一个账户）。
 * 要在第一次 lainfold_module 之前调用。 */
void lainfold_set_quota_limit(LainFold *fold, uint64_t limit_bytes);

/* 读账目快照（只读，给驱动与验收用）。 */
void lainfold_quota(const LainFold *fold, LainVmQuota *out);

/* 把整个模块里的 #eval 调用跑出来，结果替成字面量，产出新模块。
 * 输入模块不变。失败返回 NULL，诊断写进 diag。 */
const L1Module *lainfold_module(LainFold *fold, L1Builder *builder,
                                const L1Module *module, L1Diagnostic *diag);

/* 折掉了几次。 */
uint32_t lainfold_folded_count(const LainFold *fold);

#endif /* LAINFOLD_FOLD_H */
