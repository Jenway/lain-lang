/* LAINVM 的 Trap：执行失败的分类、状态码与位置。
 *
 * Trap 是拒绝执行的报出机制，不是函数返回值：引擎把它写进 LainVmTrap 后返回
 * LAINVM_SLICE_TRAPPED，由驱动或 fault_handler 处置。域外行为（除数为 0、
 * 移位量 >= 宽度、越权地址、预算耗尽、没验证过的模块）都在这里报出去，
 * 不是 UB，也不需要 IR 里有对应构造。
 *
 * 码只表示原因，不隐含 kind：LAINVM_TRAP_EXECUTION / _CAPABILITY / _STATE
 * 由发射点按当时的情形给。1xxx 号段见 lain/ir/codes.h。
 */
#ifndef LAINVM_TRAP_H
#define LAINVM_TRAP_H

#include <stdbool.h>
#include <stdint.h>

#include "lain/ir/codes.h"

typedef enum {
  LAINVM_TRAP_NONE = 0,
  LAINVM_TRAP_EXECUTION = 1,
  LAINVM_TRAP_CAPABILITY = 2,
  LAINVM_TRAP_STATE = 3,
} LainVmTrapKind;

typedef struct {
  LainVmTrapKind kind;
  int32_t status;
  uint32_t region;   /* 在哪一层区域 */
  uint32_t position; /* 该区域内第几条指令 */
  uint32_t line;
  uint32_t column;
  bool active;
} LainVmTrap;

enum {
  /* 算术与内存的域外行为 */
  LAINVM_TRAP_DIVIDE_BY_ZERO = 1001,
  LAINVM_TRAP_SHIFT_TOO_WIDE = 1002,    /* 移位量 >= 宽度 */
  LAINVM_TRAP_INT_OVERFLOW = 1003,      /* 有符号最小值除以 -1 */
  LAINVM_TRAP_LOAD_FAULT = 1004,        /* 读取的地址不在有 READ 权限的窗口里 */
  LAINVM_TRAP_STORE_FAULT = 1005,       /* 写入的地址不在有 WRITE 权限的窗口里 */
  LAINVM_TRAP_STACK_UNAVAILABLE = 1006, /* 没有栈租约，或租约不属于当前 VSpace */
  LAINVM_TRAP_STACK_EXHAUSTED = 1007,   /* 越过租约容量 */
  LAINVM_TRAP_ALLOCA_OVERFLOW = 1035,   /* 尺寸算术回绕 */
  LAINVM_TRAP_STACK_WINDOW = 1036,      /* 授权窗口同步失败 */
  LAINVM_TRAP_QUOTA = 1044,             /* 分配配额用尽（见 lain/vm/quota.h） */

  /* 结构：模块没验证过，或内部记录对不上 */
  LAINVM_TRAP_STEP_NO_REGION = 1008,
  LAINVM_TRAP_REGION_NO_RESULT = 1009,  /* 区域末尾落下但没有结果 */
  LAINVM_TRAP_FRAME_NO_REGION = 1010,
  LAINVM_TRAP_FRAME_OVERFLOW = 1011,    /* 帧数超过 admit 给的 frame_cap */
  LAINVM_TRAP_SLOT_OVERFLOW = 1012,     /* 值槽超过 slot_cap */
  LAINVM_TRAP_LOCAL_NO_SLOT = 1013,     /* 操作数引用的槽不存在 */
  LAINVM_TRAP_BAD_BITS_OP = 1014,
  LAINVM_TRAP_BAD_COMPARE_OP = 1015,
  LAINVM_TRAP_BAD_CONVERT_OP = 1016,
  LAINVM_TRAP_BAD_FLOAT_OP = 1017,
  LAINVM_TRAP_BAD_FLOAT_WIDTH = 1018,   /* 浮点宽度既不是 32 也不是 64 */
  LAINVM_TRAP_BAD_FLOAT_COMPARE = 1019,
  LAINVM_TRAP_BAD_FLOAT_CONVERT = 1020,
  LAINVM_TRAP_SYMBOL_ADDRESS = 1021,    /* #data_addr 没有符号，或符号地址为 0 */
  LAINVM_TRAP_LOOP_NO_REGION = 1022,
  LAINVM_TRAP_LOOP_PARAMS = 1023,
  LAINVM_TRAP_YIELD_OPERANDS = 1024,
  LAINVM_TRAP_YIELD_RESULTS = 1025,     /* 交值个数与区域声明的结果数不符 */
  LAINVM_TRAP_BREAK_NO_LOOP = 1026,     /* 标签找不到外层循环帧 */
  LAINVM_TRAP_BREAK_OPERANDS = 1027,
  LAINVM_TRAP_BREAK_RESULTS = 1028,
  LAINVM_TRAP_BREAK_NO_SLOT = 1029,     /* 父区域里没有对应的结果槽 */
  LAINVM_TRAP_CONTINUE_NO_LOOP = 1030,
  LAINVM_TRAP_CONTINUE_PARAMS = 1031,
  LAINVM_TRAP_CONTINUE_OPERANDS = 1032,
  LAINVM_TRAP_RETURN_NO_SLOT = 1033,
  LAINVM_TRAP_UNKNOWN_INST = 1034,      /* kind >= INST_COUNT */
  LAINVM_TRAP_PROC_NO_SUB = 1040,       /* #proc_addr 的子过程下标非法 */
  LAINVM_TRAP_PROC_NO_ADDR = 1041,
  LAINVM_TRAP_SWITCH_NO_CASES = 1042,   /* 有 case 但没有 case 表 */
  LAINVM_TRAP_SWITCH_NO_DEFAULT = 1043,
  LAINVM_TRAP_UNIMPLEMENTED = 1099,     /* 算子占位：有 kind 没有实现 */

  /* 调用与能力 */
  LAINVM_TRAP_CALL_NO_SUB = 1100,
  LAINVM_TRAP_CALL_NO_BODY = 1101,      /* 不是 extern 却没有体区域 */
  LAINVM_TRAP_CALL_ARITY = 1102,
  LAINVM_TRAP_CALL_OPERANDS = 1103,
  LAINVM_TRAP_INDIRECT_NO_TARGET = 1104,/* 间接调用没有目标操作数 */
  LAINVM_TRAP_INDIRECT_NO_CALL = 1105,  /* 目标不在有 CALL 权限的区段里 */
  LAINVM_TRAP_INDIRECT_NO_SUB = 1106,
  LAINVM_TRAP_CAP_UNRESOLVED = 1110,    /* 这次激活没解析到这个外部符号 */
  LAINVM_TRAP_CAP_NOT_FUNCTION = 1111,
  LAINVM_TRAP_CAP_NO_FN = 1112,
};

_Static_assert(LAINVM_TRAP_DIVIDE_BY_ZERO > LAINIR_CODES_TRAP_BASE &&
                   LAINVM_TRAP_CAP_NO_FN < LAINIR_CODES_TRAP_LIMIT,
               "trap codes must stay inside the trap segment");

#endif /* LAINVM_TRAP_H */
