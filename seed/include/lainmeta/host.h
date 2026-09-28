/* Meta 宿主的底座服务。
 *
 * 这是「compiler core 提供什么」的最小落地：源码读入 + 产物写出 + 失败上报。
 * 它不含任何语言知识——不知道 `let`、`func`、`i32` 是什么，那些全部归 Meta。
 *
 * **没有全局状态。** host 对象**不再作为 #addr 传进 Meta**：可信的 C 注册路径
 * （`lainmeta_host_register`）把它绑到每个能力槽的 context 上，LAINVM 在宿主调用时
 * 注入回调的第一个参数。Meta 只看得到净化后的业务参数，无法指定或伪造宿主。
 *
 * 能力名是 link_name，同时是链接符号名。所以它们必须是**合法 C 标识符**
 * （SYMBOL 策略下后端要按这个名字发外部符号）。这就是这里用下划线而不是
 * 旧 seed 那些 `bootstrap.source-count` 短横线名字的原因。
 */
#ifndef LAINMETA_HOST_H
#define LAINMETA_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "lainmeta/ast_v1.h"
#include "lainmeta/expand.h"
#include "lainvm/caps.h"
#include "lainvm/quota.h"
#include "lainvm/space.h"
#include "laineval/eval.h"

typedef struct LainMetaHost LainMetaHost;
typedef struct LainMetaTree LainMetaTree;

/* 结果码。Meta 侧的失败用 0 以外的值报回来。 */
enum {
  LAINMETA_OK = 0,
  LAINMETA_ERR_NO_SOURCE = 1,   /* 源文件都没放进来 */
  LAINMETA_ERR_OOM = 2,         /* 宿主自己分配失败 */
  LAINMETA_ERR_NO_INTEGER = 3,  /* 源里找不到整数字面量（v0 的语言） */
  LAINMETA_ERR_UNSUPPORTED = 4, /* 这个形状 v0 还不认 */
  LAINMETA_ERR_DENIED = 5,      /* 宿主边界：没授权，或要读的区间不在授权范围内 */
  LAINMETA_ERR_TREE_PARSE = 34,
  LAINMETA_ERR_TREE_HANDLE = 35,
  LAINMETA_ERR_TREE_EDIT = 36,
  LAINMETA_ERR_TYPE_PUBLISH = 37,
  LAINMETA_ERR_RESOURCE_PUBLISH = 38,
};

/* 未知来源及位置为 UINT64_MAX；offset 是零起点字节偏移，允许文件末尾。 */
typedef struct {
  uint32_t code;
  uint64_t source, offset;
} LainMetaDiagnostic;
int lainmeta_host_diagnostic(const LainMetaHost *host, LainMetaDiagnostic *out);

/* Meta 发布给宿主调试器的类型摘要。身份及参数区间由 Meta 定义；宿主只保存
 * 八个公开字段，不读取 Meta 的内部注册表。 */
typedef struct {
  uint64_t id, kind, repr_kind, repr_width;
  uint64_t owner, namespace_id, arg_start, arg_count;
} LainMetaTypeInfo;

LainMetaHost *lainmeta_host_new(void);
void lainmeta_host_free(LainMetaHost *host);

/* 第二期物理服务：在 TCB 启动前一次配置，Arena 及可信副本均计入配额。
 * 驱动将 view.data 按完整 capacity 映射 RW；used 仍是宿主权威水位。
 * 必须在 register 之前配置；不配置时不登记八项写树/预算能力，
 * 第一期开出的只读能力集合保持不变。 */
uint32_t lainmeta_host_enable_ast_out(LainMetaHost *, const LainExpandLimits *);
bool lainmeta_host_ast_out(const LainMetaHost *, LainAstArenaView *);

/* 放一份源码。文本按**引用**持有：调用方保证它在 Meta 跑完之前有效。 */
int lainmeta_host_add_source(LainMetaHost *host, const char *path,
                             const char *text, uint32_t length);

uint32_t lainmeta_host_source_count(const LainMetaHost *host);
/* 同一份机制源码树（平铺 AstIn Arena）供宿主诊断和 Meta 能力读取。
 * 树在登记源码时**立即**建好：第一期不允许依赖「Meta 首次访问触发懒解析」——
 * 驱动要在装配 TCB 之前就把全部 AstIn 映射完。 */
const LainMetaTree *lainmeta_host_tree(LainMetaHost *host, uint32_t source);

