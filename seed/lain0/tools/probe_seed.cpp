// 验证 lain0 的链接路径：C++ 是否能经 lain0/Seed.hpp 调用 seed 的 IR API 并
// 产出通过 lainir_verify 的 canonical 文本。
//
// 目标模块等价于：
//   data message ro { 104 105 0 }
//   #proc add(%a: #bits<32>, %b: #bits<32>) -> #bits<32> {
//     %sum = #add[#bits<32>](%a, %b)
//     #return %sum
//   }

#include "lain0/Seed.hpp"

#include <cstdio>
#include <cstdlib>

int main() {
  L1Builder *b = lainir_builder_new();
  if (!b) {
    std::fprintf(stderr, "FAIL: lainir_builder_new\n");
    return 1;
  }

  // 名字必须驻留进 builder 的 arena：API 不深拷贝字符串。
  const char *n_a = lainir_builder_string(b, "a");
  const char *n_b = lainir_builder_string(b, "b");
  const char *n_sum = lainir_builder_string(b, "sum");
  const char *n_add = lainir_builder_string(b, "add");
  const char *n_msg = lainir_builder_string(b, "message");
  const char *n_mod = lainir_builder_string(b, "probe");

  const L1Type *bits32 = lainir_type(b, TY_BITS, 32);

  L1Operand addOps[2] = {lainir_ref(n_a), lainir_ref(n_b)};
  const L1Inst *iAdd = lainir_inst(b, INST_ADD, n_sum, bits32, addOps, 2);

  L1Operand retOps[1] = {lainir_ref(n_sum)};
  const L1Inst *iRet = lainir_inst_jump(b, INST_RETURN, nullptr, retOps, 1);

  const L1Inst *bodyInsts[2] = {iAdd, iRet};
  const L1Region *body = lainir_region(b, nullptr, 0, nullptr, 0, bodyInsts, 2);

  L1Param params[2] = {lainir_proc_param(n_a, bits32),
                       lainir_proc_param(n_b, bits32)};
  const L1Type *results[1] = {bits32};
  const L1Subroutine *sub =
      lainir_subroutine(b, n_add, params, 2, results, 1, body);

  const uint8_t msgBytes[3] = {104, 105, 0};
  const L1Data *data = lainir_data(b, n_msg, msgBytes, 3, false);

  L1Subroutine subs[1] = {*sub};
  const L1Module *mod = lainir_module(b, n_mod, data, 1, subs, 1);

  L1Diagnostic diag;
  diag.code = 0;
  diag.line = 0;
  diag.column = 0;
  diag.message[0] = '\0';

  int rc = lainir_verify(mod, &diag);
  std::printf("verify rc=%d code=%d line=%u col=%u msg=%s\n", rc, diag.code,
              diag.line, diag.column, diag.message);

  char *text = lainir_print_to_string(mod);
  std::printf("--- canonical text ---\n%s", text ? text : "(null)\n");

  std::free(text);
  lainir_builder_free(b);
  return rc == 0 ? 0 : 1;
}
