/* 切片探针：一个执行体跑到一半被停下，下次接着跑，状态不丢。
 *
 * 这是 `docs/implementation/meta-parser-plan.md` 第 1 步的验收。
 *
 * 要证明的事：**LAINVM 不需要"自愿让出"也能当协程载体。** 驱动按 fuel 切片，
 * 执行体停在任意指令边界，TCB 里存着帧和位置，续跑从原处继续。
 *
 * 做法：同一份 LAINIR 程序，每次只给 1 步燃料，反复调 `lainvm_engine_run`。
 * 如果状态不跨调用保留，第二次 run 会从头开始 —— 要么死循环，要么结果不对。
 * 所以"跑了很多次、每次只前进一步、最后结果对"这一件事同时证明了：
 *   步数在累加、位置在前进、局部值活着、终态是 DONE 而不是重来。
 *
 * 期望：`count(5)` = 0+1+2+3+4 = 10，分 ~30 次跑完，最后一次 slice=DONE。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lainir/build.h"
#include "lainir/parse.h"
#include "lainir/verify.h"
#include "lainvm/engine.h"
#include "lainvm/image.h"
#include "lainvm/space.h"
#include "lainvm/tcb.h"

static const char *k_program =
    "#proc count(%n: #bits<64>) -> #bits<64> {\n"
    "  %r = #loop s(%i: #bits<64> = 0, %s: #bits<64> = 0) -> (#bits<64>) {\n"
    "    %c = #ult[#bits<64>](%i, %n)\n"
    "    #if %c {\n"
    "      %s2 = #add[#bits<64>](%s, %i)\n"
    "      %i2 = #add[#bits<64>](%i, 1)\n"
    "      #continue s(%i2, %s2)\n"
    "    }\n"
    "    #break s(%s)\n"
    "  }\n"
    "  #return %r\n"
    "}\n";

int main(void) {
  L1Builder *builder = lainir_builder_new();
  L1Diagnostic diag;
  LainVmSpace space;
  LainVmImage *image;
  LainVmTcb *tcb;
  const L1Module *module;
  L1Value args[1];
  LainVmSliceResult slice;
  uint32_t runs = 0;
  int failed = 0;

  diag.code = 0;
  module = lainir_parse(builder, k_program, &diag);
  if (!module) {
    printf("parse failed: code=%d %s\n", diag.code, diag.message);
    return 1;
  }
  if (lainir_verify(module, &diag) != 0) {
    printf("verify failed: code=%d %s\n", diag.code, diag.message);
    return 1;
  }

  lainvm_space_init(&space);
  image = lainvm_image_load(module, &space, &diag);
  if (!image) {
    printf("load failed: code=%d %s\n", diag.code, diag.message);
    return 1;
  }
  tcb = lainvm_tcb_new(image, &space, 1, 1, 64, 4096, 1);
  if (!tcb) {
    printf("admit failed\n");
    return 1;
  }

  memset(args, 0, sizeof(args));
  args[0] = (L1Value){L1_VALUE_BITS, 64, {.bits = 5}};
  if (lainvm_tcb_start(tcb, "count", args, 1, &diag) != 0) {
    printf("start failed: code=%d %s\n", diag.code, diag.message);
    return 1;
  }

  /* 每次只给 1 步燃料。 */
  for (;;) {
    uint64_t before = tcb->steps;

    slice = lainvm_engine_run(tcb, 1);
    runs++;

    if (slice == LAINVM_SLICE_DONE) {
      printf("run %2u: DONE      steps=%llu\n", runs,
             (unsigned long long)tcb->steps);
      break;
    }
    if (slice != LAINVM_SLICE_RUNNABLE) {
      printf("run %2u: slice=%d trap kind=%d status=%d\n", runs, (int)slice,
             (int)tcb->trap.kind, tcb->trap.status);
      failed = 1;
      break;
    }
    if (tcb->steps == before) {
      printf("run %2u: 没有前进 —— 状态没跨调用保留\n", runs);
      failed = 1;
      break;
    }
    if (runs > 1000) {
      printf("跑了 1000 次还没结束\n");
      failed = 1;
      break;
    }
  }

  printf("---\n");
  if (failed) {
    printf("FAILURES\n");
    return 1;
  }
  if (!tcb->has_result) {
    printf("FAILURES：DONE 了但没有结果\n");
    return 1;
  }
  if (tcb->result.as.bits != 10) {
    printf("FAILURES：count(5) = %llu，期望 10\n",
           (unsigned long long)tcb->result.as.bits);
    return 1;
  }

  printf("count(5) = %llu，分 %u 次跑完（每次 1 步燃料）\n",
         (unsigned long long)tcb->result.as.bits, runs);
  printf("ALL PASS\n");

  lainvm_tcb_free(tcb);
  lainvm_image_free(image);
  lainir_builder_free(builder);
  return 0;
}
