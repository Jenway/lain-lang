/* LAINVM 的内核对象。
 *
 * microkernel 风格：
 *   - TCB 是不透明的内核对象。LAINIR 里没有任何类型、值或指令能引用它，
 *     TCB 的地址绝不进入程序。
 *   - TCB ≠ 进程。地址空间（LainVmSpace）是独立对象，TCB 只引用它，
 *     所以多个 TCB 能共享一个地址空间——那就是线程。
 *   - 上下文住在 TCB，不住在地址空间：区域栈和值槽是执行状态，
 *     程序既不能读也不能命名它们（值不占存储、不可寻址）。
 *   - 故障不致命：出错走 fault_handler 这条端点，把 trap 交出去；
 *     TCB 只记录，不自行终止。
 *
 * 命名：LAINIR 侧 L1*，LAINVM 侧 LainVm*。
 */
#ifndef LAINVM_TCB_H
#define LAINVM_TCB_H

#include <stdbool.h>
#include <stdint.h>

#include "lainir/core.h"
#include "lainir/value.h"
#include "lainvm/caps.h"
#include "lainvm/space.h"

typedef struct LainVmTcb LainVmTcb;
typedef struct LainVmImage LainVmImage;
typedef struct LainVmEndpoint LainVmEndpoint;

typedef enum {
  LAINVM_READY = 0,
  LAINVM_RUNNING = 1,
  LAINVM_BLOCKED = 2,
  LAINVM_DEAD = 3,
} LainVmState;

typedef enum {
  LAINVM_SLICE_RUNNABLE = 0, /* 还有事做：可再次选中 */
  LAINVM_SLICE_BLOCKED = 1,  /* 阻塞在某个端点上 */
  LAINVM_SLICE_DONE = 2,     /* 激活结束 */
  LAINVM_SLICE_TRAPPED = 3,  /* 拒绝执行，trap 已记录 */
} LainVmSliceResult;

/* BLOCKED 时的原因 */
enum {
  LAINVM_SUSPEND_YIELD = 1,
  LAINVM_SUSPEND_ENDPOINT = 2,
};

/* 会合结果 */
enum {
  LAINVM_ENDPOINT_SEND = 1,
  LAINVM_ENDPOINT_RECEIVE = 2,
  LAINVM_ENDPOINT_CANCELLED = 3,
};

/* VM 拒绝执行的原因。
 * 域外行为（除数为 0、移位量 >= 宽度、越权地址、预算耗尽）都在这里报出去，
 * 不是 UB，也不需要 IR 里有对应构造。 */
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
   * 不碰字符串。没有调用 lainvm_tcb_set_caps 时是 NULL，于是任何宿主调用都被拒绝：
   * 「没注入能力 = 不能碰宿主」是默认。 */
  LainVmCaps *caps;
  const LainVmCapEntry **resolved;
  uint32_t resolved_count;

  /* alloca 栈。区段登记在 vspace 里，owner 是这个 TCB；
   * 字节在 admit 时按 stack_bytes 一次给够（引擎里不许分配），
   * 满了就是 trap，不是扩容。 */
  int32_t stack_region; /* LAINVM_SPACE_NO_REGION 表示没有栈 */
  uint64_t stack_used;  /* alloca 水位 */

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

/* --- admit -----------------------------------------------------------------
 * 引擎里不许分配，所以帧栈和值槽在这里一次给够：
 *   frame_cap = max_call_depth * (image->max_region_depth + 1)
 *   slot_cap  = frame_cap * image->max_slots
 * id 同时是栈区段的 owner，lainvm_tcb_free 时按 owner 回收。
 * stack_bytes 为 0 表示不要栈（程序里没有 #alloca）。 */
LainVmTcb *lainvm_tcb_new(LainVmImage *image, LainVmSpace *space, uint64_t id,
                          uint64_t owner, uint32_t max_call_depth,
                          uint64_t stack_bytes);
void lainvm_tcb_free(LainVmTcb *tcb);

/* 把模块里每个 extern 子过程按 link_name 解析到能力空间。
 * 解析不到的记 NULL，等真去调它时再拒绝——一个模块可以带着用不到的外部依赖。
 * 必须活得比 TCB 长（TCB 之后不再碰这张表）。 */
int lainvm_tcb_set_caps(LainVmTcb *tcb, LainVmCaps *caps, L1Diagnostic *diag);

/* 压入根帧、把 args 绑到入口过程的参数槽、置 RUNNING。
 * 成功返回 0；失败返回非 0 并把原因写进 diag（可为 NULL）。 */
int lainvm_tcb_start(LainVmTcb *tcb, const char *entry, const L1Value *args,
                     uint32_t arg_count, L1Diagnostic *diag);

#endif /* LAINVM_TCB_H */
