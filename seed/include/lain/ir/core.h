/* LAINIR 物理数据模型：只有数据，没有函数、全局状态或分配。
 *
 * 判据是「机器上怎么做」——表达式、goto/标签跳转、可变绑定、unit/never、
 * trap、隐式 flags、上下文类型、调用约定、内存管理都不进模型，所以没有
 * 对应字段。
 */
#ifndef LAINIR_CORE_H
#define LAINIR_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * 类型
 *
 * 类型只描述存储与搬运：搬多少字节、这些位怎么解释。不含符号——符号在算子上
 * （#sdiv/#udiv、#slt/#ult、#zext/#sext/#trunc）。
 *
 * 没有复合类型：数组、结构体、元组的布局完全是 Meta 的事，LAINIR 只看见
 * 字节和偏移。
 * ------------------------------------------------------------------------- */
typedef enum {
  TY_BITS,   /* #bits(N)   整数解释 */
  TY_FLOATS, /* #f(F)      IEEE 格式 F；width 存格式的位数 */
  TY_VEC,    /* #vec(N)    打包车道解释；车道分解在算子上 */
  TY_ADDR,   /* #addr      地址；width 无意义 */
} L1TypeKind;

typedef struct {
  L1TypeKind kind;
  uint32_t width; /* TY_ADDR 无意义 */
} L1Type;

/* ---------------------------------------------------------------------------
 * 内存序
 *
 * 写在内存操作上。不写 = 该目标最弱。
 * volatile 是访问的一个属性（面向 MMIO：这条访问不能被动），不是序。
 * ------------------------------------------------------------------------- */
typedef enum {
  ORDER_RELAXED,
  ORDER_ACQUIRE,
  ORDER_RELEASE,
  ORDER_ACQ_REL,
  ORDER_SEQ_CST,
} L1MemOrder;

/* ---------------------------------------------------------------------------
 * 操作数
 *
 * 字面量不带类型：它的宽度就是所在指令的类型实参，只要在那个宽度里表示得出来。
 * ------------------------------------------------------------------------- */
typedef enum {
  OPERAND_VALUE, /* 引用一个值 */
  OPERAND_INT,   /* 整数字面量，按指令的类型实参解释 bits */
  OPERAND_FLOAT, /* 浮点字面量，bits 是该方法格式的位模式 */
} L1OperandKind;

typedef struct {
  L1OperandKind kind;
  const char *name; /* OPERAND_VALUE */
  uint64_t bits;    /* 字面量 */
} L1Operand;

/* ---------------------------------------------------------------------------
 * 指令集合
 *
 * 形式：
 *   %name = #op[TYPE](operands)        产出值
 *   %a, %b = #op[TYPE](operands)       产出多个值
 *   #op[TYPE](operands)                不产出值
 *
 * 结果类型不存进模型，由算子推出——见 lainir_inst_result_type。
 * ------------------------------------------------------------------------- */
