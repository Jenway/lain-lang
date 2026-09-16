/* LAINVM 引擎：CU + ALU。
 *
 * 无状态：所有可变状态在 TCB 里。所以没有 LainVmEngine 这个类型，
 * 只有一组函数；同一个 TCB 可以被任何一次调用推进。
 *
 * 一条指令 = 一步。终结子也算一步。
 *
 * 它不做的事：
 *   不分配    帧栈和值槽在 admit 时按 max_call_depth 分配好，
 *             这里没有分配，也就没有分配失败路径
 *   不调度    那是 scheduler 的
 *   不给预算  燃料由调用者给
 *   不管阻塞  返回 BLOCKED，由 scheduler 处置
 *   不吞 trap 写进 tcb->trap 后返回 TRAPPED
 *
 * 它不认识 #eval。阶段标注在 folding 阶段就被消掉了：folding 发现一条
 * eval 调用，是它去驱动 VM 跑被调方再替换成常量，VM 运行期永远看不到它。
 *
 * 前置条件：tcb->state == LAINVM_RUNNING。不满足时不动任何东西，
 * 原样返回当前的 slice_result。
 */
#ifndef LAINVM_ENGINE_H
#define LAINVM_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "lainir/core.h"
#include "lainvm/image.h"
#include "lainvm/tcb.h"

/* 引擎从映像要四样东西：区域描述、指令、操作数引用、结果槽。
 * 它们的接口在 lainvm/image.h。 */

/* ---------------------------------------------------------------------------
 * 引擎内部助手
 *
 * 操作数读取和结果写回不要在 88 个算子里各写一遍。
 * 只在「当前指令」上工作——CU 传进来的就是当前那条。
 * ------------------------------------------------------------------------- */

/* 读第 index 个操作数。字面量按 inst->ty 解释，不进槽。 */
L1Value lainvm_operand_read(const LainVmTcb *tcb, const L1Inst *inst,
                            uint32_t index);

/* 写第 index 个结果。 */
void lainvm_result_write(LainVmTcb *tcb, const L1Inst *inst, uint32_t index,
                         L1Value value);

/* ---------------------------------------------------------------------------
 * CU：取指 -> 查表 -> 调用
 * ------------------------------------------------------------------------- */

typedef LainVmSliceResult (*LainVmOp)(LainVmTcb *tcb, const L1Inst *inst);

/* 分派表，按 L1InstKind 索引。测试可以直接取。 */
const LainVmOp *lainvm_op_table(void);

/* 推进一步。 */
LainVmSliceResult lainvm_engine_step(LainVmTcb *tcb);

/* 最多推进 fuel 步（fuel > 0）。燃料耗尽返回 RUNNABLE。 */
LainVmSliceResult lainvm_engine_run(LainVmTcb *tcb, uint64_t fuel);

/* 直接调一次宿主能力，不经 IR 的调用指令。
 *
 * 编译期执行需要它：#eval 打到外部符号上时没有体区域可以跑，
 * 起不了 activation，只能直接把这一项能力调掉。
 * 结果按被调方声明的返回类型还原（#addr 就是地址，其余是位串）。 */
LainVmSliceResult lainvm_vm_call_host(LainVmTcb *tcb, uint32_t sub_index,
                                      const L1Value *args, uint32_t arg_count,
                                      L1Value *result_out);

/* ---------------------------------------------------------------------------
 * ALU：纯算子。忽略 tcb 里的 vspace / 映像 / 帧栈，只通过值槽进出。
 *
 * 域外行为（除数为 0、移位量 >= 宽度）记 trap 后返回 TRAPPED，
 * 不返回任何值。
 * ------------------------------------------------------------------------- */

