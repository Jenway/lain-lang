/* 全局符号驻留表：相同文本永远拿到同一个 SymId，Meta 因此可以按符号而不是
 * 按字节扫描做匹配。
 *
 * 表在创建时**一次性**分配存储：字节池 + 偏移表 + 长度表 + 哈希桶连成一次 quota
 * 扣账，之后不再扩容。宿主把字节池整块只读映射给 VM，驻留期间追加内容不会让已
 * 映射的地址失效。容量用满就拒绝（返回 0），不做翻倍增长。
 *
 * SymId 从 1 起；0 表示「没有符号」，永远不会被驻留出来。
 */
#ifndef LAIN_META_SYM_H
#define LAIN_META_SYM_H

#include <stdbool.h>
#include <stdint.h>

#include "lain/vm/quota.h"

typedef uint32_t LainMetaSymId;

#define LAIN_META_SYM_NONE 0u
#define LAIN_META_SYM_MAX UINT32_C(0x00FFFFFF)

typedef struct LainMetaSymTab LainMetaSymTab;

/* 创建表。capacity 是符号条数上限，bytes_capacity 是字节池容量；两者都必须非 0。
 * quota 可为 NULL（不限额），须活到表销毁。失败返回 NULL（存储被账户拒绝或
 * malloc 失败，两者对宿主都是「没有表可用」）。 */
LainMetaSymTab *lainmeta_sym_new(uint32_t capacity, uint32_t bytes_capacity,
                                 LainVmQuota *quota);
void lainmeta_sym_free(LainMetaSymTab *tab);

/* 驻留一段文本并返回它的 SymId。已存在则返回原 id；容量或字节池不足返回 0。 */
LainMetaSymId lainmeta_sym_intern(LainMetaSymTab *tab, const void *text,
                                  uint32_t length);
/* 只查不插入。查不到返回 0。 */
LainMetaSymId lainmeta_sym_find(const LainMetaSymTab *tab, const void *text,
                                uint32_t length);

/* 取一段已驻留文本。bytes 指向表内字节池（映射期间有效），length 是字节数。
 * 未知 id 返回 false。 */
bool lainmeta_sym_bytes(const LainMetaSymTab *tab, LainMetaSymId sym,
                        const unsigned char **bytes, uint32_t *length);

uint32_t lainmeta_sym_count(const LainMetaSymTab *tab);

/* 字节池的映射视图：宿主启动时按 (pool, capacity) 映射一次。pool 在表销毁前
 * 不移动。 */
const unsigned char *lainmeta_sym_pool(const LainMetaSymTab *tab,
                                       uint32_t *capacity, uint32_t *used);

#endif /* LAIN_META_SYM_H */
