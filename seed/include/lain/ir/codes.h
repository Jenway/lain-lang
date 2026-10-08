/* 诊断码号段注册表。
 *
 * 一个诊断码是四位十进制数，两套机制共用同一片号码空间：
 *   - Trap：VM 拒绝执行，码写进 LainVmTrap.status，调用方看 LainVmSliceResult；
 *   - 拒绝码：函数返回 L1Diagnostic，失败时把码写进 diag。
 * 号码只属于一个模块：新码落在自己的段里，退休的号码不复用。段内边界由各模块
 * 自己的头文件断言（见 lain/vm/trap.h、lain/vm/image.h、lain/vm/tcb.h）。
 *
 *   1xxx  Trap（vm/engine 发，vm/quota 借 1044）      lain/vm/trap.h
 *   2xxx  验证码（ir/verify）                        lain/ir/verify.h
 *   3xxx  文本解析拒绝码（text/parse）                尚无正式码表
 *   90xx  映像装载拒绝码（vm/image）                  lain/vm/image.h
 *   91xx  TCB 拒绝码（vm/tcb）                       lain/vm/tcb.h
 *   92xx  后端拒绝码（backend/cbackend）             尚无正式码表
 *   93xx  apply 拒绝码（meta/apply 与其宿主）         lain/meta/apply.h
 *   94xx  AstOut 与 Meta 库共享码                    lain/meta/ast_v1.h、bootstrap/std 下的 .l1
 *
 * 93xx 与 94xx 同时被 C 侧和 Meta 侧（bootstrap/std 的 .l1）读取，是跨语言契约：
 * 只能新增，不能改义，也不能挪号。
 */
#ifndef LAINIR_CODES_H
#define LAINIR_CODES_H

enum {
  LAINIR_CODES_TRAP_BASE = 1000,
  LAINIR_CODES_TRAP_LIMIT = 1199,
  LAINIR_CODES_VERIFY_BASE = 2000,
  LAINIR_CODES_VERIFY_LIMIT = 2999,
  LAINIR_CODES_TEXT_BASE = 3000,
  LAINIR_CODES_TEXT_LIMIT = 3999,
  LAINIR_CODES_IMAGE_BASE = 9000,
  LAINIR_CODES_IMAGE_LIMIT = 9099,
  LAINIR_CODES_TCB_BASE = 9100,
  LAINIR_CODES_TCB_LIMIT = 9199,
  LAINIR_CODES_BACKEND_BASE = 9200,
  LAINIR_CODES_BACKEND_LIMIT = 9299,
  LAINIR_CODES_APPLY_BASE = 9300,
  LAINIR_CODES_APPLY_LIMIT = 9399,
  LAINIR_CODES_META_BASE = 9400,
  LAINIR_CODES_META_LIMIT = 9499,
};

#endif /* LAINIR_CODES_H */
