/* LAINVM 的能力空间。
 *
 * 一个能力 = 「这次激活允许调用这个外部符号」。拥有这一项就是许可，不需要额外
 * 的权限位。
 *
 * 键是**外部符号名**（L1Subroutine.link_name）。这个键两边共用：
 *   - 解释器按它去能力空间里查；
 *   - 后端按它发一个外部符号引用。
 * 也就是说 SYMBOL 还是 TRAP 只是「这个键由谁来兑现」的区别，键本身不变。
 *
 * 宿主 ABI 只把 64 位业务原始值给回调：
 *   - 位串：低 width 位有效；地址：宿主地址。
 *   - kind 和 width 在 artifact 里静态已知（调用点声明了参数与结果类型），
 *     装配和还原都不丢信息。
 *   这样嵌入者写**一个** C 函数，解释器和编译产物都能调——否则一条能力
 *   要写两份。
 *
 * **context 是槽上的绑定，不是参数。** 每个能力项带一个 `void *context`，
 * 由可信的 C 注册路径在登记时给出（`lainvm_caps_add`），冻结后不再变；
 * VM 调用时把它注回回调的第一个参数。它不进入 `args`、不占程序槽位、
 * 也不能由 LAINIR 侧指定或读取——所以 Meta 无法用业务参数选择或伪造宿主。
 *
 * **注册—冻结模型。** 表在初始化后是可写的；`lainvm_caps_freeze` 之后禁止
 * 新增和修改绑定，`lainvm_caps_add` 返回专门的冻结错误。冻结是纯策略：本层
 * 从不重分配存储，项地址从初始化起就不再变。冻结**不**管理 context 的
 * 生命周期：绑定的宿主必须活到所有引用该表的 TCB 销毁之后，由可信注册者
 * 负责。能力表不拥有 context。
 *
 * 没有预算：步数、内存、递归上限是**预算**，归 admit 参数与调度上下文。
 */
#ifndef LAINVM_CAPS_H
#define LAINVM_CAPS_H

#include <stdbool.h>
#include <stdint.h>

/* 宿主函数。context 是能力槽上的可信绑定；返回 0 = 成功，非 0 = 拒绝
 * （稳定的诊断码）。result_out 可以为 NULL（这次调用不产出值）。
 *
 * 参数数目是**净化后的业务参数**：原来的裸 context 首参数已经取消。 */
typedef uint32_t (*LainVmHostFn)(void *context, const uint64_t *args,
                                 uint32_t arg_count, uint64_t *result_out);

typedef enum {
  /* 普通宿主函数：调完就回来，帧不保留。 */
  LAINVM_CAP_FUNCTION = 0,
  /* 会合端点：调了可能挂起，帧要保留。（端点对象还没设计；
   * VM 现在碰到这种项会拒绝，而不是静默当成 FUNCTION。） */
  LAINVM_CAP_ENDPOINT = 1,
} LainVmCapKind;

/* 登记 / 冻结的返回码。0 = 成功。 */
typedef enum {
  LAINVM_CAPS_OK = 0,
  LAINVM_CAPS_ERR_ARG = 1,       /* 表、符号名等参数不合法 */
  LAINVM_CAPS_ERR_FULL = 2,      /* 表已满；容量由初始化时给的存储决定 */
  LAINVM_CAPS_ERR_DUPLICATE = 3, /* 同名重复登记 */
  LAINVM_CAPS_ERR_NO_FN = 4,     /* FUNCTION 项缺函数 */
  LAINVM_CAPS_ERR_FROZEN = 5,    /* 表已冻结，禁止新增或修改绑定 */
  LAINVM_CAPS_ERR_NAME = 6,      /* 名字为空或超出 LAINVM_CAPS_NAME_MAX */
} LainVmCapsStatus;

/* 名字缓冲长度（含终止符）。名字被复制进项自带的缓冲：超长拒绝，不截断。 */
#define LAINVM_CAPS_NAME_MAX 64

typedef struct LainVmCapEntry {
  char link_name[LAINVM_CAPS_NAME_MAX];
  LainVmCapKind kind;
  LainVmHostFn fn;
  void *context;
} LainVmCapEntry;

/* 表和它那一排项都由调用方提供：本层一次动态分配都不做，容量由嵌入者选，
 * 所以没有 LAINVM_CAPS_MAX 这种上限——登记满了就是拒绝。 */
typedef struct LainVmCaps {
  LainVmCapEntry *entries; /* 调用方的存储；表不拥有、不释放 */
  uint32_t count;
  uint32_t capacity;
  bool frozen;
} LainVmCaps;

/* 初始化。entries 至少要装下 capacity 项，并且活得比表长；重复初始化会丢弃
 * 原有内容（这里没有任何东西需要释放）。 */
void lainvm_caps_init(LainVmCaps *caps, LainVmCapEntry *entries,
                      uint32_t capacity);

/* 按外部符号名登记，并把可信 context 绑到这一项上。
 * 名字被复制进表自带的缓冲（冻结后不受调用方改字符串影响）。
 * 同名重复登记返回 LAINVM_CAPS_ERR_DUPLICATE，表满返回 LAINVM_CAPS_ERR_FULL；
 * 已冻结返回 LAINVM_CAPS_ERR_FROZEN，且表的内容一个字节都不动。
 * context 可以是 NULL（通用无状态服务）；`lainmeta_host_register` 不接受。 */
int lainvm_caps_add(LainVmCaps *caps, const char *link_name, LainVmCapKind kind,
                    LainVmHostFn fn, void *context);

/* 冻结：成功后不可解冻；再次 freeze 幂等成功；NULL 表失败（非 0）。
 * 冻结之后表的内容、容量与项地址都不再变——TCB 保存的项指针因此长期有效。 */
int lainvm_caps_freeze(LainVmCaps *caps);
bool lainvm_caps_is_frozen(const LainVmCaps *caps);

/* 查一项。**只在装载/admit 时用**——运行期要碰字符串，不许查。 */
const LainVmCapEntry *lainvm_caps_find(const LainVmCaps *caps,
                                       const char *link_name);

LainVmCapKind lainvm_cap_kind(const LainVmCapEntry *entry);
LainVmHostFn lainvm_cap_fn(const LainVmCapEntry *entry);
/* 这一项绑定的可信 context。VM 的宿主调用路径用它，不对外开放成程序值。 */
void *lainvm_cap_context(const LainVmCapEntry *entry);

uint32_t lainvm_caps_count(const LainVmCaps *caps);

/* 活的规则：能力空间必须活得比引用它的 TCB 长，绑定的 context（宿主）也要活得
 * 比引用它的 TCB 长。TCB 在 admit 时把解析结果存成自己的数组，之后不再碰这张表；
 * 但项里的 fn/context 仍按指针使用，所以表不能先于 TCB 释放。
 * 表**不拥有** context，也不为它做引用计数或撤销：这是可信宿主的生命周期契约。 */

#endif /* LAINVM_CAPS_H */