typedef enum {
  /* 整数算术 */
  INST_ADD, INST_SUB, INST_MUL, INST_SDIV, INST_UDIV, INST_SREM, INST_UREM,
  /* 位运算 */
  INST_AND, INST_OR, INST_XOR, INST_SHL, INST_LSHR, INST_ASHR,
  /* 浮点算术 */
  INST_FADD, INST_FSUB, INST_FMUL, INST_FDIV,
  /* 整数比较；结果 #bits<1> */
  INST_EQ, INST_NE, INST_SLT, INST_SLE, INST_SGT, INST_SGE,
  INST_ULT, INST_ULE, INST_UGT, INST_UGE,
  /* 浮点比较；有序。NaN 是不可区分的状态，所以有序/无序必须分家 */
  INST_FOEQ, INST_FONE, INST_FOLT, INST_FOLE, INST_FOGT, INST_FOGE,
  /* 浮点比较；无序 */
  INST_FUEQ, INST_FUNE, INST_FULT, INST_FULE, INST_FUGT, INST_FUGE,
  /* 整数转换：源宽度取操作数，类型实参是目标宽度 */
  INST_ZEXT, INST_SEXT, INST_TRUNC, INST_BITCAST,
  /* 浮点转换 */
  INST_FPEXT, INST_FPTRUNC, INST_FPTOSI, INST_FPTOUI, INST_SITOFP, INST_UITOFP,
  /* 地址转换 */
  INST_INT2PTR, INST_PTR2INT,
  /* 向量；车道分解在算子名上，模型里用 (kind, lane_bits) 表示，见 L1Inst.lane_bits */
  INST_VADD, INST_VSUB, INST_VMUL, INST_VDIV, INST_VAND, INST_VOR, INST_VXOR,
  INST_VSHL, INST_VSHUFFLE, INST_VBROADCAST, INST_VEXTRACT, INST_VINSERT,
  INST_VCMPEQ, INST_VCMPNE, INST_VCMPLT, INST_VCMPGT,
  /* 原子 */
  INST_XCHG, INST_CMPXCHG, INST_RMW_ADD, INST_RMW_SUB,
  INST_RMW_AND, INST_RMW_OR, INST_RMW_XOR,
  /* 取址：base + idx*scale + offset */
  INST_LEA,
  /* 内存 */
  INST_LOAD, INST_STORE, INST_ALLOCA,
  /* 引用 */
  INST_DATA_ADDR,
  /* 取子过程的地址。产出的是 #addr，和 #data_addr 同形：
   * 地址没有类型，能不能调用由它所在区段的权限说话。 */
  INST_PROC_ADDR,
  /* 调用 */
  INST_CALL, INST_CALL_INDIRECT,
  /* 结构：每条拥有自己的区域 */
  INST_IF, INST_LOOP, INST_SWITCH,
  /* 终结子 */
  INST_YIELD, INST_BREAK, INST_CONTINUE, INST_RETURN,

  /* 计数用；不是一条指令 */
  INST_COUNT,
} L1InstKind;

/* ---------------------------------------------------------------------------
 * 区域
 *
 * 单一入口、单一出口、词法嵌套、自己的名字作用域。
 * 机器上没有对应物：它是对跳转图的一个限制。
 *
 * 值只能走边界：进来走 params，出去走终结子交的 results。
 *
 * 区域末尾落下 = 顺序离开这个区域。回边必须显式 #continue。
 * ------------------------------------------------------------------------- */
struct L1Inst; /* 指令序列以不完整类型出现在区域里 */

typedef struct {
  const char *name;
  const L1Type *ty;
} L1Param;

/* 区域入口参数；只有 #loop 有。init 是进入这个区域时的初值。 */
typedef struct {
  L1Param param;
  L1Operand init;
} L1RegionParam;

typedef struct L1Region {
  const L1RegionParam *params; /* 入口 / 回边携带的状态 */
  uint32_t param_count;
  const L1Type *const *results; /* 出口结果的类型；无结果时为 NULL */
  uint32_t result_count;
  const struct L1Inst *insts; /* 平坦指令序列；位置即指令下标 */
  uint32_t inst_count;
} L1Region;

/* #switch 的每个 case 是一个区域；case 值必须是常量 */
typedef struct {
  uint64_t value; /* 按 selector 的类型实参解释 */
  const L1Region *body;
} L1SwitchCase;

/* 一个算子最多能有多少个结果。
 * 机器上一条指令可以同时给出值和标志位（sadd_overflow、cmpxchg），两个够用。 */
#define L1_MAX_RESULTS 2

/* 一条指令最多能有多少个操作数（= 一次调用最多几个实参）。
 *
 * 这个数字必须三层一致：解析器不接受更多，验证器拒绝更多，引擎的定长缓冲区
 * 装得下更多。任何一层单独调大，都会让「验证通过」不再等于「跑得起来」。 */
#define L1_MAX_OPERANDS 16u

/* ---------------------------------------------------------------------------
 * 指令
 *
 * 执行顺序 = 书写顺序。操作数必须是值或字面量，不得是嵌套操作。
 * 类型实参写在方括号里，含义按算子类别确定；内存序与 volatile 跟在类型后面。
 * ------------------------------------------------------------------------- */
