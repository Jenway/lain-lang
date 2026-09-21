/* 编译期执行的**分配配额**（规范 04-lain-vm.md §4/:105、§6/:162、§8.2/:212-214）。
 *
 * 单位是**字节**，记的是**实际承诺**的底层存储：谁真的拿到一块底层内存，谁就扣账。
 *
 *   - 账户由**一次最外层编译期执行**创建；嵌套调用共享同一个账户（同一个 TCB）；
 *   - 扣费点：VSpace 承诺一块底层存储时（这一版是 TCB 的栈租约），以及宿主暂存区
 *     与输出缓冲的**扩容**；
 *   - 归还点：真的把这块存储还回去时（销毁 TCB、释放宿主）；
 *   - 只回退水位（`#alloca` 出栈 / 返回 / 跳出循环）**不**归还整块租约的额度；
 *     栈整块预留时只在预留那一刻按整块容量扣一次，其中的子分配不重复扣；
 *   - 切换 VSpace 不创建新账户，也不恢复额度：账户跟着**执行**走，不跟着地址空间走。
 *
 * 没有账户（quota == NULL）时预扣一律成功 —— 所以现有的调用点行为不变。
 * 「没注入账户 = 不限额」和「没注入能力 = 不能碰宿主」是同一个默认。
 */
#ifndef LAINVM_QUOTA_H
#define LAINVM_QUOTA_H

#include <stdbool.h>
#include <stdint.h>

/* 账目。limit = 0 表示**不限额**（不是"一个字节都不许"—— 免得把「没设」和
 * 「设成 0」混成同一件事）。 */
typedef struct {
  uint64_t limit;     /* 预算字节数；0 = 不限额 */
  uint64_t used;      /* 已承诺字节数 */
  uint64_t peak;      /* 峰值（诊断） */
  uint64_t charges;   /* 成功预扣次数（诊断） */
  uint64_t releases;  /* 归还次数（诊断） */
  uint64_t rejected;  /* 因余额不足/溢出被拒的次数（诊断） */
  uint64_t underflow; /* 归还量超过已承诺量的次数；账目不一致，正常路径必须为 0 */
} LainVmQuota;

/* allocation quota 用尽。engine 段 1001-1043 已被占用，1044 空着（全树检索过），
 * 权威登记在 docs/spec/vm.md。 */
enum { LAINVM_QUOTA_TRAP = 1044 };

void lainvm_quota_init(LainVmQuota *quota, uint64_t limit_bytes);

/* 预扣 bytes 字节。成功返回 0；余额不足或加法溢出返回 LAINVM_QUOTA_TRAP，
 * 并且**不改变账目**（要么整笔扣掉，要么一分不扣）。 */
int lainvm_quota_charge(LainVmQuota *quota, uint64_t bytes);

/* 归还 bytes 字节——**真的释放了存储**才调。返回 0；若归还量超过已承诺量，
 * 返回非零并记下 underflow：那是账目不一致，不是可以忽略的小事。 */
int lainvm_quota_release(LainVmQuota *quota, uint64_t bytes);

/* 余额。不限额时返回 UINT64_MAX。 */
uint64_t lainvm_quota_remaining(const LainVmQuota *quota);

#endif /* LAINVM_QUOTA_H */
