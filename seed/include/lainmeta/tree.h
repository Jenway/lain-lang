/* 语言无关的源码树。句柄只在所属树存活期间有效。 */
#ifndef LAINMETA_TREE_H
#define LAINMETA_TREE_H

#include <stdbool.h>
#include <stdint.h>

#include "lainvm/quota.h"

typedef struct LainMetaTree LainMetaTree;

typedef enum {
  LAINMETA_TREE_TOKEN = 1,
  LAINMETA_TREE_GROUP = 2,
} LainMetaTreeKind;

typedef struct {
  LainMetaTreeKind kind;
  uint32_t start;
  uint32_t length;
  uint32_t source_index;
  uint64_t origin;
  unsigned char delimiter; /* 0 为文件根，其他值为开括号 */
  uint32_t child_count;
} LainMetaTreeNode;

/* 读取一份源码并建立词与括号组；跳过 C++ 风格单行注释与 C 风格块注释，
 * 保留 # 等其他符号，不解释关键词或表达式。
 * 失败返回 NULL，error_offset 给出第一个无法配对的括号位置。 */
LainMetaTree *lainmeta_tree_parse(uint32_t source_index, const char *text,
                                  uint32_t length, uint32_t *error_offset);
/* 配额由调用方持有，须活到树销毁；NULL = 不限额。 */
LainMetaTree *lainmeta_tree_parse_with_quota(uint32_t source_index,
                                             const char *text, uint32_t length,
                                             uint32_t *error_offset,
                                             LainVmQuota *quota);
void lainmeta_tree_free(LainMetaTree *tree);
uint64_t lainmeta_tree_root(const LainMetaTree *tree);
bool lainmeta_tree_node(const LainMetaTree *tree, uint64_t handle,
                        LainMetaTreeNode *out);
uint64_t lainmeta_tree_child(const LainMetaTree *tree, uint64_t parent,
                             uint32_t index);
const char *lainmeta_tree_text(const LainMetaTree *tree, uint64_t handle,
                               uint32_t *length_out);

/* 新节点追加到同一棵树；旧节点与旧句柄不变。origin 指向该树的来源节点，
 * 0 表示没有源码来源。失败返回 0。 */
uint64_t lainmeta_tree_make_token(LainMetaTree *tree, const char *text,
                                  uint32_t length, uint64_t origin);
uint64_t lainmeta_tree_make_group(LainMetaTree *tree, unsigned char delimiter,
                                  const uint64_t *children, uint32_t count,
                                  uint64_t origin);
uint64_t lainmeta_tree_replace_child(LainMetaTree *tree, uint64_t group,
                                     uint32_t index, uint64_t replacement);

#endif
