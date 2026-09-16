/* 两边共用的宿主函数。
 *
 * 签名就是 lainvm/caps.h 里的 LainVmHostFn——**类型完全相同**，
 * 所以同一个 C 函数既能被解释器通过能力表调用，也能被编译产物当外部符号链接。
 * 这就是「一条 ABI 两个消费者」。 */
#ifndef LAIN_HOST_FUNCTIONS_H
#define LAIN_HOST_FUNCTIONS_H

#include <stdint.h>

uint32_t host_add_100(const uint64_t *args, uint32_t arg_count,
                      uint64_t *result_out);
uint32_t host_sum_bytes(const uint64_t *args, uint32_t arg_count,
                        uint64_t *result_out);
uint32_t host_refuse(const uint64_t *args, uint32_t arg_count,
                     uint64_t *result_out);
/* 「这个能力不存在」的替身：SYMBOL 策略下，产物引用的每个符号都必须在链接期
 * 被兑现，所以「没有这个能力」要用一个拒绝的桩表达，而不是缺一个符号。 */
uint32_t host_missing(const uint64_t *args, uint32_t arg_count,
                      uint64_t *result_out);

#endif /* LAIN_HOST_FUNCTIONS_H */
