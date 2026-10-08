/* TCB 的内部结构：引擎与 admit 用的那份执行上下文。不属于公开面。
 *
 * 公开面（lain/vm/tcb.h）只有不透明句柄、状态与拒绝码，外加几个只读访问器。
 * 驱动与宿主机制读结果、故障、步数走访问器，不直接取字段。
 */
#ifndef LAINVM_TCB_INTERNAL_H
#define LAINVM_TCB_INTERNAL_H

#include "lain/vm/tcb.h"

/* 一层区域激活。
 *
 * 进 #if / #loop / #switch 都压一帧；#call 压的帧带 is_call_frame。
 * 值槽按区域静态布局，帧里只放起点和数量。
 *
 * parent_position 是父区域里创造这一帧的那条指令：区域交值、返回、
 * 跳出循环都要用它找到该写进哪个结果槽。
 * label 只有循环帧非空，#break / #continue 靠它找目标。
 * stack_mark 是进入这一帧时的 alloca 水位，离开时回退。 */
typedef struct {
  uint32_t region;
  uint32_t position;
  uint32_t slot_base;
  uint32_t slot_count;
  uint32_t parent_position;
  const char *label;
  bool is_call_frame;
  uint64_t stack_mark;
} LainVmFrame;

struct LainVmTcb {
  /* 身份 */
  uint64_t id;    /* 调度顺序、诊断用；不是地址 */
  uint64_t owner; /* 谁创建的；每个控制操作都要比对 */

  /* 状态机 */
  LainVmState state;
  uint32_t suspend_reason; /* BLOCKED 时有效 */

  /* 引用：它跑在什么上。都是引用，不拥有。 */
  LainVmImage *image;  /* 装载后的映像：名字全换成槽、符号已解析、地址已烤进去 */
  LainVmSpace *vspace; /* 地址空间 */

  /* 能力：这次激活允许调用哪些外部符号。
   * resolved 按子过程下标索引，是 admit 时解析好的结果——运行期只做数组索引，
   * 不碰字符串。每项自带可信 context，宿主调用时由 VM 注入，不从业务参数读。
   * 没有调用 lainvm_tcb_set_caps 时是 NULL，于是任何宿主调用都被拒绝：
   * 「没注入能力 = 不能碰宿主」是默认。 */
  LainVmCaps *caps;
  const LainVmCapEntry **resolved;
  uint32_t resolved_count;

  /* 栈：**只借，不拥有**。
   *
   * 字节由供给方用 `lainvm_space_alloc_stack` 申请并登记；TCB 拿到的是一条租约
   * （空间 + 稳定句柄），校验后 borrow_count++。
   *   - TCB **不** calloc、**不** free、**不**撤销区段、**不**动 quota；
   *   - 执行期间 TCB 只决定窗口：`stack_used` 是水位，窗口 = 该区段的可访问前缀；
   *   - Trap 时窗口归零；销毁时窗口收回 0 并结束借用，区段留给供给方释放。
   * base / capacity / 权限一律从 VSpace 的记录读，不在 TCB 里留副本。 */
  LainVmStackLease stack_lease;
  uint64_t stack_used; /* alloca 水位（procedure activation 的生命周期，见下） */

  /* 这次执行的分配账户。NULL = 不限额。
   * 账户跟着**执行**走，不跟着地址空间走：嵌套调用共享同一个账户，
   * `lainvm_tcb_set_space` 既不创建新账户、也不恢复额度。
   * 栈按**整块容量**在这里预扣一次，销毁时归还；`#alloca` 的子分配与水位回退
   * 都不动账（授权窗口的登记/撤销也不扣 —— 那是同一块已扣过账的存储）。 */
  LainVmQuota *quota;

  /* 上下文：恢复一次激活所需的全部 */
  LainVmFrame *frames;
  uint32_t frame_count;
  uint32_t frame_cap;
  L1Value *slots;
  uint32_t slot_count;
  uint32_t slot_cap;

  /* 结果 */
  L1Value result;
  bool has_result;

  /* 调度上下文 */
  uint64_t steps; /* 这个 TCB 一共消耗多少步 */
  uint64_t fuel;  /* 当前切片剩余燃料 */
  LainVmSliceResult slice_result;
  bool slice_exhausted;

  /* 阻塞 / 会合 */
  LainVmEndpoint *blocked_on; /* 非阻塞时为 NULL */
  uint32_t pending_kind;
  uint64_t pending_value;
  bool has_pending;

  /* 故障 */
  LainVmTrap trap;
  LainVmEndpoint *fault_handler;
};

#endif /* LAINVM_TCB_INTERNAL_H */
