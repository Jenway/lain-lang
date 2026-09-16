/* C 后端的驱动：只调用编译出来的 l1_call，打印和 VM 那一侧同样的格式。
 * 子过程序号必须和 emit_c.c 里的枚举一致。 */
#include <stdint.h>
#include <stdio.h>

extern uint64_t l1_call(uint32_t sub_index, const uint64_t *args, uint32_t nargs);
extern uint64_t l1_data_addr(uint32_t index);
extern uint32_t l1_data_count(void);

enum {
  SUB_ANSWER = 0,
  SUB_MAX = 1,
  SUB_FACT = 2,
  SUB_SUM_BYTES = 3,
  SUB_FIRST_BYTE = 4,
  SUB_INC = 5,
  SUB_APPLY = 6,
  SUB_VIA_PTR = 7,
  SUB_SLT32 = 8,
  SUB_ALLOCA_STORE = 9,
  SUB_HOST_ADD_100 = 10,
  SUB_HOST_CALLER = 11,
  SUB_HOST_SUM_BYTES = 12,
  SUB_HOST_SUM = 13
};

typedef struct {
  const char *name;
  uint32_t sub;
  uint64_t args[2];
  uint32_t nargs;
} Case;

int main(void) {
  Case cases[16];
  uint32_t count = 0;
  uint32_t i;
  uint64_t data_addr;

  if (l1_data_count() != 1) {
    printf("# unexpected data count\n");
    return 1;
  }
  data_addr = l1_data_addr(0);

  cases[count].name = "answer"; cases[count].sub = SUB_ANSWER; cases[count].nargs = 0; count++;
  cases[count].name = "max(3,9)"; cases[count].sub = SUB_MAX; cases[count].nargs = 2;
  cases[count].args[0] = 3; cases[count].args[1] = 9; count++;
  cases[count].name = "max(9,3)"; cases[count].sub = SUB_MAX; cases[count].nargs = 2;
  cases[count].args[0] = 9; cases[count].args[1] = 3; count++;
  cases[count].name = "fact(5)"; cases[count].sub = SUB_FACT; cases[count].nargs = 1;
  cases[count].args[0] = 5; count++;
  cases[count].name = "fact(0)"; cases[count].sub = SUB_FACT; cases[count].nargs = 1;
  cases[count].args[0] = 0; count++;
  cases[count].name = "sum_bytes(3)"; cases[count].sub = SUB_SUM_BYTES; cases[count].nargs = 2;
  cases[count].args[0] = data_addr; cases[count].args[1] = 3; count++;
  cases[count].name = "sum_bytes(0)"; cases[count].sub = SUB_SUM_BYTES; cases[count].nargs = 2;
  cases[count].args[0] = data_addr; cases[count].args[1] = 0; count++;
  cases[count].name = "first_byte"; cases[count].sub = SUB_FIRST_BYTE; cases[count].nargs = 0; count++;
  cases[count].name = "via_ptr"; cases[count].sub = SUB_VIA_PTR; cases[count].nargs = 0; count++;
  cases[count].name = "slt32"; cases[count].sub = SUB_SLT32; cases[count].nargs = 0; count++;
  cases[count].name = "alloca_store"; cases[count].sub = SUB_ALLOCA_STORE; cases[count].nargs = 0; count++;
  cases[count].name = "host_caller(5)"; cases[count].sub = SUB_HOST_CALLER; cases[count].nargs = 1;
  cases[count].args[0] = 5; count++;
  cases[count].name = "host_sum()"; cases[count].sub = SUB_HOST_SUM; cases[count].nargs = 0; count++;

  printf("# C\n");
  for (i = 0; i < count; i++) {
    uint64_t value = l1_call(cases[i].sub, cases[i].args, cases[i].nargs);
    printf("%s = %llu\n", cases[i].name, (unsigned long long)value);
  }
  return 0;
}
