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

typedef struct LainMetaHost LainMetaHost;

/* 结果码。Meta 侧的失败用 0 以外的值报回来。 */
enum {
  LAINMETA_OK = 0,
  LAINMETA_ERR_NO_SOURCE = 1,   /* 源文件都没放进来 */
  LAINMETA_ERR_OOM = 2,         /* 宿主自己分配失败 */
  LAINMETA_ERR_NO_INTEGER = 3,  /* 源里找不到整数字面量（v0 的语言） */
  LAINMETA_ERR_UNSUPPORTED = 4, /* 这个形状 v0 还不认 */
};

LainMetaHost *lainmeta_host_new(void);
void lainmeta_host_free(LainMetaHost *host);

/* 放一份源码。文本按**引用**持有：调用方保证它在 Meta 跑完之前有效。 */
int lainmeta_host_add_source(LainMetaHost *host, const char *path,
                             const char *text, uint32_t length);

uint32_t lainmeta_host_source_count(const LainMetaHost *host);

/* Meta 写出来的 canonical LAINIR 文本。 */
const char *lainmeta_host_output(const LainMetaHost *host);
uint32_t lainmeta_host_output_length(const LainMetaHost *host);

/* Meta 报回来的失败码；0 = 没失败。 */
uint32_t lainmeta_host_status(const LainMetaHost *host);
void lainmeta_host_clear_status(LainMetaHost *host);

/* 把底座服务登记进能力表。返回 0 = 成功。
 * 登记的名字见 host.c 顶部的表。 */
int lainmeta_host_register(LainMetaHost *host, LainVmCaps *caps);

#endif /* LAINMETA_HOST_H */