typedef struct L1Inst {
  L1InstKind kind;

  /* 结果。result_count == 0 表示这条指令不产出值。 */
  const char *results[L1_MAX_RESULTS];
  uint32_t result_count;

  /* 类型实参。has_ty 为假时无类型实参
   * （#lea、#data_addr、#call、结构指令、终结子）。 */
  const L1Type *ty;
  bool has_ty;

  /* 向量算子的车道宽度：`vadd.i32x4` 的 32。
   * 车道数 = ty->width / lane_bits。0 表示不适用。 */
  uint32_t lane_bits;

  const L1Operand *operands;
  uint32_t operand_count;

  /* 内存操作 */
  L1MemOrder order;
  bool is_volatile;

  /* 被调 / 被引用的符号：INST_CALL、INST_DATA_ADDR。
   * INST_CALL_INDIRECT 不用它，函数指针是 operands[0]。 */
  const char *symbol;

  /* 结构指令 */
  const char *label;        /* INST_LOOP / INST_BREAK / INST_CONTINUE 的目标名 */
  const L1Region *body;     /* INST_IF 的 then；INST_LOOP 的体 */
  const L1Region *else_body;/* INST_IF 的 else；可空 */
  const L1SwitchCase *cases;/* INST_SWITCH */
  uint32_t case_count;
  const L1Region *default_case; /* INST_SWITCH 的 default；语义必须显式 */

  /* 源位置。LAINIR 除源位置外不携带源信息。 */
  uint32_t line;
  uint32_t column;
} L1Inst;

/* ---------------------------------------------------------------------------
 * 过程 SubRoutine
 *
 * 代码里叫 Subroutine，语法关键字仍是 `#proc`（历史原因）。
 *
 * 过程 = 签名 + 一个区域。字段里没有寄存器、参数位置、栈布局、返回地址：
 * 调用约定由 IR 定义，帧、对齐、栈大小由 lowering 从过程体算出来。
 * 「不返回」是被调用者的行为，不是 IR 的构造。
 * ------------------------------------------------------------------------- */
typedef enum {
  SUBROUTINE_NONE = 0,
  /* 没有过程体：实现在 LAINIR 之外，靠 link_name 解析。 */
  SUBROUTINE_EXTERN = 1u << 0,
} L1SubroutineFlags;

typedef struct {
  const char *name;
  L1SubroutineFlags flags;
  const char *link_name; /* SUBROUTINE_EXTERN */
  const L1Param *params;
  uint32_t param_count;
  const L1Type *const *results; /* 返回值；无结果时为 NULL */
  uint32_t result_count;
  const L1Region *body; /* SUBROUTINE_EXTERN 时为 NULL */
} L1Subroutine;

/* ---------------------------------------------------------------------------
 * 模块级数据对象
 *
 * 对应机器上的 .rodata / .data / .bss：静态的、有符号、加载时就在。
 * 和 #alloca 的区别：alloca 是运行时在栈上要的。
 * ------------------------------------------------------------------------- */
typedef struct {
  const char *symbol;
  const uint8_t *bytes; /* 初值 */
  uint32_t size;
  bool is_writable; /* 假 = .rodata */
} L1Data;

typedef struct {
  const char *name;
  const L1Data *data;
  uint32_t data_count;
  const L1Subroutine *subroutines;
  uint32_t subroutine_count;
} L1Module;

/* ---------------------------------------------------------------------------
 * 诊断
 *
 * 0 = 成功，非 0 = 稳定诊断码。
 * 域外行为不进 IR：除数为 0、移位量 >= 宽度、越权地址、预算耗尽，都由 VM
 * 确定性拒绝，不是 UB。
 * ------------------------------------------------------------------------- */
typedef struct {
  int code;
  uint32_t line;
  uint32_t column;
  char message[192];
} L1Diagnostic;

#endif /* LAINIR_CORE_H */
