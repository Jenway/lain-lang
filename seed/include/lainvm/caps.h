/* LAINVM 的能力空间。
 *
 * 一个能力 = 「这次激活允许调用这个外部符号」。**拥有这一项就是许可**，
 * 不需要额外的权限位——microkernel 意义上能力本身就是权限。
 *
 * 键是**外部符号名**（L1Subroutine.link_name）。这个键两边共用：
 *   - 解释器按它去能力空间里查；
 *   - 后端按它发一个外部符号引用。
 * 也就是说 SYMBOL 还是 TRAP 只是「这个键由谁来兑现」的区别，键本身不变。
 *
 * 宿主 ABI 只传 64 位原始值：
 *   - 位串：低 width 位有效；地址：宿主地址。
 *   - kind 和 width 在 artifact 里静态已知（调用点声明了参数与结果类型），
 *     装配和还原都不丢信息。
 *   这样嵌入者写**一个** C 函数，解释器和编译产物都能调——否则一条能力
 *   要写两份。
 *
 * 没有 user_data：要参数化就换一张表绑不同的函数（或者宿主自己用自己的全局，
 * 那是它自己的代码）。多一个上下文指针会让编译产物不得不从一个全局去读它，
 * 那就成了「隐式捕获环境输入」。
 *
 * 没有预算：步数、内存、递归上限是**预算**，归 admit 参数与调度上下文。
 * archive 把能力和预算塞进了同一个结构，这里分开。
 */
#ifndef LAINVM_CAPS_H
#define LAINVM_CAPS_H

#include <stdbool.h>
#include <stdint.h>

/* 宿主函数。返回 0 = 成功，非 0 = 拒绝（稳定的诊断码）。
 * result_out 可以为 NULL（这次调用不产出值）。 */
typedef uint32_t (*LainVmHostFn)(const uint64_t *args, uint32_t arg_count,
                                 uint64_t *result_out);

typedef enum {
  /* 普通宿主函数：调完就回来，帧不保留。 */
  LAINVM_CAP_FUNCTION = 0,
  /* 会合端点：调了可能挂起，帧要保留。（端点对象还没设计；
   * VM 现在碰到这种项会拒绝，而不是静默当成 FUNCTION。） */
  LAINVM_CAP_ENDPOINT = 1,
} LainVmCapKind;

typedef struct LainVmCaps LainVmCaps;
typedef struct LainVmCapEntry LainVmCapEntry;

/* 定长表：内核结构不做动态扩容。 */
#define LAINVM_CAPS_MAX 64u

LainVmCaps *lainvm_caps_new(void);
void lainvm_caps_free(LainVmCaps *caps);

/* 按外部符号名登记。同名重复登记是错误（返回非 0）。 */
int lainvm_caps_add(LainVmCaps *caps, const char *link_name, LainVmCapKind kind,
                    LainVmHostFn fn);

/* 查一项。**只在装载/admit 时用**——运行期要碰字符串，不许查。 */
const LainVmCapEntry *lainvm_caps_find(const LainVmCaps *caps,
                                       const char *link_name);

LainVmCapKind lainvm_cap_kind(const LainVmCapEntry *entry);
LainVmHostFn lainvm_cap_fn(const LainVmCapEntry *entry);

uint32_t lainvm_caps_count(const LainVmCaps *caps);

/* 活的规则：能力空间必须活得比引用它的 TCB 长。
 * （TCB 在 admit 时把解析结果存成自己的数组，之后不再碰这张表；
 * 以后要跨生命周期共享就得加引用计数。） */

#endif /* LAINVM_CAPS_H */
