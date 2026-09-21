/* `#eval` 的验收驱动（IR 层）。
 *
 * 用法：
 *     eval_checks.exe --list
 *     eval_checks.exe --case NAME [--expect-verify CODE] [--expect-trap CODE]
 *                     [--expect-text STR]
 *
 * 每个用例自己声明期望结果；命令行可以**覆盖**期望，用来做负对照：
 *     --case block_ok --expect-verify 9999     必须失败（否则断言没生效）
 *     --case block_addr_result --expect-verify 2019   必须失败（2030 ≠ 2019）
 *
 * 退出码：0 通过 / 1 失败 / 2 参数错误。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lainbackend/emit.h"
#include "lainir/build.h"
#include "lainir/infer.h"
#include "lainir/parse.h"
#include "lainir/print.h"
#include "lainir/verify.h"
#include "lainvm/engine.h"
#include "lainvm/image.h"

typedef struct {
  int code;         /* 0 = 通过；非 0 = 稳定拒绝码（30xx 语法 / 2xxx 验证 / 92xx 后端 / 1xxx trap） */
  const char *note; /* 失败说明 */
  char *text;       /* 规范文本（调用方 free），可为空 */
} EvalResult;

static EvalResult pass_result(char *text) {
  EvalResult r;
  r.code = 0;
  r.note = NULL;
  r.text = text;
  return r;
}

static EvalResult fail_result(int code, const char *note) {
  EvalResult r;
  r.code = code;
  r.note = note;
  r.text = NULL;
  return r;
}

/* --- 语料 ----------------------------------------------------------------- */

/* 正例：块声明 #bits<8>，块里 #return 一个字面量（宽度由声明给），块的结果再
 * 被 #zext 成 #bits<64>。这一条同时压住「结果类型来自块的声明」。 */
static const char k_block_ok[] =
    "#proc main() -> #bits<64> {\n"
    "  %v = #eval -> (#bits<8>) {\n"
    "    #return 7\n"
    "  }\n"
    "  %w = #zext[#bits<64>](%v)\n"
    "  #return %w\n"
    "}\n";

/* 反例：块里没有 #return（区域顺序结束）。 */
static const char k_block_no_return[] =
    "#proc main() -> #bits<64> {\n"
    "  %v = #eval -> (#bits<8>) {\n"
    "    %a = #add[#bits<8>](3, 4)\n"
    "  }\n"
    "  %w = #zext[#bits<64>](%v)\n"
    "  #return %w\n"
    "}\n";

/* 反例：块声明 #bits<8>，但 #return 交的是 #bits<64> 的值。 */
static const char k_block_bad_return[] =
    "#proc main(%x: #bits<64>) -> #bits<64> {\n"
    "  %v = #eval -> (#bits<8>) {\n"
    "    #return %x\n"
    "  }\n"
    "  %w = #zext[#bits<64>](%v)\n"
    "  #return %w\n"
    "}\n";

/* 反例：块的结果类型是 #addr —— 编译期地址不许进入产物。 */
static const char k_block_addr_result[] =
    "#proc main() -> #bits<64> {\n"
    "  %v = #eval -> (#addr) {\n"
    "    #return 0\n"
    "  }\n"
    "  #return 0\n"
    "}\n";

/* 调用形态（S1）：`#eval f(1)` 与 `#call` 同一条路径，只是带上编译期标记。 */
static const char k_eval_call[] =
    "#proc one(%a: #bits<64>) -> #bits<64> {\n"
    "  #return %a\n"
    "}\n"
    "\n"
    "#proc main() -> #bits<64> {\n"
    "  %v = #eval one(1)\n"
    "  #return %v\n"
    "}\n";

/* --- 助手 ----------------------------------------------------------------- */

