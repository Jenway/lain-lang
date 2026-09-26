/* Meta 宿主的底座服务。
 *
 * 这是「compiler core 提供什么」的最小落地：源码读入 + 产物写出 + 失败上报。
 * 它不含任何语言知识——不知道 `let`、`func`、`i32` 是什么，那些全部归 Meta。
 *
 * **没有全局状态。** host 对象的地址作为一个 `#addr` 显式传进 Meta
 * （`lain_std_initialize(context)`），再原样作为每个能力调用的第一个参数
 * 传回来。理由和能力表里那句一样：宿主 ABI 不带 user_data，因为编译产物
 * 没法读它；让指针变成显式输入，而不是让产物去某个全局里捞。
 *
 * 能力名是 link_name，同时是链接符号名。所以它们必须是**合法 C 标识符**
 * （SYMBOL 策略下后端要按这个名字发外部符号）。这就是这里用下划线而不是
 * 旧 seed 那些 `bootstrap.source-count` 短横线名字的原因。
 */
#ifndef LAINMETA_HOST_H
#define LAINMETA_HOST_H

#include <stdbool.h>
#include <stdint.h>

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

/* 放一份源码。文本按**引用**持有：调用方保证它在 Meta 跑完之前有效。 */
int lainmeta_host_add_source(LainMetaHost *host, const char *path,
                             const char *text, uint32_t length);

uint32_t lainmeta_host_source_count(const LainMetaHost *host);
/* 同一份机制源码树供宿主诊断和 Meta 能力读取。 */
const LainMetaTree *lainmeta_host_tree(LainMetaHost *host, uint32_t source);

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

/* Meta 的可写暂存区。类型注册表、作用域表、语法树都建在这里。
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

/* 把底座服务登记进能力表。返回 0 = 成功。
 * 登记的名字见 host.c 顶部的表。 */
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
 * 暂存区要驱动显式授权是同一件事——能力表里没有 user_data，所以要带的东西
 * 只能挂在**宿主对象自己**身上，而且必须是显式输入。
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
