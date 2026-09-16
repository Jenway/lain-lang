/* LAINVM 映像：装载后的模块。
 *
 * 编译器交付的是 L1Module（artifact）：名字是字符串、符号未解析、布局未定。
 * 映像 = 名字全换成槽、符号已解析、数据地址已烤进去的那份形态。
 * artifact 要保持 canonical，所以预备数据不许塞进它——这就是分开的理由。
 *
 * 映像绑定地址空间（数据地址是烤进去的），换 VSpace 就要重新装载。
 *
 * 定长表：内核结构不做动态扩容；满了是装载失败，不是扩容。
 */
#ifndef LAINVM_IMAGE_H
#define LAINVM_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "lainir/core.h"
#include "lainvm/space.h"

typedef struct LainVmImage LainVmImage;

#define LAINVM_IMAGE_MAX_REGIONS 128u
#define LAINVM_IMAGE_MAX_INSTS 2048u
#define LAINVM_IMAGE_MAX_OPERANDS 8192u
#define LAINVM_IMAGE_MAX_RESULTS 2048u
#define LAINVM_IMAGE_MAX_CASES 512u
#define LAINVM_IMAGE_MAX_SYMBOLS 128u
#define LAINVM_IMAGE_MAX_SUBS 128u

#define LAINVM_IMAGE_NO_REGION 0xFFFFFFFFu
#define LAINVM_IMAGE_NO_INDEX 0xFFFFFFFFu

/* 一个操作数在装载后是什么。
 *
 * 需要帧距离：一个值可以被嵌套区域里的指令引用（内层看得到外层），
 * 而同一个区域会因为递归在多个帧里同时活着。装载器把词法距离算好，
 * 运行时就是两次数组索引。
 * 结果不需要距离——结果总是定义在当前帧。 */
typedef struct {
  bool is_literal;         /* 真：字面量，不进槽，值在 L1Inst.operands 里 */
  uint32_t frame_distance; /* 0 = 当前帧，1 = 外面一层 */
  uint32_t slot;           /* 相对该帧 slot_base 的下标；字面量时为 0 */
} LainVmOperandRef;

typedef struct {
  const L1Region *region;
  /* 过程签名里的参数：它们是体区域自己的入口值，占最前面几个槽。
   * 只有根区域非空。 */
  const L1Param *extra_params;
  uint32_t extra_count;
  uint32_t param_slots; /* 入口参数占的槽数（extra + 区域自己的） */
  uint32_t slot_count;  /* 参数 + 所有指令的结果 */
  uint32_t parent;      /* 父区域；装载时用，运行时不用 */
  uint32_t meta_base;   /* 索引到 insts[] */
  uint32_t inst_count;
  uint32_t init_base;   /* 循环参数初值的操作数引用，索引到 operands[] */
  uint32_t sub_index;   /* 这棵区域树属于哪个子过程（终结子要看签名） */
} LainVmImageRegion;

typedef struct {
  uint32_t operand_base; /* 索引到 operands[] */
  uint32_t result_base;  /* 索引到 results[]；值是相对 slot_base 的下标 */
  uint32_t then_region;
  uint32_t else_region;
  uint32_t loop_region;
  uint32_t case_base; /* 索引到 case_regions[] */
  uint32_t default_region;
  uint32_t symbol_index; /* data_addr 用的符号 */
  uint32_t sub_index;    /* call 的目标子过程 */
} LainVmImageInstMeta;

typedef struct {
  const char *symbol;
  uintptr_t addr;
  uint32_t size;
  uint32_t rights;
} LainVmImageSymbol;

typedef struct {
  const L1Subroutine *sub;
  uint32_t body_region;
  uintptr_t entry_addr; /* 代码段里这条子过程的入口地址 */
} LainVmImageSub;

/* 代码段里每条子过程一条记录。
 * 「地址指向什么」由实现定义——这里是一条运行时记录；将来真做 AOT
 * 就是代码段里的第一条指令。机器上也是同一回事：函数的地址就是它
 * 第一条指令的地址，权限说「这里可以调用」。 */
typedef struct {
  uint32_t sub_index;
  uint32_t reserved;
} LainVmCodeEntry;

struct LainVmImage {
  const L1Module *module;
  LainVmSpace *space;

  /* 模块数据排成两段连续内存：只读段和可写段（.rodata / .data）；
   * 代码排成一段（.text），权限是 READ|CALL，不带 WRITE。 */
  uint8_t *ro_arena;
  uint8_t *rw_arena;
  LainVmCodeEntry *code_arena;

  LainVmImageRegion regions[LAINVM_IMAGE_MAX_REGIONS];
  uint32_t region_count;
  LainVmImageInstMeta insts[LAINVM_IMAGE_MAX_INSTS];
  uint32_t inst_count;
  LainVmOperandRef operands[LAINVM_IMAGE_MAX_OPERANDS];
  uint32_t operand_count;
  uint32_t results[LAINVM_IMAGE_MAX_RESULTS];
  uint32_t result_count;
  uint32_t case_regions[LAINVM_IMAGE_MAX_CASES];
  uint32_t case_count;
  LainVmImageSymbol symbols[LAINVM_IMAGE_MAX_SYMBOLS];
  uint32_t symbol_count;
  LainVmImageSub subs[LAINVM_IMAGE_MAX_SUBS];
  uint32_t sub_count;

  /* admit 用来算上界：一个子过程里最深的词法嵌套、单个区域最多的槽数。 */
  uint32_t max_region_depth;
  uint32_t max_slots;
};

/* 装载。失败返回 NULL，诊断写进 diag（可为 NULL）。
 * 装载器负责把模块数据映射进 space 并登记区段。 */
LainVmImage *lainvm_image_load(const L1Module *module, LainVmSpace *space,
                               L1Diagnostic *diag);
void lainvm_image_free(LainVmImage *image);

const LainVmImageRegion *lainvm_image_region(const LainVmImage *image,
                                             uint32_t region_id);
const L1Inst *lainvm_image_inst(const LainVmImage *image, uint32_t region_id,
                                uint32_t position);
LainVmImageInstMeta lainvm_image_inst_meta(const LainVmImage *image,
                                           uint32_t region_id,
                                           uint32_t position);
LainVmOperandRef lainvm_image_operand_ref(const LainVmImage *image,
                                          uint32_t region_id, uint32_t position,
                                          uint32_t operand_index);
uint32_t lainvm_image_result_slot(const LainVmImage *image, uint32_t region_id,
                                  uint32_t position, uint32_t result_index);

/* 循环区域的第 index 个参数的初值引用。
 * 初值在**父区域**的作用域里求值，所以帧距离是相对执行 #loop 那一帧算的。 */
LainVmOperandRef lainvm_image_param_init_ref(const LainVmImage *image,
                                             uint32_t region_id,
                                             uint32_t param_index);

/* 按名字找入口子过程，同时给出它的体区域。找不到返回 NULL。 */
const L1Subroutine *lainvm_image_entry(const LainVmImage *image,
                                       const char *name,
                                       uint32_t *body_region_out);

uintptr_t lainvm_image_symbol_addr(const LainVmImage *image,
                                   uint32_t symbol_index);

/* 子过程的入口地址（#proc_addr 的结果）。 */
uintptr_t lainvm_image_sub_addr(const LainVmImage *image, uint32_t sub_index);

/* 把代码段里的一个地址换回子过程下标；不是合法入口就返回 NO_INDEX。
 * 越界、没对齐、超出子过程数都算不合法。 */
uint32_t lainvm_image_sub_at(const LainVmImage *image, uintptr_t addr);

#endif /* LAINVM_IMAGE_H */
