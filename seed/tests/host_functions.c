#include "host_functions.h"

uint32_t host_add_100(const uint64_t *args, uint32_t arg_count,
                      uint64_t *result_out) {
  if (arg_count != 1 || !result_out) return 1;
  *result_out = args[0] + 100;
  return 0;
}

/* 读程序内存。地址是宿主地址，所以宿主能直接读——能力约束的是
 * 「程序能调谁」，不约束「宿主能碰什么」；宿主是被信任的。 */
uint32_t host_sum_bytes(const uint64_t *args, uint32_t arg_count,
                        uint64_t *result_out) {
  const uint8_t *bytes;
  uint64_t count;
  uint64_t sum = 0;
  uint64_t i;
  if (arg_count != 2 || !result_out) return 2;
  bytes = (const uint8_t *)(uintptr_t)args[0];
  count = args[1];
  if (!bytes) return 3;
  for (i = 0; i < count; i++) sum += bytes[i];
  *result_out = sum;
  return 0;
}

uint32_t host_refuse(const uint64_t *args, uint32_t arg_count,
                     uint64_t *result_out) {
  (void)args;
  (void)arg_count;
  (void)result_out;
  return 7; /* 宿主自己拒绝 */
}

uint32_t host_missing(const uint64_t *args, uint32_t arg_count,
                      uint64_t *result_out) {
  (void)args;
  (void)arg_count;
  (void)result_out;
  return 1110; /* 和 VM 的「没登记这项能力」用同一个码 */
}