/* 整数算术 */
LainVmSliceResult lainvm_op_add(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_sub(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_mul(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_sdiv(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_udiv(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_srem(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_urem(LainVmTcb *tcb, const L1Inst *inst);

/* 位运算 */
LainVmSliceResult lainvm_op_and(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_or(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_xor(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_shl(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_lshr(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_ashr(LainVmTcb *tcb, const L1Inst *inst);

/* 浮点算术 */
LainVmSliceResult lainvm_op_fadd(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_fsub(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_fmul(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_fdiv(LainVmTcb *tcb, const L1Inst *inst);

/* 整数比较；结果 #bits<1> */
LainVmSliceResult lainvm_op_eq(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_ne(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_slt(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_sle(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_sgt(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_sge(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_ult(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_ule(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_ugt(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_uge(LainVmTcb *tcb, const L1Inst *inst);

/* 浮点比较；有序。NaN 让所有有序比较为假 */
LainVmSliceResult lainvm_op_foeq(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_fone(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_folt(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_fole(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_fogt(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_foge(LainVmTcb *tcb, const L1Inst *inst);

/* 浮点比较；无序。NaN 让所有无序比较为真 */
LainVmSliceResult lainvm_op_fueq(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_fune(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_fult(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_fule(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_fugt(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_fuge(LainVmTcb *tcb, const L1Inst *inst);

/* 整数转换。源宽度取操作数，inst->ty 是目标宽度 */
LainVmSliceResult lainvm_op_zext(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_sext(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_trunc(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_bitcast(LainVmTcb *tcb, const L1Inst *inst);

/* 浮点转换 */
LainVmSliceResult lainvm_op_fpext(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_fptrunc(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_fptosi(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_fptoui(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_sitofp(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_uitofp(LainVmTcb *tcb, const L1Inst *inst);

/* 地址转换。只重解释位，不给权限；有效性在访问时判定 */
LainVmSliceResult lainvm_op_int2ptr(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_ptr2int(LainVmTcb *tcb, const L1Inst *inst);

/* 向量。车道分解在 inst->lane_bits 和 inst->ty 里 */
LainVmSliceResult lainvm_op_vadd(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_vsub(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_vmul(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_vdiv(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_vand(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_vor(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_vxor(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_vshl(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_vshuffle(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_vbroadcast(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_vextract(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_vinsert(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_vcmpeq(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_vcmpne(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_vcmplt(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_vcmpgt(LainVmTcb *tcb, const L1Inst *inst);

/* ---------------------------------------------------------------------------
 * 需要机器的那一侧：碰 VSpace / 映像 / 帧栈
 * ------------------------------------------------------------------------- */

/* 取址。纯算术：base + idx*scale + offset，不判有效性 */
LainVmSliceResult lainvm_op_lea(LainVmTcb *tcb, const L1Inst *inst);

/* 内存。地址不在 VSpace 里就拒绝 */
LainVmSliceResult lainvm_op_load(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_store(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_alloca(LainVmTcb *tcb, const L1Inst *inst);

/* 模块级数据对象的地址。装载时已烤成地址（映像绑定地址空间） */
LainVmSliceResult lainvm_op_data_addr(LainVmTcb *tcb, const L1Inst *inst);

/* 子过程的地址。产出普通 #addr；能不能被调用由区段的 CALL 权限说话 */
LainVmSliceResult lainvm_op_proc_addr(LainVmTcb *tcb, const L1Inst *inst);

/* 调用。压帧，绑定实参到被调方的参数槽 */
LainVmSliceResult lainvm_op_call(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_call_indirect(LainVmTcb *tcb, const L1Inst *inst);

/* 原子 */
LainVmSliceResult lainvm_op_xchg(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_cmpxchg(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_rmw_add(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_rmw_sub(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_rmw_and(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_rmw_or(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_rmw_xor(LainVmTcb *tcb, const L1Inst *inst);

/* 结构。压/弹区域帧 */
LainVmSliceResult lainvm_op_if(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_loop(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_switch(LainVmTcb *tcb, const L1Inst *inst);

/* 终结子 */
LainVmSliceResult lainvm_op_yield(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_break(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_continue(LainVmTcb *tcb, const L1Inst *inst);
LainVmSliceResult lainvm_op_return(LainVmTcb *tcb, const L1Inst *inst);

#endif /* LAINVM_ENGINE_H */