/* 解析 + 验证；通过时给出规范文本（调用方 free），失败时返回拒绝码。 */
static int parse_verify(const char *src, char **canon_out, const char **note) {
  L1Builder *b = lainir_builder_new();
  L1Diagnostic d;
  const L1Module *m;
  char *canon;
  d.code = 0;
  m = lainir_parse(b, src, &d);
  if (!m) {
    *note = "解析失败";
    lainir_builder_free(b);
    return d.code ? d.code : 3002;
  }
  if (lainir_verify(m, &d) != 0) {
    *note = "验证失败";
    lainir_builder_free(b);
    return d.code;
  }
  canon = lainir_print_to_string(m);
  lainir_builder_free(b);
  if (!canon) {
    *note = "打印失败";
    return -1;
  }
  *canon_out = canon;
  return 0;
}

/* 找到入口过程的第 index 条指令。 */
static const L1Inst *inst_of(const L1Module *m, const char *name, uint32_t index) {
  const L1Subroutine *sub = lainir_find_subroutine(m, name);
  if (!sub || !sub->body || index >= sub->body->inst_count) return NULL;
  return &sub->body->insts[index];
}

/* 规范文本的固定点：print(parse(print(m))) == print(m)。 */
static bool fixpoint_holds(const char *canon) {
  L1Builder *b = lainir_builder_new();
  L1Diagnostic d;
  const L1Module *again;
  char *twice;
  bool ok;
  d.code = 0;
  again = lainir_parse(b, canon, &d);
  if (!again) {
    lainir_builder_free(b);
    return false;
  }
  twice = lainir_print_to_string(again);
  ok = twice != NULL && strcmp(twice, canon) == 0;
  free(twice);
  lainir_builder_free(b);
  return ok;
}

/* --- 用例 ----------------------------------------------------------------- */

static EvalResult case_block_ok(void) {
  char *canon = NULL;
  const L1Inst *inst;
  const char *note = "?";
  int code = parse_verify(k_block_ok, &canon, &note);
  L1Builder *b;
  L1Diagnostic d;
  const L1Module *m;
  if (code != 0) return fail_result(code, note);

  b = lainir_builder_new();
  d.code = 0;
  m = lainir_parse(b, k_block_ok, &d);
  inst = m ? inst_of(m, "main", 0) : NULL;
  if (!inst || inst->kind != INST_EVAL || !inst->body ||
      inst->body->result_count != 1 || !inst->body->results[0] ||
      inst->body->results[0]->kind != TY_BITS ||
      lainir_type_width(inst->body->results[0]) != 8) {
    lainir_builder_free(b);
    free(canon);
    return fail_result(-1, "第一条指令不是带 #bits<8> 声明的 INST_EVAL");
  }
  lainir_builder_free(b);

  if (!strstr(canon, "#eval -> (#bits<8>) {")) {
    free(canon);
    return fail_result(-1, "规范文本不是 `#eval -> (#bits<8>) {`");
  }
  if (!fixpoint_holds(canon)) {
    free(canon);
    return fail_result(-1, "固定点不成立");
  }
  return pass_result(canon);
}

static EvalResult case_eval_call_ok(void) {
  char *canon = NULL;
  const char *note = "?";
  int code = parse_verify(k_eval_call, &canon, &note);
  L1Builder *b;
  L1Diagnostic d;
  const L1Module *m;
  const L1Inst *inst;
  if (code != 0) return fail_result(code, note);
  b = lainir_builder_new();
  d.code = 0;
  m = lainir_parse(b, k_eval_call, &d);
  inst = m ? inst_of(m, "main", 0) : NULL;
  if (!inst || inst->kind != INST_CALL || !inst->is_eval) {
    lainir_builder_free(b);
    free(canon);
    return fail_result(-1, "调用形态不是 INST_CALL + is_eval");
  }
  lainir_builder_free(b);
  if (!strstr(canon, "%v = #eval one(1)")) {
    free(canon);
    return fail_result(-1, "规范文本不是 `#eval one(1)`");
  }
  if (!fixpoint_holds(canon)) {
    free(canon);
    return fail_result(-1, "固定点不成立");
  }
  return pass_result(canon);
}

/* 只解析 + 验证，期待被拒的用例共用一个实现。 */
static EvalResult verify_rejects(const char *src) {
  char *canon = NULL;
  const char *note = "?";
  int code = parse_verify(src, &canon, &note);
  free(canon);
  if (code == 0) return fail_result(-1, "本该被拒，却通过了验证");
  return fail_result(code, note);
}

