/* 切片 5 的验收（2/2）：编译产物那一侧的域外行为。
 *
 * 无参数：跑合法输入，打印和 VM 同样的形状（给 harness 对拍）。
 * 带参数 "div0" / "shift64"：跑域外输入，产物应当 abort（退出码非 0）。 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint64_t l1_call(uint32_t sub_index, const uint64_t *args,
                        uint32_t nargs);

enum { SUB_DIV_BY = 0, SUB_SHIFT_BY = 1 };

int main(int argc, char **argv) {
  uint64_t args[2];

  if (argc > 1 && strcmp(argv[1], "div0") == 0) {
    args[0] = 10;
    args[1] = 0;
    printf("should not get here: %llu\n",
           (unsigned long long)l1_call(SUB_DIV_BY, args, 2));
    return 1;
  }
  if (argc > 1 && strcmp(argv[1], "shift64") == 0) {
    args[0] = 1;
    args[1] = 64;
    printf("should not get here: %llu\n",
           (unsigned long long)l1_call(SUB_SHIFT_BY, args, 2));
    return 1;
  }

  args[0] = 10;
  args[1] = 2;
  printf("div_by(10,2) = %llu\n",
         (unsigned long long)l1_call(SUB_DIV_BY, args, 2));
  args[0] = 1;
  args[1] = 3;
  printf("shift_by(1,3) = %llu\n",
         (unsigned long long)l1_call(SUB_SHIFT_BY, args, 2));
  return 0;
}