/* 第 index 份源码的 AstIn Arena 视图。驱动按 `[data, used)` 只读映射，
 * 映射长度用 used（published 水位），capacity 是真实分配。
 * 失败返回 false（越界或尚未建树）。 */
bool lainmeta_host_arena(const LainMetaHost *host, uint32_t source,
                         LainAstArenaView *out);

/* 撤销某个 AstIn 段（驱动 unmap 之后调用）。撤销后该段的一切引用解析都拒 9401；
 * 宿主自己仍持有树用于诊断。退出顺序：TCB 销毁 → 撤销区域 → 撤销段 → 释放宿主。 */
void lainmeta_host_revoke_tree(LainMetaHost *host, uint32_t source);

/* 段的分配（规范 §1.2）：主源码 Source=1、AstIn=2；3 是 AstOut 的保留编号，
 * 第一期没有消费者也不许被源码占用；额外源码按**注册顺序**成对发 4/5、6/7、……
 * 编号在任务存活期不回收、不复用，失败源码也占住它的段号对。 */
uint16_t lainmeta_host_source_segment(const LainMetaHost *host,
                                      uint32_t source);
uint16_t lainmeta_host_tree_segment(const LainMetaHost *host, uint32_t source);

/* 引用解析：属性里的每一项先在本段内校验（段存活、Header 合法、节点已发布、
 * 区间不越界），通过后再交给 VSpace。失败返回 false 并把固定拒码记进 status
 * （9401 段 / 9402 Header / 9403 槽位 / 9404 范围）。
 *
 * `lainmeta_host_ast_node_addr` 只接受 AST 段的**节点**引用；
 * `lainmeta_host_ast_span_addr` 接受 Source 或 AST 段的**字节**引用——
 * 两者不许互相代用（节点引用不是字节引用）。
 * `lainmeta_host_ast_child_ref` 是 `child(cursor)` 的宿主实现：能力只能返回整数，
 * 由它把「本段孩子列表第 i 项」解析成引用，Meta 永远不需要自己解码引用。 */
bool lainmeta_host_ast_node_addr(LainMetaHost *host, LainAstRef ref,
                                 uintptr_t *address_out);
bool lainmeta_host_ast_span_addr(LainMetaHost *host, LainAstRef ref,
                                 uint64_t length, uintptr_t *address_out);
bool lainmeta_host_ast_child_ref(LainMetaHost *host, LainAstRef ref,
                                 uint64_t index, LainAstRef *out);

/* Meta 侧诊断通道：`%slot` 0..15，`%value` 原样存下。**故意不落暂存区** ——
 * Meta 的暂存区格子会被分配器与实参表覆盖，把诊断写在那里会读到全 0，
 * 得出「这段代码没跑」这种错误结论（实测踩过两次）。这里写的是宿主结构里的字段，
 * 分配器碰不到，驱动在跑失败后读出来即可。 */
#define LAINMETA_TRACE_SLOTS 64
/* 诊断：最近一次节点引用解析失败的引用与拒码，以及失败次数。 */
void lainmeta_host_trace_bad(const LainMetaHost *host, uint64_t *ref_out,
                             uint32_t *code_out, uint32_t *count_out);
/* 诊断：被拒（DENIED=5）的次数。 */
uint32_t lainmeta_host_deny_count(const LainMetaHost *host);
/* 读回 Meta 写下的诊断槽。越界返回 0。 */
uint64_t lainmeta_host_trace_slot(const LainMetaHost *host, uint32_t slot);
/* 诊断：宿主自己把 status 设成 5 的次数（与 Meta 报上来的 5 分开）。 */
uint32_t lainmeta_host_status5_count(const LainMetaHost *host);
/* 诊断：emit_write 的环形缓冲（最近 32 次调用的 addr/size）。失败时用来
 * 看「失败前那一次的准确入参」——不依赖 L1 侧配对，也不会互相覆盖。 */
#define LAINMETA_EMIT_RING 32
uint32_t lainmeta_host_emit_ring(const LainMetaHost *host, uint32_t index,
                                 uint64_t *addr_out, uint64_t *size_out);
/* 诊断：最近一次把 status 设成 5 时的 bad_count 与返回地址。 */
void lainmeta_host_status5_site(const LainMetaHost *host, uint64_t *a, uint64_t *b);