static EvalResult case_block_no_return(void) {
  return verify_rejects(k_block_no_return);
}
static EvalResult case_block_bad_return(void) {
  return verify_rejects(k_block_bad_return);
}
static EvalResult case_block_addr_result(void) {
  return verify_rejects(k_block_addr_result);
}

/* 语法层：拼写必须精确。 */
static EvalResult case_unknown_opcode(void) {
  L1Builder *b = lainir_builder_new();
  L1Diagnostic d;
  const L1Module *m;
  d.code = 0;
  m = lainir_parse(b, "#proc f() -> #bits<64> {\n  %v = #nosuchop()\n"
                     "  #return %v\n}\n",
                   &d);
  lainir_builder_free(b);
  if (m) return fail_result(-1, "未知算子被接受了");
  return fail_result(d.code, "未知算子");
}

static EvalResult case_eval_typo(void) {
  L1Builder *b = lainir_builder_new();
  L1Diagnostic d;
  const L1Module *m;
  d.code = 0;
  m = lainir_parse(b, "#proc f() -> #bits<64> {\n  %v = #evall f(1)\n"
                     "  #return %v\n}\n",
                   &d);
  lainir_builder_free(b);
  if (m) return fail_result(-1, "`#evall` 被接受了（前缀匹配？）");
  return fail_result(d.code, "`#evall`");
}

/* 引擎不该见到块：必须稳定拒绝，不是静默执行。 */
static int run_trap(const char *src, const char *entry, int *trap_code) {
  L1Builder *b = lainir_builder_new();
  L1Diagnostic d;
  const L1Module *m;
  LainVmSpace space;
  LainVmImage *image;
  LainVmTcb *tcb;
  LainVmSliceResult result;
  int rc = -1;
  d.code = 0;
  m = lainir_parse(b, src, &d);
  if (!m || lainir_verify(m, &d) != 0) {
    lainir_builder_free(b);
    return d.code ? d.code : -1;
  }
  lainvm_space_init(&space);
  image = lainvm_image_load(m, &space, &d);
  if (!image) {
    lainir_builder_free(b);
    return d.code ? d.code : -1;
  }
  tcb = lainvm_tcb_new(image, &space, 1, 1, 64, lainvm_stack_no_lease(), NULL);
  if (tcb) {
    if (lainvm_tcb_start(tcb, entry, NULL, 0, &d) != 0) {
      rc = d.code ? d.code : -1;
    } else {
      result = lainvm_engine_run(tcb, 1000000);
      if (result == LAINVM_SLICE_TRAPPED) {
        *trap_code = tcb->trap.status;
        rc = tcb->trap.status;
      }
    }
    lainvm_tcb_free(tcb);
  }
  lainvm_image_free(image);
  lainir_builder_free(b);
  return rc;
}

static EvalResult case_block_engine_rejects(void) {
  int trap = 0;
  int rc = run_trap(k_block_ok, "main", &trap);
  if (rc > 0 && rc < 10000) return fail_result(rc, "引擎的 trap 码");
  return fail_result(rc, "引擎没有给出 trap 码");
}

/* 后端也不该见到块。 */
static void sink_write(void *user, const char *bytes, uint32_t size) {
  (void)user;
  (void)bytes;
  (void)size;
}
static void sink_symbol(void *user, const char *symbol, bool is_extern) {
  (void)user;
  (void)symbol;
  (void)is_extern;
}

static EvalResult case_block_backend_rejects(void) {
  static const uint32_t ints[4] = {8, 16, 32, 64};
  static const uint32_t floats[2] = {32, 64};
  L1Builder *b = lainir_builder_new();
  L1Diagnostic d;
  const L1Module *m;
  LainTarget target;
  LainBackendSink sink;
  LainBackend *backend;
  int rc;
  d.code = 0;
  m = lainir_parse(b, k_block_ok, &d);
  if (!m || lainir_verify(m, &d) != 0) {
    lainir_builder_free(b);
    return fail_result(d.code ? d.code : -1, "解析/验证失败");
  }
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
  backend = lainbackend_new(&target, &sink, &d);
  rc = lainbackend_emit(backend, m);
  lainbackend_free(backend);
  lainir_builder_free(b);
  if (rc == 0) return fail_result(-1, "后端接受了 #eval 块");
  return fail_result(d.code ? d.code : -1, "后端拒绝码");
}

