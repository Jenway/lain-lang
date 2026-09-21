/* 切片 4 的验收：文本两端。
 *
 * 判据：
 *   1. 手写文本 -> parse -> verify -> 在 VM 里跑，结果正确；
 *   2. **固定点**：print(parse(text)) 再 parse 再 print，两次逐字节相同；
 *   3. 往返保语义：跑 canonical 文本和跑原文得到同样的值；
 *   4. 输入糖：操作数位置上的嵌套指令展平成独立指令，语义不变；
 *   5. 语法错误给出位置；
 *   6. 解析出来的模块能被 C 后端编译。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lainir/build.h"
#include "lainir/parse.h"
#include "lainir/print.h"
#include "lainir/verify.h"
#include "lainbackend/emit.h"
#include "lainvm/engine.h"
#include "lainvm/image.h"

static int failures = 0;

static void check(bool ok, const char *what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static const char k_text[] =
    "data bytes ro { 7 8 9 }\n"
    "\n"
    "#proc answer() -> #bits<64> {\n"
    "  %r = #add[#bits<64>](40, 2)\n"
    "  #return %r\n"
    "}\n"
    "\n"
    "#proc max(%a: #bits<64>, %b: #bits<64>) -> #bits<64> {\n"
    "  %c = #sgt[#bits<64>](%a, %b)\n"
    "  %r = #if %c -> (#bits<64>) {\n"
    "    #yield %a\n"
    "  } else {\n"
    "    #yield %b\n"
    "  }\n"
    "  #return %r\n"
    "}\n"
    "\n"
    "#proc sum_bytes(%p: #addr, %n: #bits<64>) -> #bits<64> {\n"
    "  %sum = #loop bytes(%i: #bits<64> = 0, %s: #bits<64> = 0) -> (#bits<64>) {\n"
    "    %c = #uge[#bits<64>](%i, %n)\n"
    "    #if %c {\n"
    "      #break bytes(%s)\n"
    "    }\n"
    "    %a = #lea(%p, %i, 1, 0)\n"
    "    %b = #load[#bits<8>](%a)\n"
    "    %w = #zext[#bits<64>](%b)\n"
    "    %s2 = #add[#bits<64>](%s, %w)\n"
    "    %i2 = #add[#bits<64>](%i, 1)\n"
    "    #continue bytes(%i2, %s2)\n"
    "  }\n"
    "  #return %sum\n"
    "}\n"
    "\n"
    "#proc classify(%x: #bits<64>) -> #bits<64> {\n"
    "  %r = #switch[#bits<64>] %x -> (#bits<64>) {\n"
    "    case 0 {\n"
    "      #yield 100\n"
    "    }\n"
    "    case 1 {\n"
    "      #yield 200\n"
    "    }\n"
    "    default {\n"
    "      #yield 999\n"
    "    }\n"
    "  }\n"
    "  #return %r\n"
    "}\n";

/* 操作数位置上的嵌套指令：糖。 */
static const char k_sugar[] =
    "#proc sugar() -> #bits<64> {\n"
    "  %r = #add[#bits<64>](40, #mul[#bits<64>](2, 1))\n"
    "  #return %r\n"
    "}\n";

static int run_bits(const L1Module *module, const char *entry, uint64_t a0,
                    uint64_t a1, uint32_t nargs, uint32_t data_index,
                    uint64_t *out) {
  LainVmSpace space;
  LainVmImage *image;
  LainVmTcb *tcb;
  L1Diagnostic diag;
  L1Value args[2];
  LainVmSliceResult result;
  int rc = 1;

  diag.code = 0;
  lainvm_space_init(&space);
  image = lainvm_image_load(module, &space, &diag);
  if (!image) {
    printf("     load failed: %d %s\n", diag.code, diag.message);
    return 1;
  }
  tcb = lainvm_tcb_new(image, &space, 1, 1, 64, lainvm_stack_no_lease(), NULL);
  if (!tcb) {
    lainvm_image_free(image);
    return 1;
  }
  memset(args, 0, sizeof(args));
  if (data_index != 0xFFFFFFFFu)
    args[0] = (L1Value){L1_VALUE_ADDR, 0,
                        {.addr = (void *)image->symbols[data_index].addr}};
  else
    args[0] = (L1Value){L1_VALUE_BITS, 64, {.bits = a0}};
  args[1] = (L1Value){L1_VALUE_BITS, 64, {.bits = a1}};

  if (lainvm_tcb_start(tcb, entry, args, nargs, &diag) != 0) {
    printf("     start %s failed: %d %s\n", entry, diag.code, diag.message);
  } else {
    result = lainvm_engine_run(tcb, 1000000);
    if (result == LAINVM_SLICE_DONE && tcb->has_result) {
      *out = tcb->result.kind == L1_VALUE_ADDR
                 ? (uint64_t)(uintptr_t)tcb->result.as.addr
                 : tcb->result.as.bits;
      rc = 0;
    } else {
      printf("     run %s: slice=%d trap=%d\n", entry, (int)result,
             tcb->trap.status);
    }
  }
  lainvm_tcb_free(tcb);
  lainvm_image_free(image);
  return rc;
}

