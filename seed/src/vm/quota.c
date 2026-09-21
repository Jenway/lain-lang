/* 分配配额。契约、扣费点与归还规则见 lainvm/quota.h 的文件头。
 *
 * 纪律：预扣是**原子**的（要么整笔扣掉，要么一分不扣）——失败之后账目必须与
 * 调用前一模一样，否则"分配失败"会留下抹不掉的扣账。
 */
#include "lainvm/quota.h"

#include <string.h>

void lainvm_quota_init(LainVmQuota *quota, uint64_t limit_bytes) {
  if (!quota) return;
  memset(quota, 0, sizeof(*quota));
  quota->limit = limit_bytes;
}

/* 这笔预扣放得下吗。limit = 0 = 不限额。用减法判上界，避免 used + bytes 溢出。 */
static bool fits(const LainVmQuota *quota, uint64_t bytes) {
  if (quota->limit == 0) return true;
  if (bytes > quota->limit) return false;
  return quota->used <= quota->limit - bytes;
}

int lainvm_quota_charge(LainVmQuota *quota, uint64_t bytes) {
  uint64_t next;

  if (!quota || bytes == 0) return 0; /* 没账户 = 不限额 */
  next = quota->used + bytes;
  if (next < quota->used) { /* 加法溢出：宁可拒，也不把账绕回去 */
    quota->rejected += 1;
    return LAINVM_QUOTA_TRAP;
  }
  if (!fits(quota, bytes)) {
    quota->rejected += 1;
    return LAINVM_QUOTA_TRAP; /* 账目没动 */
  }
  quota->used = next;
  if (quota->used > quota->peak) quota->peak = quota->used;
  quota->charges += 1;
  return 0;
}

int lainvm_quota_release(LainVmQuota *quota, uint64_t bytes) {
  if (!quota || bytes == 0) return 0;
  if (bytes > quota->used) { /* 归还超过已承诺：账目不一致 */
    quota->underflow += 1;
    quota->used = 0; /* 不许把账做成负数 */
    return 1;
  }
  quota->used -= bytes;
  quota->releases += 1;
  return 0;
}

uint64_t lainvm_quota_remaining(const LainVmQuota *quota) {
  if (!quota || quota->limit == 0) return UINT64_MAX;
  return quota->limit - quota->used;
}
