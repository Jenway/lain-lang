/* 语言无关的源码平铺 Arena（LAINAST v1 的第 1 段：AstIn）。
 *
 * reader 只做机制的事：词法约定（词、字符串、`::`/`->`/`..` 双字节标点）、注释与
 * 括号配对、源码位置、Arena 存储、结构验证。let / func / struct / my_if 的含义不在
 * 这里，全归 Meta。
 *
 * 布局与引用编码的唯一来源是 `lain/meta/ast_v1.h`。这里不给句柄、不给 C 指针：
 * 引用是「段号 + 段内偏移」的整数，节点在映射格式里。
 *
 * 手工测试与驱动都必须用 `lainmeta_tree_arena` 拿视图，禁止再抄一份布局。
 */
#ifndef LAINMETA_TREE_H
#define LAINMETA_TREE_H

#include <stdbool.h>
#include <stdint.h>

#include "lain/meta/ast_v1.h"
#include "lain/vm/quota.h"

typedef struct LainMetaTree LainMetaTree;

/* 解析失败类别。reader 内部本来就知道自己是哪一种失败，必须原样报给宿主：
 * 拿累计的 `quota->rejected` 去猜，会把「这次只是语法错」误判成配额错。
 *
 *   SYNTAX  词法/括号/注释边界不配对：源码可读，位置是第一个说不通的地方
 *   QUOTA   账户余额不够：这一次的分配被账户拒了
 *   ALLOC   宿主自己的 calloc/malloc 失败
 *   INTERNAL 两遍扫描对不上、发布前自检没过：**不是**输入的问题，不冒充前三种
 */
typedef enum {
  LAINMETA_TREE_OK = 0,
  LAINMETA_TREE_ERR_SYNTAX = 1,
  LAINMETA_TREE_ERR_QUOTA = 2,
  LAINMETA_TREE_ERR_ALLOC = 3,
  LAINMETA_TREE_ERR_INTERNAL = 4
} LainMetaTreeStatus;

/* 读取一份源码并建立平铺 Arena；跳过 C++ 单行注释与 C 块注释，保留 # 等其他
 * 符号，不解释关键词或表达式。
 *
 * 两遍扫描：第一遍数节点与孩子引用并检查错误，第二遍按准确预分配的
 * `64 + 56*N + 8*E` 字节填充。两遍共用同一套扫描逻辑，计数与填充不许分家。
 *
 * 失败返回 NULL（不发布半成品），`*status_out` 给出**这一次**的失败类别、
 * error_offset 给出第一个无法配对的括号位置（语法失败时）。status_out 可为 NULL。
 * segment_id 是这份 Arena 绑定的段号，写进 Header 并编码进 root 引用；
 * 宿主视图（lainmeta_tree_arena）可以把真实段号补上。 */
LainMetaTree *lainmeta_tree_parse(uint32_t source_index, const char *text,
                                  uint32_t length, uint32_t *error_offset);
/* 配额由调用方持有，须活到树销毁；NULL = 不限额。Arena 一次计账、一次归还。 */
LainMetaTree *lainmeta_tree_parse_with_quota(uint32_t source_index,
                                             const char *text, uint32_t length,
                                             uint32_t *error_offset,
                                             LainVmQuota *quota);
/* 同上，另外把**本次失败类别**写进 `*status_out`（成功时写 `LAINMETA_TREE_OK`）。
 * 这是宿主唯一该用来区分语法 / 配额 / 分配失败的通道。 */
LainMetaTree *lainmeta_tree_parse_classified(uint32_t source_index,
                                             const char *text, uint32_t length,
                                             uint32_t *error_offset,
                                             LainVmQuota *quota,
                                             LainMetaTreeStatus *status_out);
void lainmeta_tree_free(LainMetaTree *tree);

/* 已建好的平铺 Arena 视图：data/capacity/used 是真实分配，root 是源文件根引用
 *（delimiter=0 的组，覆盖全文）。驱动按 `[data, used)` 只读映射；映射不拥有存储、
 * 不重复扣账，映射期间禁止 free/realloc。
 * 失败返回 false（未建树或参数为空）。 */
bool lainmeta_tree_arena(const LainMetaTree *tree, LainAstArenaView *out);

/* 登记这份 Arena 的绑定段号（驱动在映射前调用），并同时记下**文本所在段的段号**。
 *
 * 两者必须分开：Arena 自己的段号（Header.segment_id、root 引用、孩子列表里的每一项）
 * 和令牌文本的段号（`text`）不是同一段——文本字节在 Source 段，不在 AstIn 段。
 * 两个段号都必须是有效段号（非 0）。 */
bool lainmeta_tree_set_segment(LainMetaTree *tree, uint16_t segment_id,
                               uint16_t text_segment);

#endif /* LAINMETA_TREE_H */
