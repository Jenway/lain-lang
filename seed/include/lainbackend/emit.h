/* 后端：把 artifact 变成目标程序。
 *
 * 和 engine 的分工：
 *   engine   决定「每条指令做什么」  产物是状态变化   运行时反复执行   成本正比于运行时间
 *   backend  决定「每条指令变成什么」 产物是另一个程序  运行前恰好一次   成本正比于程序大小
 *
 * 它们是 IR 的两种消除方式：一个把它执行掉，一个把它翻译掉。
 * 唯一共有的是**算子语义**——engine 的 ALU 和后端对每个算子的降级是同一份规范的
 * 两个实现。差分测试只能做在这一层上，因为别处根本没有可比的两份。
 *
 * 输入是 **artifact（L1Module）**，不是映像。映像是 VM 的装载结果、绑定了地址
 * 空间；后端要的是目标无关的那一份。
 *
 * 前置条件：
 *   1. 模块已验证；
 *   2. #eval 已经全部消掉（折叠跑完了）。碰到 is_eval 一律拒绝——那说明折叠漏了。
 *
 * 「拒绝」有两种，别混：engine 拒绝的是**执行**（trap，程序继续被别人处置）；
 * 后端拒绝的是**编译**（目标表达不出来 = 编译失败，根本没有程序）。
 */
#ifndef LAINBACKEND_EMIT_H
#define LAINBACKEND_EMIT_H

#include <stdbool.h>
#include <stdint.h>

#include "lainir/core.h"
#include "lainbackend/target.h"

/* 输出口。后端只往这里写，不管外面是文件、缓冲区还是二进制流。 */
typedef struct {
  /* 代码与数据。可以调多次。 */
  void (*write)(void *user, const char *bytes, uint32_t size);
  /* 引用了一个符号。is_extern 为真表示要外面给（能力落在链接期）。 */
  void (*symbol)(void *user, const char *symbol, bool is_extern);
  void *user;
} LainBackendSink;

typedef struct LainBackend LainBackend;

LainBackend *lainbackend_new(const LainTarget *target,
                             const LainBackendSink *sink, L1Diagnostic *diag);
void lainbackend_free(LainBackend *backend);

/* 编译整个模块。成功返回 0；失败返回非 0，诊断写进构造时给的 diag。 */
int lainbackend_emit(LainBackend *backend, const L1Module *module);

/* 每个后端自己提供自己的目标描述，例如：
 *   const LainTarget *lainbackend_c_target(void);
 * 目标不进这个头文件——通用接口不该知道任何具体目标。 */

/* 写实现之前要先定的三件：
 *
 *   1. **域外行为的检查由谁消去。** 后端发检查（有成本），还是验证器 / Meta
 *      证明它不会发生？没有这条，「无 UB」在编译路径上还不成立。
 *   2. **机器码后端还需要分节和重定位**，现在这个接口里没有表示。
 *   3. **分配与指令选择的策略**没有接口。不必暴露，但要有地方定——
 *      它决定产物质量，也决定同一模块两次编译能不能逐字节一致。
 */

#endif /* LAINBACKEND_EMIT_H */