/* --- 用例表 --------------------------------------------------------------- */

typedef EvalResult (*EvalCaseFn)(void);

typedef struct {
  const char *name;
  EvalCaseFn fn;
  int want_code;        /* 默认期望的码（0 = 通过） */
  const char *want_text;/* 非空时：期望规范文本里含这个串 */
} EvalCase;

static const EvalCase k_cases[] = {
    {"block_ok", case_block_ok, 0, "#eval -> (#bits<8>) {"},
    {"eval_call_ok", case_eval_call_ok, 0, "%v = #eval one(1)"},
    {"block_no_return", case_block_no_return, 2006, NULL},
    {"block_bad_return", case_block_bad_return, 2019, NULL},
    {"block_addr_result", case_block_addr_result, 2030, NULL},
    {"block_engine_rejects", case_block_engine_rejects, 1045, NULL},
    {"block_backend_rejects", case_block_backend_rejects, 9225, NULL},
    {"unknown_opcode", case_unknown_opcode, 3002, NULL},
    {"eval_typo", case_eval_typo, 3002, NULL},
};

#define CASE_COUNT (sizeof(k_cases) / sizeof(k_cases[0]))

int main(int argc, char **argv) {
  const char *name = NULL;
  const char *expect_text = NULL;
  int expect_code = -1;
  uint32_t i;

  setvbuf(stdout, NULL, _IONBF, 0);

  for (i = 1; i < (uint32_t)argc; i++) {
    if (strcmp(argv[i], "--list") == 0) {
      uint32_t k;
      for (k = 0; k < CASE_COUNT; k++) printf("%s\n", k_cases[k].name);
      return 0;
    } else if (strcmp(argv[i], "--case") == 0 && i + 1 < (uint32_t)argc) {
      name = argv[++i];
    } else if ((strcmp(argv[i], "--expect-verify") == 0 ||
                strcmp(argv[i], "--expect-trap") == 0) &&
               i + 1 < (uint32_t)argc) {
      expect_code = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--expect-text") == 0 &&
               i + 1 < (uint32_t)argc) {
      expect_text = argv[++i];
    } else {
      printf("用法：eval_checks.exe --list | --case NAME [--expect-verify CODE]"
             " [--expect-trap CODE] [--expect-text STR]\n");
      return 2;
    }
  }
  if (!name) {
    printf("用法：eval_checks.exe --list | --case NAME [...]\n");
    return 2;
  }

  for (i = 0; i < CASE_COUNT; i++) {
    const EvalCase *c = &k_cases[i];
    EvalResult r;
    int want;
    if (strcmp(c->name, name) != 0) continue;
    r = c->fn();
    want = expect_code >= 0 ? expect_code : c->want_code;
    if (expect_text) {
      if (r.code == 0 && r.text && strstr(r.text, expect_text)) {
        printf("PASS %s（文本里含 `%s`）\n", name, expect_text);
        free(r.text);
        return 0;
      }
      printf("FAIL %s：期望文本里含 `%s`，实际 code=%d %s\n", name, expect_text,
             r.code, r.note ? r.note : "");
      free(r.text);
      return 1;
    }
    if (r.code == want) {
      if (want == 0)
        printf("PASS %s（通过）\n", name);
      else
        printf("PASS %s（拒 %d）\n", name, want);
      free(r.text);
      return 0;
    }
    printf("FAIL %s：期望 %d，实际 %d %s\n", name, want, r.code,
           r.note ? r.note : "");
    free(r.text);
    return 1;
  }
  printf("FAIL 没有这个用例：%s\n", name);
  return 2;
}
