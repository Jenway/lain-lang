/* 文档 §4.1 的七个 Trap 分类视图。
 *
 * 状态码本身不动（1xxx 号段见 lain/ir/codes.h）：这一层只回答「这次失败在文档的
 * 哪个类别名下」，让宿主诊断、差分测试和文档用同一个名字说话。
 */
#include "lain/vm/trap.h"

LainVmTrapClass lainvm_trap_class(const LainVmTrap *trap) {
  if (!trap || !trap->active) return LAINVM_TRAP_CLASS_NONE;
  switch (trap->status) {
  case LAINVM_TRAP_DIVIDE_BY_ZERO:
  case LAINVM_TRAP_SHIFT_TOO_WIDE:
  case LAINVM_TRAP_INT_OVERFLOW:
  /* #alloca 的尺寸算术回绕：算不下去的加法，归算术域。 */
  case LAINVM_TRAP_ALLOCA_OVERFLOW:
    return LAINVM_TRAP_CLASS_ARITH_DOMAIN;
  case LAINVM_TRAP_ALIGNMENT:
    return LAINVM_TRAP_CLASS_ALIGNMENT;
  case LAINVM_TRAP_READ_ONLY:
  /* 间接调用的目标区段没有 CALL 权限：是权限判定，不是越界。 */
  case LAINVM_TRAP_INDIRECT_NO_CALL:
    return LAINVM_TRAP_CLASS_PERM_VIOLATION;
  case LAINVM_TRAP_LOAD_FAULT:
  case LAINVM_TRAP_STORE_FAULT:
  case LAINVM_TRAP_STACK_WINDOW:
    return LAINVM_TRAP_CLASS_MEMORY_OOB;
  case LAINVM_TRAP_QUOTA:
  case LAINVM_TRAP_STACK_EXHAUSTED:
    return LAINVM_TRAP_CLASS_CAPACITY_EXCEEDED;
  case LAINVM_TRAP_UNREACHABLE:
    return LAINVM_TRAP_CLASS_UNREACHABLE;
  case LAINVM_TRAP_CAP_UNRESOLVED:
  case LAINVM_TRAP_CAP_NOT_FUNCTION:
  case LAINVM_TRAP_CAP_NO_FN:
    return LAINVM_TRAP_CLASS_CAP_DENIED;
  default:
    break;
  }
  /* 能力类的码号段会继续加：按 kind 兜一层，不必逐个列。 */
  if (trap->kind == LAINVM_TRAP_CAPABILITY) return LAINVM_TRAP_CLASS_CAP_DENIED;
  return LAINVM_TRAP_CLASS_NONE;
}

const char *lainvm_trap_class_name(LainVmTrapClass cls) {
  switch (cls) {
  case LAINVM_TRAP_CLASS_MEMORY_OOB: return "TRAP_MEMORY_OOB";
  case LAINVM_TRAP_CLASS_ALIGNMENT: return "TRAP_ALIGNMENT";
  case LAINVM_TRAP_CLASS_PERM_VIOLATION: return "TRAP_PERM_VIOLATION";
  case LAINVM_TRAP_CLASS_ARITH_DOMAIN: return "TRAP_ARITH_DOMAIN";
  case LAINVM_TRAP_CLASS_CAP_DENIED: return "TRAP_CAP_DENIED";
  case LAINVM_TRAP_CLASS_CAPACITY_EXCEEDED: return "TRAP_CAPACITY_EXCEEDED";
  case LAINVM_TRAP_CLASS_UNREACHABLE: return "TRAP_UNREACHABLE";
  case LAINVM_TRAP_CLASS_NONE:
  default: return "TRAP_NONE";
  }
}