static FILE *g_out;
static uint32_t g_bytes;
static void sink_write(void *user, const char *bytes, uint32_t size) {
  (void)user;
  g_bytes += size;
  if (g_out) fwrite(bytes, 1, size, g_out);
}
static void sink_symbol(void *user, const char *symbol, bool is_extern) {
  (void)user;
  (void)symbol;
  (void)is_extern;
}

int main(void) {
  L1Builder *b = lainir_builder_new();
  L1Diagnostic diag;
  const L1Module *m;
  char *canonical;
  char *again;
  uint64_t v = 0;

  setvbuf(stdout, NULL, _IONBF, 0);
  diag.code = 0;

  /* 1. 解析 + 验证 */
  m = lainir_parse(b, k_text, &diag);
  check(m != NULL, "手写文本解析成功");
  if (!m) {
    printf("     diag: %d:%u %s\n", diag.code, diag.line, diag.message);
    return 1;
  }
  check(lainir_verify(m, &diag) == 0, "解析出来的模块通过验证");
  if (diag.code) printf("     diag: %d %s\n", diag.code, diag.message);

  /* 2. 跑起来 */
  check(run_bits(m, "answer", 0, 0, 0, 0xFFFFFFFFu, &v) == 0 && v == 42,
        "answer() = 42");
  v = 0;
  check(run_bits(m, "max", 3, 9, 2, 0xFFFFFFFFu, &v) == 0 && v == 9,
        "max(3,9) = 9");
  v = 0;
  check(run_bits(m, "sum_bytes", 0, 3, 2, 0, &v) == 0 && v == 24,
        "sum_bytes(bytes, 3) = 24");

  /* 3. 固定点 */
  canonical = lainir_print_to_string(m);
  check(canonical != NULL, "打印成功");
  if (canonical) {
    FILE *dump = fopen("build/tmp-probe/canonical.l1", "wb");
    if (dump) {
      fwrite(canonical, 1, strlen(canonical), dump);
      fclose(dump);
    }
  }
  {
    L1Builder *b2 = lainir_builder_new();
    L1Diagnostic d2;
    const L1Module *m2;
    d2.code = 0;
    m2 = lainir_parse(b2, canonical, &d2);
    check(m2 != NULL, "canonical 文本能再解析");
    if (!m2) {
      printf("     diag: %d:%u %s\n", d2.code, d2.line, d2.message);
      return 1;
    }
    again = lainir_print_to_string(m2);
    check(again != NULL && strcmp(again, canonical) == 0,
          "固定点：print(parse(print(m))) == print(m)");
    if (again && strcmp(again, canonical) != 0) {
      printf("----- canonical -----\n%s", canonical);
      printf("----- again -----\n%s", again);
    }
    /* 往返保语义 */
    v = 0;
    check(run_bits(m2, "sum_bytes", 0, 3, 2, 0, &v) == 0 && v == 24,
          "canonical 文本跑出来一样（sum_bytes = 24）");
    v = 0;
    check(run_bits(m2, "classify", 1, 0, 1, 0xFFFFFFFFu, &v) == 0 && v == 200,
          "canonical 文本跑出来一样（classify(1) = 200）");
    free(again);
    lainir_builder_free(b2);
  }

  /* canonical 形状的抽查 */
  check(strstr(canonical, "data bytes ro { 7 8 9 }") != NULL,
        "canonical 里有数据对象");
  check(strstr(canonical, "%r = #add[#bits<64>](40, 2)") != NULL,
        "canonical 里类型实参总是写出来");
  check(strstr(canonical, "%sum = #loop bytes(%i: #bits<64> = 0") != NULL,
        "canonical 里循环头带参数与初值");
  /* #switch 的选择子类型实参必须打出来：解析器要求它，往返靠它成立。 */
  check(strstr(canonical, "#switch[#bits<64>] %x -> (#bits<64>)") != NULL,
        "canonical 里 #switch 带选择子类型");

  /* 4. 输入糖 */
  {
    L1Builder *b3 = lainir_builder_new();
    L1Diagnostic d3;
    const L1Module *sugar;
    d3.code = 0;
    sugar = lainir_parse(b3, k_sugar, &d3);
    check(sugar != NULL, "带嵌套的文本解析成功");
    if (sugar) {
      check(lainir_verify(sugar, &d3) == 0, "展平后的模块通过验证");
      check(sugar->subroutines[0].body->inst_count == 3,
            "糖展平成 3 条指令（多出一条 #mul）");
      v = 0;
      check(run_bits(sugar, "sugar", 0, 0, 0, 0xFFFFFFFFu, &v) == 0 && v == 42,
            "糖展平后语义不变：sugar() = 42");
    } else {
      printf("     diag: %d:%u %s\n", d3.code, d3.line, d3.message);
    }
    lainir_builder_free(b3);
  }

  /* 5. 语法错误要给出位置 */
  {
    L1Builder *b4 = lainir_builder_new();
    L1Diagnostic d4;
    const L1Module *bad;
    d4.code = 0;
    bad = lainir_parse(b4, "#proc f() -> #bits<64> {\n  %r = #nosuchop()\n}\n",
                       &d4);
    check(bad == NULL && d4.code == 3002 && d4.line == 2,
          "未知算子被拒，行号正确");
    lainir_builder_free(b4);
  }

  /* 6. 解析出来的模块能被 C 后端编译 */
  {
    static const uint32_t ints[4] = {8, 16, 32, 64};
    static const uint32_t floats[2] = {32, 64};
    LainTarget target;
    LainBackendSink sink;
    LainBackend *backend;
    target.name = "c";
    target.address_bits = 64;
    target.int_widths = ints;
    target.int_width_count = 4;
    target.float_formats = floats;
    target.float_format_count = 2;
    target.max_alignment = 16;
    target.unaligned_ok = true;
    target.little_endian = true;
    target.capability = LAINBC_CAP_SYMBOL;
    sink.write = sink_write;
    sink.symbol = sink_symbol;
    sink.user = NULL;
    g_out = fopen("build/tmp-probe/from_text.c", "wb");
    g_bytes = 0;
    backend = lainbackend_new(&target, &sink, &diag);
    check(lainbackend_emit(backend, m) == 0 && g_bytes > 0,
          "解析出来的模块能编成 C");
    if (diag.code) printf("     diag: %d %s\n", diag.code, diag.message);
    lainbackend_free(backend);
    if (g_out) fclose(g_out);
    g_out = NULL;
  }

  /* 7. `#eval` 的文本形式：与 `#call` 同一个指令种类，只是拼写与标记不同。
   *    编译期执行本身由 fold 负责（fold_eval.c 覆盖），这里只管文本两端。 */
  {
    static const char k_eval[] =
        "#proc one(%a: #bits<64>) -> #bits<64> {\n"
        "  #return %a\n"
        "}\n"
        "\n"
        "#proc uses_eval() -> #bits<64> {\n"
        "  %v = #eval one(1)\n"
        "  #return %v\n"
        "}\n";
    static const char k_eval_dynamic[] =
        "#proc one(%a: #bits<64>) -> #bits<64> {\n"
        "  #return %a\n"
        "}\n"
        "\n"
        "#proc bad(%d: #bits<64>) -> #bits<64> {\n"
        "  %v = #eval one(%d)\n"
        "  #return %v\n"
        "}\n";
    L1Builder *b7 = lainir_builder_new();
    L1Diagnostic d7;
    const L1Module *me;

    d7.code = 0;
    me = lainir_parse(b7, k_eval, &d7);
    check(me != NULL, "`#eval one(1)` 解析成功");
    if (!me) {
      printf("     diag: %d:%u %s\n", d7.code, d7.line, d7.message);
      return 1;
    }
    check(lainir_verify(me, &d7) == 0, "`#eval` 模块通过验证");
    if (d7.code) printf("     diag: %d %s\n", d7.code, d7.message);
    {
      char *c7 = lainir_print_to_string(me);
      check(c7 != NULL && strstr(c7, "%v = #eval one(1)") != NULL,
            "规范文本是 `#eval one(1)`（不打印成行尾标记）");
      if (c7) {
        L1Builder *b8 = lainir_builder_new();
        L1Diagnostic d8;
        const L1Module *m8;
        d8.code = 0;
        m8 = lainir_parse(b8, c7, &d8);
        check(m8 != NULL, "`#eval` 的规范文本能再解析");
        if (m8) {
          char *c8 = lainir_print_to_string(m8);
          check(c8 != NULL && strcmp(c8, c7) == 0,
                "`#eval` 固定点：print(parse(print(m))) == print(m)");
          free(c8);
        } else {
          printf("     diag: %d:%u %s\n", d8.code, d8.line, d8.message);
        }
        lainir_builder_free(b8);
        free(c7);
      }
    }
    /* 实参必须是编译期已知的值：带名字的操作数被验证器拒（2012）。 */
    {
      L1Builder *b9 = lainir_builder_new();
      L1Diagnostic d9;
      const L1Module *mb;
      d9.code = 0;
      mb = lainir_parse(b9, k_eval_dynamic, &d9);
      check(mb != NULL, "带变量的 `#eval` 也能解析（拒绝发生在验证阶段）");
      if (mb) {
        int v9 = lainir_verify(mb, &d9);
        check(v9 != 0 && d9.code == 2012,
              "`#eval` 的动态实参被拒，码是 2012");
        if (d9.code != 2012)
          printf("     diag: %d %s\n", d9.code, d9.message);
      }
      lainir_builder_free(b9);
    }
    /* 拼写是精确匹配：近似拼写仍然是未知算子。 */
    {
      L1Builder *b10 = lainir_builder_new();
      L1Diagnostic d10;
      d10.code = 0;
      check(lainir_parse(b10,
                         "#proc f() -> #bits<64> {\n  %v = #evall f(1)\n"
                         "  #return %v\n}\n",
                         &d10) == NULL &&
                d10.code == 3002,
            "`#evall` 仍拒 3002（不做前缀匹配）");
      lainir_builder_free(b10);
    }
    lainir_builder_free(b7);
  }

  free(canonical);
  lainir_builder_free(b);
  printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