/* 第 index 份源码的**逻辑路径**（NUL 结尾）。这是 import 解析的注册表：
 * `import("std::math")` 规范化成 `std/math.lain` 之后和它逐字节比较。
 *
 * 返回的是宿主的地址，不在 Meta 的映像里——驱动要用它就得先授权，
 * 和源码文本、暂存区是同一个规矩。越界返回 ""。 */
const char *lainmeta_host_source_path(const LainMetaHost *host,
                                      uint32_t index);

/* 第 index 份源码的文本地址与字节数（读到 length_out）。同样要驱动授权。 */
const char *lainmeta_host_source_text(const LainMetaHost *host, uint32_t index,
                                      uint32_t *length_out);

/* Meta 的可写暂存区。类型注册表、作用域表、语义旁表建在这里；AstIn 独立只读映射。
 *
 * 大小**由编译单元决定**（已登记源码的总字节数），所以是**按需分配**的：要等
 * 源码都登记完再拿，不是创建 host 的时候。
 * 由**驱动显式授权**：Meta 的 TCB 不自带地址空间，这块内存不是它映像的
 * 一部分。不给授权就别想写——和源码只读那次是同一个道理。
 * 返回 NULL 表示宿主分配失败。 */
void *lainmeta_host_scratch(LainMetaHost *host, uint32_t *size_out);
/* Meta 主动报告的暂存区最高使用位置；未报告时返回 0，不修改输出。 */
int lainmeta_host_scratch_peak(const LainMetaHost *host, uint64_t *out);

/* Meta 写出来的 canonical LAINIR 文本。 */
const char *lainmeta_host_output(const LainMetaHost *host);
uint32_t lainmeta_host_output_length(const LainMetaHost *host);

uint32_t lainmeta_host_type_count(const LainMetaHost *host);
int lainmeta_host_type_at(const LainMetaHost *host, uint32_t index,
                          LainMetaTypeInfo *out);

/* Meta 报回来的失败码；0 = 没失败。 */
uint32_t lainmeta_host_status(const LainMetaHost *host);
void lainmeta_host_clear_status(LainMetaHost *host);

/* 把底座服务登记进能力表，并把 host 绑为每一项的 context。返回 0 = 成功。
 * 登记的名字见 host.c 顶部的表。表必须尚未冻结；冻结表或重名会失败，
 * 此时调用方不能使用半注册的任务。 */
int lainmeta_host_register(LainMetaHost *host, LainVmCaps *caps);

/* 挂上这次执行的**分配账户**（NULL = 不限额）。
 *
 * 暂存区与输出缓冲的扩容都是"实际承诺一块底层存储"，所以要先过账户：余额不够时
 * 暂存区返回 NULL、输出扩容保持原样，状态码记 LAINVM_QUOTA_TRAP（1044）；
 * 释放宿主时按已计账字节归还。必须在拿暂存区或通用源码树之前调用。 */
void lainmeta_host_attach_quota(LainMetaHost *host, LainVmQuota *quota);

/* 把这份宿主服务**授权**到某个地址空间（驱动在 admit 之前调用）。
 *
 * 宿主能力只拿得到裸地址（宿主 ABI 没有类型），所以"这个地址能不能读"必须由
 * 宿主这一侧自己判：没授权的宿主对象不许解引用调用方给的地址。这和源码文本、
 * 暂存区要驱动显式授权是同一件事——**context 绑定解决的是"由哪个宿主兑现能力"，
 * 不替代业务缓冲权限**。授权挂在宿主对象自己身上，由驱动显式给。
 *
 * 传 NULL 表示撤销授权。 */
void lainmeta_host_attach_space(LainMetaHost *host, LainVmSpace *space);

/* Meta 的 Eval 请求使用独立的执行环境；默认只允许纯计算。
 * 驱动可显式设置预算与授予的能力，设置须在运行 Meta 前完成。 */
void lainmeta_host_set_eval_limits(LainMetaHost *host,
                                   const LainEvalLimits *limits);

/* 当前编译请求发给 Eval 服务的次数，供驱动与专项验收核对实际调用路径。 */
uint32_t lainmeta_host_eval_requests(const LainMetaHost *host);
/* 最近失败的 Eval 请求诊断；行列属于请求文本。无失败时不修改输出。 */
int lainmeta_host_eval_diagnostic(const LainMetaHost *host, L1Diagnostic *out);

#endif /* LAINMETA_HOST_H */
