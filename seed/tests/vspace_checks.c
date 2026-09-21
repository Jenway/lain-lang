/* VSpace / LAINVM 边界用例驱动（交付 A：先建立可执行证据）。
 *
 * 这不是 VM 的一部分，也不改 VM。它调公开接口，把「据说有边界」的地方逐条测出来。
 *
 *   用法：
 *     build/vspace_checks.exe --list                     列出用例（id + group）
 *     build/vspace_checks.exe                            跑全部
 *     build/vspace_checks.exe --group region             只跑一组
 *     build/vspace_checks.exe --case region_valid_read   只跑一条
 *     build/vspace_checks.exe --case X --expect-value 99 覆盖期望值（负对照）
 *     build/vspace_checks.exe --case Y --expect-trap 9999 覆盖期望码（负对照）
 *
 *   每条用例一行，机器可解析：
 *     CASE <id> group=<g> expect=<spec> actual=<spec> result=<PASS|FAIL|BLOCKED> detail=<text>
 *   末行：SUMMARY total=N pass=N fail=N blocked=N
 *
 *   退出码：0 = 没有 FAIL 且没有 BLOCKED；1 = 有 FAIL 或 BLOCKED；2 = 用法错误。
 *   单用例模式下只看那一条。
 *
 * 两条纪律（方案 §3.2）：
 *   1. 不依赖两次 malloc 的偶然地址顺序 —— 需要「低地址插入」时，直接在已知基址的
 *      正下方**登记**一段合成区段（只登记、不解引用，所以不会碰非法内存）。
 *   2. 不拿宿主崩溃当期望结果 —— 越界读发生在一块真实分配的大缓冲上，长度只超一字节。
 *
 * 期望值语义：PASS 只表示**当前行为与期望一致**。期望是「按契约应当如此」，不是「现在这样就对」。
 * 因此 FAILED 的那些正是要交给作者定夺的缺口；不要为了变绿而改期望。
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lainir/build.h"
#include "lainir/parse.h"
#include "lainir/verify.h"
#include "lainmeta/host.h"
#include "lainvm/caps.h"
#include "lainvm/engine.h"
#include "lainvm/image.h"
#include "lainvm/memcap.h"
#include "lainvm/space.h"
#include "lainvm/tcb.h"

/* --- 观察结果 ---------------------------------------------------------------
 * kind 0 = 得到值；1 = 得到拒绝（trap / 非零状态）；2 = 无法判定（附带事实）。
 */
typedef struct {
  int kind;
  uint64_t value;
  int32_t trap_code;
  int trap_kind;
  char detail[224];
} Observation;

static Observation g_obs;

static void obs_value(uint64_t value) {
  memset(&g_obs, 0, sizeof(g_obs));
  g_obs.kind = 0;
  g_obs.value = value;
}

static void obs_trap(int trap_kind, int32_t code) {
  memset(&g_obs, 0, sizeof(g_obs));
  g_obs.kind = 1;
  g_obs.trap_kind = trap_kind;
  g_obs.trap_code = code;
}

static void obs_facts(const char *fmt, ...);

static void obs_facts(const char *fmt, ...) {
  va_list ap;
  memset(&g_obs, 0, sizeof(g_obs));
  g_obs.kind = 2;
  va_start(ap, fmt);
  vsnprintf(g_obs.detail, sizeof(g_obs.detail), fmt, ap);
  va_end(ap);
}

/* --- 用例表 ----------------------------------------------------------------- */
typedef enum {
  EXP_VALUE = 0,   /* 期望得到一个值 */
  EXP_TRAP = 1,    /* 期望被拒；code 0 = 只要求「拒」 */
  EXP_BLOCKED = 2, /* 契约未定或实现不存在 —— 只记录现状，不算通过 */
} ExpectKind;

typedef struct {
  const char *id;
  const char *group;
  ExpectKind expect;
  uint64_t expect_value;
  int32_t expect_code;
  int (*run)(void);
} Case;

/* --- 小的公共工具 ----------------------------------------------------------- */

/* 一块真实分配的大缓冲，用来切出低/高地址段，插入顺序可控。 */
static uint8_t *new_buffer(size_t bytes) {
  uint8_t *p = (uint8_t *)malloc(bytes);
  if (p) memset(p, 0x5A, bytes);
  return p;
}

/* 只关心"登记成没成"的用例用这个：0 = 成功，-1 = 失败。
 * 测试用的缓冲都是**外部借入**（测试自己 malloc），所以走 external mapping：
 * 整段立刻可访问，撤销时不 free、不归还 quota。 */
static int add(LainVmSpace *space, uintptr_t base, uint64_t size,
               uint32_t rights, uint64_t owner) {
  return lainvm_space_handle_none(lainvm_space_map_external(
             space, base, size, size, rights, owner))
             ? -1
             : 0;
}

/* 需要句柄的用例（"这个引用还指得到原对象吗"）用这个。 */
static LainVmRegionHandle add_handle(LainVmSpace *space, uintptr_t base,
                                     uint64_t size, uint32_t rights,
                                     uint64_t owner) {
  return lainvm_space_map_external(space, base, size, size, rights, owner);
}

/* 句柄现在指向哪个 base；无效 / 已撤销 → 0。 */
static uintptr_t base_of(const LainVmSpace *space, LainVmRegionHandle handle) {
  const LainVmRegion *region = lainvm_space_slot(space, handle);
  return region ? region->base : (uintptr_t)0;
}

/* --- 装载 + 执行的台架 ------------------------------------------------------ */
typedef struct {
  L1Builder *builder;
  LainVmSpace space;
  LainVmImage *image;
  LainVmTcb *tcb;
  int32_t fail_code; /* 前置失败时的诊断码（parse/verify/load 各自的值域） */
} Rig;

static int rig_load(Rig *rig, const char *text, uint64_t stack_bytes) {
  L1Diagnostic diag;
  const L1Module *module;

  memset(rig, 0, sizeof(*rig));
  rig->builder = lainir_builder_new();
  diag.code = 0;
  module = lainir_parse(rig->builder, text, &diag);
  if (!module) {
    rig->fail_code = diag.code;
    obs_facts("parse: code=%d %s", diag.code, diag.message);
    return -1;
  }
  if (lainir_verify(module, &diag) != 0) {
    rig->fail_code = diag.code;
    obs_facts("verify: code=%d %s", diag.code, diag.message);
    return -2;
  }
  lainvm_space_init(&rig->space);
  rig->image = lainvm_image_load(module, &rig->space, &diag);
  if (!rig->image) {
    rig->fail_code = diag.code;
    obs_facts("load: code=%d %s", diag.code, diag.message);
    return -3;
  }
  rig->tcb = lainvm_tcb_new(rig->image, &rig->space, 1, 1, 64, stack_bytes, NULL);
  if (!rig->tcb) {
    if (diag.code != 0) rig->fail_code = diag.code;
    obs_facts("admit 失败（stack_bytes=%llu）", (unsigned long long)stack_bytes);
    return -4;
  }
  return 0;
}

static void rig_free(Rig *rig) {
  if (rig->tcb) lainvm_tcb_free(rig->tcb);
  if (rig->image) lainvm_image_free(rig->image);
  if (rig->builder) lainir_builder_free(rig->builder);
}

/* 跑一个入口；返回 0 = 起来了（结果写进 g_obs），非 0 = 前置失败（g_obs 里是事实）。 */
static int rig_run(Rig *rig, const char *entry) {
  L1Diagnostic diag;
  LainVmSliceResult slice;

  diag.code = 0;
  if (lainvm_tcb_start(rig->tcb, entry, NULL, 0, &diag) != 0) {
    obs_facts("start: code=%d %s", diag.code, diag.message);
    return -1;
  }
  slice = lainvm_engine_run(rig->tcb, 1000000);
  if (slice == LAINVM_SLICE_TRAPPED) {
    obs_trap((int)rig->tcb->trap.kind, rig->tcb->trap.status);
    return 0;
  }
  if (slice != LAINVM_SLICE_DONE) {
    obs_facts("slice=%d（既没跑完也没 trap）", (int)slice);
    return 0;
  }
  if (!rig->tcb->has_result) {
    obs_facts("跑完了但没有结果值");
    return 0;
  }
  obs_value(rig->tcb->result.as.bits);
  return 0;
}

/* --- LAINIR 测试程序（内联；都是本用例专用的小程序） ---------------------- */

/* 一个从来没登记过的地址上读一个字节：走 `#lea` 构造（data 符号 + 偏移）。
 * D3 已定：**构造不查、访问查** —— 所以构造成功，随后 load 必须被 VSpace 拒。 */
static const char *k_prog_outside_region =
    "data bytes ro { 42 }\n"
    "#proc outside() -> #bits<64> {\n"
    "  %base = #data_addr bytes\n"
    "  %p = #lea(%base, 0, 1, 1000000)\n"
    "  %b = #load[#bits<8>](%p)\n"
    "  %w = #zext[#bits<64>](%b)\n"
    "  #return %w\n"
    "}\n";

/* `#int2ptr` 只做**位模式转换**：整数 4096 变成一个数值为 4096 的裸地址，
 * 不授予任何访问权。所以 load 会在 VSpace 那一步被拒（1004）——
 * 不是"假句柄/假代数"被拒（`#addr` 里没有身份可查）。 */
static const char *k_prog_int2ptr_bare =
    "#proc from_int() -> #bits<64> {\n"
    "  %p = #int2ptr[#addr](4096)\n"
    "  %b = #load[#bits<8>](%p)\n"
    "  %w = #zext[#bits<64>](%b)\n"
    "  #return %w\n"
    "}\n";

/* `#addr` 槽是**单字裸地址**：存进去什么数值，读回来就是什么数值（没有 tag 字、
 * 没有代数）。data 地址读回 42，alloca 地址读回 9，和 = 51。 */
static const char *k_prog_addr_slot_roundtrip =
    "data bytes ro { 42 }\n"
    "data slot rw { 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 }\n"
    "#proc two_flavors() -> #bits<64> {\n"
    "  %slotp = #data_addr slot\n"
    "  %raw = #data_addr bytes\n"
    "  #store[#addr](%raw, %slotp)\n"
    "  %back = #load[#addr](%slotp)\n"
    "  %b = #load[#bits<8>](%back)\n"
    "  %w = #zext[#bits<64>](%b)\n"
    "  %a = #alloca[#bits<8>](1)\n"
    "  #store[#bits<8>](9, %a)\n"
    "  #store[#addr](%a, %slotp)\n"
    "  %back2 = #load[#addr](%slotp)\n"
    "  %v = #load[#bits<8>](%back2)\n"
    "  %w2 = #zext[#bits<64>](%v)\n"
    "  %sum = #add[#bits<64>](%w, %w2)\n"
    "  #return %sum\n"
    "}\n";

/* 没有栈区段（stack_bytes = 0）却用 #alloca。期望：拒绝（当前 1006）。 */
static const char *k_prog_alloca =
    "#proc use_alloca() -> #bits<64> {\n"
    "  %s = #alloca[#bits<64>](1)\n"
    "  #store[#bits<64>](7, %s)\n"
    "  %v = #load[#bits<64>](%s)\n"
    "  #return %v\n"
    "}\n";

/* 两次 alloca，第二次要的比剩下的多。期望：第二次拒绝，且水位不变。 */
static const char *k_prog_alloca_exhaust =
    "#proc two_alloca() -> #bits<64> {\n"
    "  %a = #alloca[#bits<64>](1)\n"
    "  #store[#bits<64>](7, %a)\n"
    "  %b = #alloca[#bits<64>](4)\n"
    "  #store[#bits<64>](9, %b)\n"
    "  #return 0\n"
    "}\n";

/* count * 元素大小 溢出成 0：8 * 2^61 == 2^64 == 0。期望：拒绝。 */
static const char *k_prog_alloca_overflow =
    "#proc alloca_overflow() -> #bits<64> {\n"
    "  %p = #alloca[#bits<64>](0x2000000000000000)\n"
    "  #store[#bits<64>](1, %p)\n"
    "  #return 0\n"
    "}\n";

/* 零 count：现在被当成 1。期望未定（要核 LAINIR 规范）。 */
static const char *k_prog_alloca_zero =
    "#proc alloca_zero() -> #bits<64> {\n"
    "  %p = #alloca[#bits<64>](0)\n"
    "  #store[#bits<64>](7, %p)\n"
    "  %v = #load[#bits<64>](%p)\n"
    "  #return %v\n"
    "}\n";

/* R06：调用方拿回那个地址去读。期望：拒绝（activation 已经结束）。 */
static const char *k_prog_escape =
    "data leaked rw { 0 0 0 0 0 0 0 0 }\n"
    "#proc give() -> #bits<64> {\n"
    "  %s = #alloca[#bits<64>](1)\n"
    "  #store[#bits<64>](7, %s)\n"
    "  %d = #data_addr leaked\n"
    "  %w = #ptr2int[#bits<64>](%s)\n"
    "  #store[#bits<64>](%w, %d)\n"
    "  #return 1\n"
    "}\n"
    "#proc use_after_return() -> #bits<64> {\n"
    "  %z = #call give()\n"
    "  %d = #data_addr leaked\n"
    "  %w = #load[#bits<64>](%d)\n"
    "  %p = #int2ptr[#addr](%w)\n"
    "  %v = #load[#bits<64>](%p)\n"
    "  #return %v\n"
    "}\n";

/* R07：同一地址被发两次吗？返回 1 = 两次相同（旧引用无法区分）。 */
static const char *k_prog_reuse =
    "data leaked rw { 0 0 0 0 0 0 0 0 }\n"
    "#proc give() -> #bits<64> {\n"
    "  %s = #alloca[#bits<64>](1)\n"
    "  #store[#bits<64>](7, %s)\n"
    "  %d = #data_addr leaked\n"
    "  %w = #ptr2int[#bits<64>](%s)\n"
    "  #store[#bits<64>](%w, %d)\n"
    "  #return 1\n"
    "}\n"
    "#proc reuse() -> #bits<64> {\n"
    "  %z1 = #call give()\n"
    "  %d = #data_addr leaked\n"
    "  %w1 = #load[#bits<64>](%d)\n"
    "  %z2 = #call give()\n"
    "  %w2 = #load[#bits<64>](%d)\n"
    "  %same = #eq[#bits<64>](%w1, %w2)\n"
    "  %out = #zext[#bits<64>](%same)\n"
    "  #return %out\n"
    "}\n";

/* R07（作者校正后的断言）：**裸地址不可区分**。
 *
 * `give` 分配一块栈、写进自己的值，把地址记到模块级数据里；同时用参数 `probe`
 * ——一个**上一次**留下的数值地址——去读。第二次调用时 `probe` 正是第一次那段地址：
 * VSpace 已经把它重新授权给本次分配，于是读回来的必须是本次写的 9。
 * （第一次调用传的 probe 是 leaked 自己的地址，它有授权，读到的值没人用。） */
static const char *k_prog_reuse_guard =
    "data leaked rw { 0 0 0 0 0 0 0 0 }\n"
    "#proc give(%v: #bits<64>, %probe: #addr) -> #bits<64> {\n"
    "  %s = #alloca[#bits<64>](1)\n"
    "  #store[#bits<64>](%v, %s)\n"
    "  %d = #data_addr leaked\n"
    "  %w = #ptr2int[#bits<64>](%s)\n"
    "  #store[#bits<64>](%w, %d)\n"
    "  %x = #load[#bits<64>](%probe)\n"
    "  #return %x\n"
    "}\n"
    "#proc reuse_same_address() -> #bits<64> {\n"
    "  %d = #data_addr leaked\n"
    "  %z1 = #call give(7, %d)\n"
    "  %old = #load[#bits<64>](%d)\n"
    "  %p = #int2ptr[#addr](%old)\n"
    "  %v = #call give(9, %p)\n"
    "  #return %v\n"
    "}\n";

/* `#lea` 是**地址位模式算术**（D3 已定：构造不查、访问查，按地址宽度取模）。
 * 下面几个程序都从同一个 data 符号出发，把**构造出来的相对偏移**返回 —— 这样
 * "构造成功了"就变成一个可断言的数值，而不是靠"没崩"推断。 */
static const char *k_prog_lea_far =
    "data bytes ro { 42 }\n"
    "#proc lea_far() -> #bits<64> {\n"
    "  %b = #data_addr bytes\n"
    "  %q = #lea(%b, 0, 1, 1000000)\n"
    "  %w = #ptr2int[#bits<64>](%q)\n"
    "  %bw = #ptr2int[#bits<64>](%b)\n"
    "  %d = #sub[#bits<64>](%w, %bw)\n"
    "  #return %d\n"
    "}\n";

/* 同一个构造，但接着**访问**：区段外地址上的 load 必须被 VSpace 拒（1004）。 */
static const char *k_prog_lea_far_access =
    "data bytes ro { 42 }\n"
    "#proc lea_far_access() -> #bits<64> {\n"
    "  %b = #data_addr bytes\n"
    "  %q = #lea(%b, 0, 1, 1000000)\n"
    "  %v = #load[#bits<8>](%q)\n"
    "  %w = #zext[#bits<64>](%v)\n"
    "  #return %w\n"
    "}\n";

/* 尾后地址（one-past-end）：构造得出，偏移正好 1。 */
static const char *k_prog_lea_one_past =
    "data bytes ro { 42 }\n"
    "#proc lea_one_past() -> #bits<64> {\n"
    "  %b = #data_addr bytes\n"
    "  %q = #lea(%b, 0, 1, 1)\n"
    "  %w = #ptr2int[#bits<64>](%q)\n"
    "  %bw = #ptr2int[#bits<64>](%b)\n"
    "  %d = #sub[#bits<64>](%w, %bw)\n"
    "  #return %d\n"
    "}\n";

/* 尾后地址上的访问：一样要被拒（1004）。 */
static const char *k_prog_lea_one_past_access =
    "data bytes ro { 42 }\n"
    "#proc lea_one_past_access() -> #bits<64> {\n"
    "  %b = #data_addr bytes\n"
    "  %q = #lea(%b, 0, 1, 1)\n"
    "  %v = #load[#bits<8>](%q)\n"
    "  %w = #zext[#bits<64>](%v)\n"
    "  #return %w\n"
    "}\n";

/* 回绕是**定义好的**：idx = 2^63、scale = 2 → idx * scale = 2^64 ≡ 0（按地址宽度
 * 取模），所以构造出来的地址正好等于 base。不取模的话这个和根本表示不出来。 */
static const char *k_prog_lea_wrap =
    "data bytes ro { 42 }\n"
    "#proc lea_wrap() -> #bits<64> {\n"
    "  %b = #data_addr bytes\n"
    "  %q = #lea(%b, 0x8000000000000000, 2, 0)\n"
    "  %w = #ptr2int[#bits<64>](%q)\n"
    "  %bw = #ptr2int[#bits<64>](%b)\n"
    "  %d = #sub[#bits<64>](%w, %bw)\n"
    "  #return %d\n"
    "}\n";

/* 回绕落到**已授权**地址上之后照样正常访问（读到 42）：取模是算术，不是漏洞。 */
static const char *k_prog_lea_wrap_read =
    "data bytes ro { 42 }\n"
    "#proc lea_wrap_read() -> #bits<64> {\n"
    "  %b = #data_addr bytes\n"
    "  %q = #lea(%b, 0x8000000000000000, 2, 0)\n"
    "  %v = #load[#bits<8>](%q)\n"
    "  %w = #zext[#bits<64>](%v)\n"
    "  #return %w\n"
    "}\n";

/* --- activation 组（alloca 属于 procedure activation） ----------------------
 *
 * 契约（作者 2026-09-21 定）：`#alloca` 属于当前 procedure activation。
 * 进入 `#if` / `#loop` / `#switch` 等结构化区域**不**创建新的 alloca 生命周期；
 * 只有 procedure return、Trap、取消或 TCB 销毁才结束该 activation 的局部存储。
 *
 * 审计（修正前）：engine.c 的 3 处区域退出都在回退水位并收回窗口 ——
 *   leave_region（if / switch / loop 的区域帧退出）、op_break、op_continue。
 * 那等于把结构化区域退出当成了 alloca 生命周期结束。下面三条就是最小复现。 */

/* 1) `if` 里 alloca，离开 `if` 之后同一过程内访问：契约要求成功。 */
static const char *k_prog_if_survives =
    "#proc if_survives() -> #bits<64> {\n"
    "  %c = #eq[#bits<64>](1, 1)\n"
    "  %p = #if %c -> (#addr) {\n"
    "    %s = #alloca[#bits<8>](1)\n"
    "    #store[#bits<8>](42, %s)\n"
    "    #yield %s\n"
    "  } else {\n"
    "    %z = #int2ptr[#addr](0)\n"
    "    #yield %z\n"
    "  }\n"
    "  %v = #load[#bits<8>](%p)\n"
    "  %w = #zext[#bits<64>](%v)\n"
    "  #return %w\n"
    "}\n";

/* 2) loop 体内 alloca，经 `#continue` 之后用**上一轮**的地址读：契约要求成功
 * （不能因为区域控制流回退水位而产生悬空授权）。
 * 每轮 alloca 8 字节、水位不回收 —— 见 activation_loop_growth 记录的增长结果。 */
static const char *k_prog_loop_continue =
    "#proc loop_continue() -> #bits<64> {\n"
    "  %r = #loop it(%i: #bits<64> = 0, %prev: #bits<64> = 0, %acc: #bits<64> = 0)"
    " -> (#bits<64>) {\n"
    "    %done = #uge[#bits<64>](%i, 2)\n"
    "    #if %done {\n"
    "      #break it(%acc)\n"
    "    }\n"
    "    %s = #alloca[#bits<64>](1)\n"
    "    %n = #add[#bits<64>](%i, 11)\n"
    "    #store[#bits<64>](%n, %s)\n"
    "    %first = #eq[#bits<64>](%i, 0)\n"
    "    %acc2 = #if %first -> (#bits<64>) {\n"
    "      #yield %acc\n"
    "    } else {\n"
    "      %p = #int2ptr[#addr](%prev)\n"
    "      %v = #load[#bits<64>](%p)\n"
    "      #yield %v\n"
    "    }\n"
    "    %w = #ptr2int[#bits<64>](%s)\n"
    "    %i2 = #add[#bits<64>](%i, 1)\n"
    "    #continue it(%i2, %w, %acc2)\n"
    "  }\n"
    "  #return %r\n"
    "}\n";

/* 3) 被调过程 alloca，把地址当**返回值**交给调用方，调用方访问：契约要求拒 1004。 */
static const char *k_prog_callee_return =
    "data leaked rw { 0 0 0 0 0 0 0 0 }\n"
    "#proc give_addr() -> #bits<64> {\n"
    "  %s = #alloca[#bits<64>](1)\n"
    "  #store[#bits<64>](7, %s)\n"
    "  %w = #ptr2int[#bits<64>](%s)\n"
    "  #return %w\n"
    "}\n"
    "#proc use_returned() -> #bits<64> {\n"
    "  %w = #call give_addr()\n"
    "  %p = #int2ptr[#addr](%w)\n"
    "  %v = #load[#bits<64>](%p)\n"
    "  #return %v\n"
    "}\n";

/* 4) 根过程结束后访问：两次 activation 共用同一个 TCB 与模块数据。 */
static const char *k_prog_root_escape =
    "data leaked rw { 0 0 0 0 0 0 0 0 }\n"
    "#proc root_give() -> #bits<64> {\n"
    "  %s = #alloca[#bits<64>](1)\n"
    "  #store[#bits<64>](7, %s)\n"
    "  %d = #data_addr leaked\n"
    "  %w = #ptr2int[#bits<64>](%s)\n"
    "  #store[#bits<64>](%w, %d)\n"
    "  #return 1\n"
    "}\n"
    "#proc root_use() -> #bits<64> {\n"
    "  %d = #data_addr leaked\n"
    "  %w = #load[#bits<64>](%d)\n"
    "  %p = #int2ptr[#addr](%w)\n"
    "  %v = #load[#bits<64>](%p)\n"
    "  #return %v\n"
    "}\n";

/* 1) `if` 里 alloca，离开 `if` 后同一过程内访问：成功，读到 42。 */
static int case_activation_if_survives(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_if_survives, 4096) != 0) return 0;
  (void)rig_run(&rig, "if_survives");
  rig_free(&rig);
  return 0;
}

/* 2) loop 体内 alloca，经 `#continue` 后用上一轮地址读：成功，读到 11。 */
static int case_activation_loop_continue(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_loop_continue, 4096) != 0) return 0;
  (void)rig_run(&rig, "loop_continue");
  rig_free(&rig);
  return 0;
}

/* 3) 被调过程返回后访问它的 alloca：拒 1004。 */
static int case_activation_callee_return(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_callee_return, 4096) != 0) return 0;
  (void)rig_run(&rig, "use_returned");
  rig_free(&rig);
  return 0;
}

/* 4) 根过程结束（第一次 activation 完成）后，第二次 activation 访问旧地址：拒 1004。 */
static int case_activation_root_done(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_root_escape, 4096) != 0) return 0;
  (void)rig_run(&rig, "root_give");
  if (g_obs.kind != 0 || g_obs.value != 1) {
    rig_free(&rig);
    obs_facts("第一次 activation 没跑通：kind=%d value=%llu", g_obs.kind,
              (unsigned long long)g_obs.value);
    return 0;
  }
  (void)rig_run(&rig, "root_use"); /* 同 TCB、同模块数据、新的 activation */
  rig_free(&rig);
  return 0;
}

/* 5) Trap 之后活窗口必须归零 —— 等 VSpace 的 accessible 接口落地后补（见租约组）。 */

/* --- lease 组（容量 vs 可访问窗口；栈租约的用例也在这里） --------------------
 *
 * 数据模型：capacity 是存储字节数（占用/重叠看它），accessible 是当前可访问**前缀**
 * （访问判定看它）。窗口更新走 `lainvm_space_set_accessible`，句柄全程稳定。 */

/* 窗口可以放大：收回去的地址在放大之后又能访问。 */
static int case_owned_accessible_grow(void) {
  LainVmSpace space;
  LainVmRegionHandle h;
  uintptr_t b; /* owned 存储的 base 由 VSpace 给，不是本地的那个数组 */

  lainvm_space_init(&space);
  h = lainvm_space_alloc(&space, 64, 16, 64, LAINVM_MEM_READ, 9, NULL);
  if (lainvm_space_handle_none(h)) {
    obs_facts("owned 登记 64 字节失败");
    return 0;
  }
  b = lainvm_space_slot(&space, h)->base;
  if (!lainvm_space_set_accessible(&space, h, 16)) {
    obs_facts("把窗口收到 16 失败");
    lainvm_space_free(&space, h);
    return 0;
  }
  if (lainvm_space_check(&space, b + 32, 1, LAINVM_MEM_READ)) {
    obs_facts("窗口=16 时 +32 仍然可访问");
    lainvm_space_free(&space, h);
    return 0;
  }
  if (!lainvm_space_set_accessible(&space, h, 64)) {
    obs_facts("把窗口放大回 64 失败");
    lainvm_space_free(&space, h);
    return 0;
  }
  if (!lainvm_space_check(&space, b + 32, 1, LAINVM_MEM_READ)) {
    obs_facts("窗口放大到 64 之后 +32 仍不可访问");
    lainvm_space_free(&space, h);
    return 0;
  }
  lainvm_space_free(&space, h);
  obs_value(1);
  return 0;
}

/* 缩小之后被收回的那一段**立刻**访问不了；窗口内的还能访问；
 * capacity 与区段数都不变（窗口变化不是撤销+登记）。 */
static int case_owned_accessible_shrink_rejects_tail(void) {
  LainVmSpace space;
  LainVmRegionHandle h;
  const LainVmRegion *r;
  uintptr_t b;
  uint32_t before;

  lainvm_space_init(&space);
  h = lainvm_space_alloc(&space, 64, 16, 64, LAINVM_MEM_READ, 9, NULL);
  if (lainvm_space_handle_none(h)) {
    obs_facts("owned 登记 64 字节失败");
    return 0;
  }
  b = lainvm_space_slot(&space, h)->base;
  before = space.live_count;
  if (!lainvm_space_set_accessible(&space, h, 8)) {
    obs_facts("把窗口收到 8 失败");
    lainvm_space_free(&space, h);
    return 0;
  }
  r = lainvm_space_slot(&space, h);
  if (!r || r->accessible != 8 || r->capacity != 64) {
    obs_facts("窗口=8 之后记录不对：capacity=%llu accessible=%llu",
              r ? (unsigned long long)r->capacity : 0,
              r ? (unsigned long long)r->accessible : 0);
    lainvm_space_free(&space, h);
    return 0;
  }
  if (!lainvm_space_check(&space, b + 4, 4, LAINVM_MEM_READ)) {
    obs_facts("窗口内的 4 字节读被拒了");
    lainvm_space_free(&space, h);
    return 0;
  }
  if (lainvm_space_check(&space, b + 8, 1, LAINVM_MEM_READ)) {
    obs_facts("窗口外的 +8 仍可访问");
    lainvm_space_free(&space, h);
    return 0;
  }
  if (lainvm_space_check(&space, b + 4, 8, LAINVM_MEM_READ)) {
    obs_facts("跨出窗口的区间（+4..+12）仍可访问");
    lainvm_space_free(&space, h);
    return 0;
  }
  if (space.live_count != before) {
    obs_facts("窗口变化改动了区段数：%u -> %u", (unsigned)before,
              (unsigned)space.live_count);
    lainvm_space_free(&space, h);
    return 0;
  }
  lainvm_space_free(&space, h);
  obs_value(1);
  return 0;
}

/* 窗口不许超过容量：拒绝且 accessible 一点不变。 */
static int case_owned_accessible_over_capacity_reject(void) {
  LainVmSpace space;
  uint8_t buf[64];
  LainVmRegionHandle h;
  const LainVmRegion *r;

  memset(buf, 7, sizeof(buf));
  lainvm_space_init(&space);
  h = lainvm_space_alloc(&space, 64, 16, 64, LAINVM_MEM_READ, 9, NULL);
  if (lainvm_space_handle_none(h)) {
    obs_facts("登记 64 字节失败");
    return 0;
  }
  if (!lainvm_space_set_accessible(&space, h, 32)) {
    obs_facts("把窗口收到 32 失败");
    return 0;
  }
  if (lainvm_space_set_accessible(&space, h, 65)) {
    obs_facts("accessible=65 超过了 capacity=64 却被接受");
    return 0;
  }
  r = lainvm_space_slot(&space, h);
  if (!r || r->accessible != 32) {
    obs_facts("被拒之后 accessible 变了：%llu（期望 32）",
              r ? (unsigned long long)r->accessible : 0);
    return 0;
  }
  lainvm_space_free(&space, h);
  obs_value(1);
  return 0;
}

/* 坏句柄（无效 / 已撤销 / 跨空间）都要稳定拒绝，而且原区段不变。 */
static int case_owned_accessible_failure_unchanged(void) {
  LainVmSpace space;
  LainVmSpace other;
  uint8_t buf[64];
  LainVmRegionHandle h;
  const LainVmRegion *r;

  memset(buf, 7, sizeof(buf));
  lainvm_space_init(&space);
  lainvm_space_init(&other);
  h = lainvm_space_alloc(&space, 64, 16, 64, LAINVM_MEM_READ, 9, NULL);
  if (lainvm_space_handle_none(h)) {
    obs_facts("登记 64 字节失败");
    return 0;
  }
  if (!lainvm_space_set_accessible(&space, h, 16)) {
    obs_facts("把窗口收到 16 失败");
    return 0;
  }
  if (lainvm_space_set_accessible(&other, h, 32)) {
    obs_facts("跨空间句柄被接受了");
    return 0;
  }
  if (lainvm_space_set_accessible(&space, lainvm_space_no_handle(), 32)) {
    obs_facts("无句柄被接受了");
    return 0;
  }
  r = lainvm_space_slot(&space, h);
  if (!r || r->accessible != 16 || r->capacity != 64) {
    obs_facts("失败调用改动了区段：capacity=%llu accessible=%llu",
              r ? (unsigned long long)r->capacity : 0,
              r ? (unsigned long long)r->accessible : 0);
    return 0;
  }
  if (!lainvm_space_free(&space, h)) {
    obs_facts("撤销失败");
    return 0;
  }
  if (lainvm_space_set_accessible(&space, h, 8)) {
    obs_facts("已撤销的句柄还能改窗口");
    return 0;
  }
  obs_value(1);
  return 0;
}

/* --- owned storage：VSpace 申请、清零、登记、释放，并按**原账户**归还 -------- */

/* 新拿到的存储必须是**清零**的（不零就没有确定性，固定点比较会废）。 */
static int case_owned_alloc_zeroed(void) {
  LainVmSpace space;
  LainVmRegionHandle h;
  const LainVmRegion *r;
  uint64_t i;

  lainvm_space_init(&space);
  h = lainvm_space_alloc(&space, 64, 16, 64, LAINVM_MEM_READ, 9, NULL);
  r = lainvm_space_slot(&space, h);
  if (!r) {
    obs_facts("owned 分配失败");
    return 0;
  }
  for (i = 0; i < r->capacity; i++) {
    if (((const uint8_t *)r->base)[i] != 0) {
      obs_facts("分配出来的第 %llu 字节不是 0（capacity=%llu）",
                (unsigned long long)i, (unsigned long long)r->capacity);
      lainvm_space_free(&space, h);
      return 0;
    }
  }
  if (!lainvm_space_free(&space, h)) {
    obs_facts("释放失败");
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 恰好用完：limit == capacity 成功，used == capacity。 */
static int case_owned_alloc_quota_exact(void) {
  LainVmSpace space;
  LainVmQuota q;
  LainVmRegionHandle h;

  lainvm_space_init(&space);
  lainvm_quota_init(&q, 64);
  h = lainvm_space_alloc(&space, 64, 16, 64, LAINVM_MEM_READ, 9, &q);
  if (lainvm_space_handle_none(h)) {
    obs_facts("limit 刚好等于 capacity 却分配失败");
    return 0;
  }
  if (q.used != 64 || q.charges != 1) {
    obs_facts("恰好用完账目不对：used=%llu charges=%llu（期望 64 / 1）",
              (unsigned long long)q.used, (unsigned long long)q.charges);
    lainvm_space_free(&space, h);
    return 0;
  }
  lainvm_space_free(&space, h);
  obs_value(1);
  return 0;
}

/* 多一字节：拒绝、used 不变、**没有**登记任何区段、quota 记账为 rejected。 */
static int case_owned_alloc_quota_reject_unchanged(void) {
  LainVmSpace space;
  LainVmQuota q;
  LainVmRegionHandle h;
  uint32_t before;

  lainvm_space_init(&space);
  lainvm_quota_init(&q, 64);
  before = space.live_count;
  h = lainvm_space_alloc(&space, 65, 16, 65, LAINVM_MEM_READ, 9, &q);
  if (!lainvm_space_handle_none(h)) {
    obs_facts("余额不够却分配成功了");
    return 0;
  }
  if (q.used != 0 || q.rejected != 1) {
    obs_facts("被拒之后账目动了：used=%llu rejected=%llu（期望 0 / 1）",
              (unsigned long long)q.used, (unsigned long long)q.rejected);
    return 0;
  }
  if (space.live_count != before) {
    obs_facts("被拒之后区段数变了：%u -> %u", (unsigned)before,
              (unsigned)space.live_count);
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 预扣成功而底层分配失败：账目必须回滚（用 2^60 字节让 calloc 必然失败）。 */
static int case_owned_alloc_failure_rolls_back_quota(void) {
  LainVmSpace space;
  LainVmQuota q;
  LainVmRegionHandle h;
  uint32_t before;

  lainvm_space_init(&space);
  lainvm_quota_init(&q, 0); /* 不限额：预扣一定成功，失败必须发生在分配那一步 */
  before = space.live_count;
  h = lainvm_space_alloc(&space, 1ull << 60, 16, 0, LAINVM_MEM_READ, 9, &q);
  if (!lainvm_space_handle_none(h)) {
    obs_facts("2^60 字节居然分配成功了 —— 这条打不到回滚路径");
    return 0;
  }
  if (q.used != 0 || q.charges != 1 || q.releases != 1) {
    obs_facts("分配失败留下扣账：used=%llu charges=%llu releases=%llu"
              "（期望 0 / 1 / 1）",
              (unsigned long long)q.used, (unsigned long long)q.charges,
              (unsigned long long)q.releases);
    return 0;
  }
  if (space.live_count != before) {
    obs_facts("失败的分配登记了区段：%u -> %u", (unsigned)before,
              (unsigned)space.live_count);
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 释放：归还额度、区段消失。 */
static int case_owned_free_returns_quota(void) {
  LainVmSpace space;
  LainVmQuota q;
  LainVmRegionHandle h;

  lainvm_space_init(&space);
  lainvm_quota_init(&q, 64);
  h = lainvm_space_alloc(&space, 64, 16, 64, LAINVM_MEM_READ, 9, &q);
  if (lainvm_space_handle_none(h)) {
    obs_facts("分配失败");
    return 0;
  }
  if (!lainvm_space_free(&space, h)) {
    obs_facts("释放失败");
    return 0;
  }
  if (q.used != 0 || q.releases != 1) {
    obs_facts("释放之后账目不对：used=%llu releases=%llu（期望 0 / 1）",
              (unsigned long long)q.used, (unsigned long long)q.releases);
    return 0;
  }
  if (lainvm_space_slot(&space, h) != NULL) {
    obs_facts("释放之后句柄还指着区段");
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 重复释放：第二次拒绝，表与账都不变。 */
static int case_owned_double_free_reject(void) {
  LainVmSpace space;
  LainVmQuota q;
  LainVmRegionHandle h;

  lainvm_space_init(&space);
  lainvm_quota_init(&q, 64);
  h = lainvm_space_alloc(&space, 64, 16, 64, LAINVM_MEM_READ, 9, &q);
  if (lainvm_space_handle_none(h) || !lainvm_space_free(&space, h)) {
    obs_facts("第一次分配/释放就失败了");
    return 0;
  }
  if (lainvm_space_free(&space, h)) {
    obs_facts("重复释放被接受了");
    return 0;
  }
  if (q.used != 0 || q.releases != 1) {
    obs_facts("重复释放改动了账目：used=%llu releases=%llu（期望 0 / 1）",
              (unsigned long long)q.used, (unsigned long long)q.releases);
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 跨空间句柄：在别的空间里释放必须拒绝，原区段还在、账不动。 */
static int case_owned_cross_space_reject(void) {
  LainVmSpace space;
  LainVmSpace other;
  LainVmQuota q;
  LainVmRegionHandle h;

  lainvm_space_init(&space);
  lainvm_space_init(&other);
  lainvm_quota_init(&q, 64);
  h = lainvm_space_alloc(&space, 64, 16, 64, LAINVM_MEM_READ, 9, &q);
  if (lainvm_space_handle_none(h)) {
    obs_facts("分配失败");
    return 0;
  }
  if (lainvm_space_free(&other, h)) {
    obs_facts("跨空间释放被接受了");
    return 0;
  }
  if (!lainvm_space_slot(&space, h)) {
    obs_facts("跨空间的失败调用动到了原区段");
    return 0;
  }
  if (q.used != 64) {
    obs_facts("跨空间的失败调用动了账：used=%llu（期望 64）",
              (unsigned long long)q.used);
    return 0;
  }
  lainvm_space_free(&space, h);
  obs_value(1);
  return 0;
}

/* 还有活借用时不许释放：区域、借用计数、账目都不动。 */
static int case_owned_free_while_borrowed_reject(void) {
  LainVmSpace space;
  LainVmQuota q;
  LainVmRegionHandle h;
  const LainVmRegion *r;

  lainvm_space_init(&space);
  lainvm_quota_init(&q, 64);
  h = lainvm_space_alloc(&space, 64, 16, 64, LAINVM_MEM_READ, 9, &q);
  if (lainvm_space_handle_none(h) || !lainvm_space_borrow(&space, h)) {
    obs_facts("分配或借用失败");
    return 0;
  }
  if (lainvm_space_free(&space, h)) {
    obs_facts("有活借用时释放被接受了");
    return 0;
  }
  r = lainvm_space_slot(&space, h);
  if (!r || r->borrow_count != 1) {
    obs_facts("被拒之后借用计数变了：%u（期望 1）",
              r ? (unsigned)r->borrow_count : 999u);
    return 0;
  }
  if (q.used != 64) {
    obs_facts("被拒之后账目变了：used=%llu（期望 64）",
              (unsigned long long)q.used);
    return 0;
  }
  if (!lainvm_space_end_borrow(&space, h) || !lainvm_space_free(&space, h)) {
    obs_facts("结束借用之后仍然释放不了");
    return 0;
  }
  if (q.used != 0) {
    obs_facts("释放之后账目没归零：used=%llu", (unsigned long long)q.used);
    return 0;
  }
  obs_value(1);
  return 0;
}

/* --- external mapping：只授权与撤销，不 free、不重复扣账 -------------------- */

/* 借入的存储按登记时的窗口可读；没给 WRITE 就写不了。 */
static int case_external_map_access(void) {
  LainVmSpace space;
  uint8_t buf[64];
  LainVmRegionHandle h;

  memset(buf, 42, sizeof(buf));
  lainvm_space_init(&space);
  h = lainvm_space_map_external(&space, (uintptr_t)buf, 64, 64,
                                LAINVM_MEM_READ, 9);
  if (lainvm_space_handle_none(h)) {
    obs_facts("external 登记失败");
    return 0;
  }
  if (!lainvm_space_check(&space, (uintptr_t)buf, 64, LAINVM_MEM_READ)) {
    obs_facts("借入区间内的读被拒了");
    return 0;
  }
  if (lainvm_space_check(&space, (uintptr_t)buf, 1, LAINVM_MEM_WRITE)) {
    obs_facts("只给了 READ 却能写");
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 撤销映射之后立刻不能访问（但字节还在）。 */
static int case_external_unmap_rejects_access(void) {
  LainVmSpace space;
  uint8_t buf[64];
  LainVmRegionHandle h;

  memset(buf, 42, sizeof(buf));
  lainvm_space_init(&space);
  h = lainvm_space_map_external(&space, (uintptr_t)buf, 64, 64,
                                LAINVM_MEM_READ, 9);
  if (lainvm_space_handle_none(h) || !lainvm_space_unmap_external(&space, h)) {
    obs_facts("登记或撤销失败");
    return 0;
  }
  if (lainvm_space_check(&space, (uintptr_t)buf, 1, LAINVM_MEM_READ)) {
    obs_facts("撤销之后还能访问");
    return 0;
  }
  if (buf[0] != 42) {
    obs_facts("撤销之后字节被改动了：%u", (unsigned)buf[0]);
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 撤销映射**不释放**底层存储：字节原样，仍归调用方所有（由它自己 free）。 */
static int case_external_unmap_does_not_free_backing(void) {
  LainVmSpace space;
  uint8_t *buf;
  LainVmRegionHandle h;
  uint32_t i;
  int same = 1;

  buf = new_buffer(64);
  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  memset(buf, 0x5A, 64);
  lainvm_space_init(&space);
  h = lainvm_space_map_external(&space, (uintptr_t)buf, 64, 64,
                                LAINVM_MEM_READ, 9);
  if (lainvm_space_handle_none(h) || !lainvm_space_unmap_external(&space, h)) {
    free(buf);
    obs_facts("登记或撤销失败");
    return 0;
  }
  for (i = 0; i < 64; i++) {
    if (buf[i] != 0x5A) same = 0;
  }
  if (!same) {
    obs_facts("撤销映射改动了借入的字节（那是调用方的存储）");
    free(buf);
    return 0;
  }
  free(buf); /* 释放归借出方：VSpace 不管 */
  obs_value(1);
  return 0;
}

/* 借入的存储**不扣账**（它不归 VSpace 申请），登记与撤销都不动 quota。 */
static int case_external_mapping_does_not_double_charge(void) {
  LainVmSpace space;
  LainVmQuota q;
  uint8_t buf[64];
  LainVmRegionHandle h;
  const LainVmRegion *r;

  memset(buf, 7, sizeof(buf));
  lainvm_space_init(&space);
  lainvm_quota_init(&q, 64);
  h = lainvm_space_map_external(&space, (uintptr_t)buf, 64, 64,
                                LAINVM_MEM_READ, 9);
  if (lainvm_space_handle_none(h)) {
    obs_facts("external 登记失败");
    return 0;
  }
  r = lainvm_space_slot(&space, h);
  if (!r || r->charged != 0 || r->quota != NULL ||
      r->backing_kind != LAINVM_BACKING_EXTERNAL) {
    obs_facts("借入区段带了账目：charged=%llu kind=%u",
              r ? (unsigned long long)r->charged : 0,
              r ? (unsigned)r->backing_kind : 999u);
    return 0;
  }
  if (q.used != 0) {
    obs_facts("借入登记扣了账：used=%llu（期望 0）",
              (unsigned long long)q.used);
    return 0;
  }
  if (!lainvm_space_unmap_external(&space, h) || q.used != 0) {
    obs_facts("撤销借入改动了账目：used=%llu",
              (unsigned long long)q.used);
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 跨空间：借入映射也不能在别的空间里撤销。 */
static int case_external_cross_space_reject(void) {
  LainVmSpace space;
  LainVmSpace other;
  uint8_t buf[64];
  LainVmRegionHandle h;

  memset(buf, 7, sizeof(buf));
  lainvm_space_init(&space);
  lainvm_space_init(&other);
  h = lainvm_space_map_external(&space, (uintptr_t)buf, 64, 64,
                                LAINVM_MEM_READ, 9);
  if (lainvm_space_handle_none(h)) {
    obs_facts("external 登记失败");
    return 0;
  }
  if (lainvm_space_unmap_external(&other, h)) {
    obs_facts("跨空间撤销映射被接受了");
    return 0;
  }
  if (!lainvm_space_slot(&space, h)) {
    obs_facts("跨空间的失败调用动到了原映射");
    return 0;
  }
  if (!lainvm_space_check(&space, (uintptr_t)buf, 1, LAINVM_MEM_READ)) {
    obs_facts("失败调用之后原映射不能访问了");
    return 0;
  }
  obs_value(1);
  return 0;
}

/* --- region 组 -------------------------------------------------------------- */

/* 合法范围里的读：返回那个字节（42）。 */
static int case_region_valid_read(void) {
  LainVmSpace space;
  uint8_t buf[16];

  memset(buf, 0, sizeof(buf));
  buf[0] = 42;
  lainvm_space_init(&space);
  if (add(&space, (uintptr_t)buf, sizeof(buf), LAINVM_MEM_READ, 7) < 0) {
    obs_facts("登记 16 字节合法区段失败");
    return 0;
  }
  if (!lainvm_space_check(&space, (uintptr_t)buf, 1, LAINVM_MEM_READ)) {
    obs_facts("check 拒绝了区段内的合法读");
    return 0;
  }
  obs_value(buf[0]);
  return 0;
}

/* 从未登记过的地址读：期望拒绝（1004，走的是裸地址 + VSpace 那一步）。 */
static int case_load_outside_region(void) {
  Rig rig;
  int rc;

  if (rig_load(&rig, k_prog_outside_region, 1024) != 0) return 0;
  rc = rig_run(&rig, "outside");
  rig_free(&rig);
  (void)rc;
  return 0;
}

/* 同一帧里的裸地址通路（正例）：alloca → store → load 必须真的读到写进去的值。 */
static int case_alloca_rw_works(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_alloca, 4096) != 0) return 0;
  (void)rig_run(&rig, "use_alloca");
  rig_free(&rig);
  return 0;
}

/* `#addr` 槽是单字裸地址：往返之后数值不变（没有 tag 字在中间）。 */
static int case_addr_slot_roundtrip(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_addr_slot_roundtrip, 4096) != 0) return 0;
  (void)rig_run(&rig, "two_flavors");
  rig_free(&rig);
  return 0;
}

/* `#int2ptr` 不授予访问权：整数变的裸地址照样要过当前 VSpace（拒 1004）。 */
static int case_int2ptr_no_grant(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_int2ptr_bare, 1024) != 0) return 0;
  (void)rig_run(&rig, "from_int");
  rig_free(&rig);
  return 0;
}

/* R01：先登记高地址，再登记低地址（内部索引会把它排到前面）——
 * 旧引用还指得到原对象吗？句柄化之后必须**指得到**：重排只发生在内部索引上。 */
static int case_region_insert_identity(void) {
  LainVmSpace space;
  uint8_t *buf = new_buffer(4096);
  LainVmRegionHandle ref;
  uintptr_t low, high, high_base, seen;

  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  low = (uintptr_t)buf;
  high = (uintptr_t)buf + 2048;
  lainvm_space_init(&space);
  ref = add_handle(&space, high, 1024, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 21);
  if (lainvm_space_handle_none(ref)) {
    free(buf);
    obs_facts("登记高地址段失败");
    return 0;
  }
  high_base = base_of(&space, ref);
  if (add(&space, low, 1024, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 22) < 0) {
    free(buf);
    obs_facts("登记低地址段失败");
    return 0;
  }
  seen = base_of(&space, ref);
  free(buf);
  if (seen != high_base) {
    obs_facts("R01 复现：插入低地址后，槽 %u 从 base=%llu 变成 base=%llu",
              (unsigned)ref.slot, (unsigned long long)high_base,
              (unsigned long long)seen);
    return 0;
  }
  obs_value(1);
  return 0;
}

/* R02：三段，按**句柄**撤销第一段——后两段的句柄还指得到原对象吗？ */
static int case_region_remove_identity(void) {
  LainVmSpace space;
  uint8_t *buf = new_buffer(4096);
  LainVmRegionHandle ref_a, ref_b, ref_c;
  uintptr_t base_b, base_c, seen_b, seen_c;

  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  lainvm_space_init(&space);
  ref_a = add_handle(&space, (uintptr_t)buf, 1024, LAINVM_MEM_READ, 11);
  ref_b = add_handle(&space, (uintptr_t)buf + 1024, 1024, LAINVM_MEM_READ, 12);
  ref_c = add_handle(&space, (uintptr_t)buf + 2048, 1024, LAINVM_MEM_READ, 13);
  if (lainvm_space_handle_none(ref_a) || lainvm_space_handle_none(ref_b) ||
      lainvm_space_handle_none(ref_c)) {
    free(buf);
    obs_facts("登记三段失败");
    return 0;
  }
  base_b = base_of(&space, ref_b);
  base_c = base_of(&space, ref_c);
  if (!lainvm_space_unmap_external(&space, ref_a)) {
    free(buf);
    obs_facts("按句柄撤销第一段失败");
    return 0;
  }
  seen_b = base_of(&space, ref_b);
  seen_c = base_of(&space, ref_c);
  free(buf);
  if (seen_b != base_b || seen_c != base_c) {
    obs_facts("R02 复现：撤销第一段后 B 从 %llu 变 %llu、C 从 %llu 变 %llu",
              (unsigned long long)base_b, (unsigned long long)seen_b,
              (unsigned long long)base_c, (unsigned long long)seen_c);
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 撤销只认句柄：撤销一段不能影响别的段 —— **owner 相同的也不行**。
 * （"按 owner 扫表删"这条路已经删掉了：它会一次清掉 owner=0 的装载器段。） */
static int case_region_remove_precise(void) {
  LainVmSpace space;
  uint8_t *buf = new_buffer(4096);
  LainVmRegionHandle loader_a, loader_b, tcb_seg;
  uint32_t before, after;

  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  lainvm_space_init(&space);
  loader_a = add_handle(&space, (uintptr_t)buf, 1024, LAINVM_MEM_READ, 0);
  loader_b = add_handle(&space, (uintptr_t)buf + 2048, 1024, LAINVM_MEM_READ, 0);
  tcb_seg = add_handle(&space, (uintptr_t)buf + 1024, 512, LAINVM_MEM_READ, 7);
  if (lainvm_space_handle_none(loader_a) ||
      lainvm_space_handle_none(loader_b) ||
      lainvm_space_handle_none(tcb_seg)) {
    free(buf);
    obs_facts("登记失败");
    return 0;
  }
  before = space.live_count;
  (void)lainvm_space_unmap_external(&space, tcb_seg);
  after = space.live_count;
  if (after != before - 1 || lainvm_space_slot(&space, loader_a) == NULL ||
      lainvm_space_slot(&space, loader_b) == NULL ||
      lainvm_space_slot(&space, tcb_seg) != NULL) {
    free(buf);
    obs_facts("精确撤销不对：活段 %u -> %u，装载器段还在=%d/%d，被撤销那段还在=%d",
              (unsigned)before, (unsigned)after,
              lainvm_space_slot(&space, loader_a) != NULL,
              lainvm_space_slot(&space, loader_b) != NULL,
              lainvm_space_slot(&space, tcb_seg) != NULL);
    return 0;
  }
  free(buf);
  obs_value(1);
  return 0;
}

/* 表满：64 段能登记，第 65 段必须被拒且前 64 段不变。 */
static int case_region_full_64(void) {
  LainVmSpace space;
  uint8_t *buf = new_buffer(64 * 64 + 64);
  LainVmRegionHandle first = lainvm_space_no_handle();
  LainVmRegionHandle last = lainvm_space_no_handle();
  LainVmRegionHandle extra;
  uint32_t i;
  uintptr_t first_base, last_base;

  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  lainvm_space_init(&space);
  for (i = 0; i < 64; i++) {
    LainVmRegionHandle handle =
        add_handle(&space, (uintptr_t)buf + i * 64, 64, LAINVM_MEM_READ, 1);
    if (lainvm_space_handle_none(handle)) {
      obs_facts("第 %u 段就登记失败了（应当能到 64）", (unsigned)(i + 1));
      free(buf);
      return 0;
    }
    if (i == 0) first = handle;
    if (i == 63) last = handle;
  }
  first_base = base_of(&space, first);
  last_base = base_of(&space, last);
  extra = add_handle(&space, (uintptr_t)buf + 64 * 64, 64, LAINVM_MEM_READ, 1);
  if (!lainvm_space_handle_none(extra)) {
    obs_facts("第 65 段被接受了（活段 %u）", (unsigned)space.live_count);
    free(buf);
    return 0;
  }
  if (space.live_count != 64 || base_of(&space, first) != first_base ||
      base_of(&space, last) != last_base) {
    obs_facts("第 65 段被拒，但前 64 段被改动了（活段 %u）",
              (unsigned)space.live_count);
    free(buf);
    return 0;
  }
  free(buf);
  obs_value(1);
  return 0;
}

/* 重叠拒绝。 */
static int case_region_overlap_reject(void) {
  LainVmSpace space;
  uint8_t *buf = new_buffer(4096);
  int ok;

  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  lainvm_space_init(&space);
  (void)add(&space, (uintptr_t)buf, 1024, LAINVM_MEM_READ, 1);
  ok = add(&space, (uintptr_t)buf + 512, 1024, LAINVM_MEM_READ, 2) < 0;
  free(buf);
  if (!ok) {
    obs_facts("重叠区段被接受了");
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 相邻允许（半开区间 [base, base+size)）。 */
static int case_region_adjacent_allow(void) {
  LainVmSpace space;
  uint8_t *buf = new_buffer(4096);
  int ok;

  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  lainvm_space_init(&space);
  (void)add(&space, (uintptr_t)buf, 1024, LAINVM_MEM_READ, 1);
  ok = add(&space, (uintptr_t)buf + 1024, 1024, LAINVM_MEM_READ, 2) >= 0;
  free(buf);
  if (!ok) {
    obs_facts("首尾相邻的两段被判成重叠");
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 零大小与不可表示上界必须拒。 */
static int case_region_bad_size_reject(void) {
  LainVmSpace space;
  uint8_t *buf = new_buffer(64);
  int zero_rejected, wrap_rejected;

  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  lainvm_space_init(&space);
  zero_rejected = add(&space, (uintptr_t)buf, 0, LAINVM_MEM_READ, 1) < 0;
  wrap_rejected =
      add(&space, (uintptr_t)buf, (uint64_t)-1, LAINVM_MEM_READ, 1) < 0;
  free(buf);
  if (!zero_rejected || !wrap_rejected) {
    obs_facts("零大小被接受=%d，上界回绕被接受=%d", zero_rejected ? 0 : 1,
              wrap_rejected ? 0 : 1);
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 范围判定：整段可读、越界一字节不可读、尾后起点不可读。 */
static int case_region_range_tail(void) {
  LainVmSpace space;
  uint8_t *buf = new_buffer(1024);
  int whole, past_one, at_end;

  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  lainvm_space_init(&space);
  (void)add(&space, (uintptr_t)buf, 1024, LAINVM_MEM_READ, 1);
  whole = lainvm_space_check(&space, (uintptr_t)buf, 1024, LAINVM_MEM_READ);
  past_one = lainvm_space_check(&space, (uintptr_t)buf + 1023, 2, LAINVM_MEM_READ);
  at_end = lainvm_space_check(&space, (uintptr_t)buf + 1024, 1, LAINVM_MEM_READ);
  free(buf);
  if (!whole || past_one || at_end) {
    obs_facts("整段可读=%d，越界一字节可读=%d，尾后起点可读=%d", whole ? 1 : 0,
              past_one ? 1 : 0, at_end ? 1 : 0);
    return 0;
  }
  obs_value(1);
  return 0;
}

/* --- region：句柄契约本身 ---------------------------------------------------- */

/* 槽被重用之后，旧句柄必须失效（不能"活过来"指到新对象）。 */
static int case_region_handle_stale(void) {
  LainVmSpace space;
  uint8_t *buf = new_buffer(4096);
  LainVmRegionHandle old_handle, new_handle;

  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  lainvm_space_init(&space);
  old_handle = add_handle(&space, (uintptr_t)buf, 1024, LAINVM_MEM_READ, 1);
  if (lainvm_space_handle_none(old_handle) ||
      !lainvm_space_unmap_external(&space, old_handle)) {
    free(buf);
    obs_facts("登记/撤销第一段失败");
    return 0;
  }
  new_handle =
      add_handle(&space, (uintptr_t)buf + 2048, 1024, LAINVM_MEM_READ, 2);
  free(buf);
  if (lainvm_space_handle_none(new_handle)) {
    obs_facts("撤销之后新的登记失败（槽没有被回收）");
    return 0;
  }
  if (lainvm_space_slot(&space, old_handle) != NULL) {
    obs_facts("旧句柄在槽被重用之后仍然有效（slot=%u gen=%u -> gen=%u）",
              (unsigned)old_handle.slot, (unsigned)old_handle.generation,
              (unsigned)space.slots[old_handle.slot].generation);
    return 0;
  }
  if (lainvm_space_slot(&space, new_handle) == NULL) {
    obs_facts("新句柄无效");
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 跨空间使用句柄必须失败，且对方空间不变。 */
static int case_region_handle_cross_space(void) {
  LainVmSpace a, b;
  uint8_t *buf = new_buffer(4096);
  LainVmRegionHandle in_a;
  uint32_t live_b;

  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  lainvm_space_init(&a);
  lainvm_space_init(&b);
  in_a = add_handle(&a, (uintptr_t)buf, 1024, LAINVM_MEM_READ, 1);
  if (lainvm_space_handle_none(in_a)) {
    free(buf);
    obs_facts("登记失败");
    return 0;
  }
  live_b = b.live_count;
  if (lainvm_space_slot(&b, in_a) != NULL || lainvm_space_unmap_external(&b, in_a) ||
      b.live_count != live_b) {
    free(buf);
    obs_facts("另一个空间接受了别人的句柄（活段 %u -> %u）", (unsigned)live_b,
              (unsigned)b.live_count);
    return 0;
  }
  free(buf);
  obs_value(1);
  return 0;
}

/* 重复撤销：第二次必须失败，且表不变。 */
static int case_region_handle_double_remove(void) {
  LainVmSpace space;
  uint8_t *buf = new_buffer(4096);
  LainVmRegionHandle handle;
  uint32_t live;

  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  lainvm_space_init(&space);
  handle = add_handle(&space, (uintptr_t)buf, 1024, LAINVM_MEM_READ, 1);
  if (lainvm_space_handle_none(handle) || !lainvm_space_unmap_external(&space, handle)) {
    free(buf);
    obs_facts("登记 / 第一次撤销失败");
    return 0;
  }
  live = space.live_count;
  if (lainvm_space_unmap_external(&space, handle)) {
    free(buf);
    obs_facts("第二次撤销被当成成功");
    return 0;
  }
  if (space.live_count != live) {
    free(buf);
    obs_facts("第二次撤销改动了表（活段 %u -> %u）", (unsigned)live,
              (unsigned)space.live_count);
    return 0;
  }
  free(buf);
  obs_value(1);
  return 0;
}

/* --- stack 组 --------------------------------------------------------------- */

/* R03：TCB 的栈**租约**记的是自己的 base/size，不是区段表里的位置——在它下面插入
 * 别的区段之后，租约还指得到自己那块吗？销毁时 free 的还是不是自己那块？
 * （这里曾经必须"先撤销这次插入再销毁"：那时销毁按缓存的表下标取地址去 free，
 *   下标一挪就 free 到别人的地址，直接把进程打成堆损坏 0xC0000374。） */
static int case_stack_lease_identity(void) {
  Rig rig;
  uintptr_t stack_base;
  int rc;

  if (rig_load(&rig, k_prog_alloca, 4096) != 0) return 0;
  stack_base = rig.tcb->stack_base;
  if (stack_base == 0 || rig.tcb->stack_size != 4096) {
    rig_free(&rig);
    obs_facts("TCB 没有栈租约（stack_bytes=4096 却拿到 base=%llu size=%llu）",
              (unsigned long long)stack_base,
              (unsigned long long)rig.tcb->stack_size);
    return 0;
  }
  /* 在这块栈的**正下方**登记一段合成区段：base 更低 → 内部索引会把它排到前面。
     只登记、不解引用，所以不碰任何未映射内存。 */
  rc = add(&rig.space, stack_base - 4096, 4096, LAINVM_MEM_READ, 99);
  if (rc < 0) {
    rig_free(&rig);
    obs_facts("在栈下方登记合成区段失败（base=%llu）",
              (unsigned long long)stack_base);
    return 0;
  }
  if (rig.tcb->stack_base != stack_base || rig.tcb->stack_size != 4096) {
    rig_free(&rig);
    obs_facts("R03 复现：插入别人之后栈租约指到 base=%llu（自己是 %llu）",
              (unsigned long long)rig.tcb->stack_base,
              (unsigned long long)stack_base);
    return 0;
  }
  rig_free(&rig); /* 撤销精确：free 的是自己那块，不在区段表里找位置 */
  obs_value(1);
  return 0;
}

/* 没有栈却用 #alloca：期望拒绝（1006）。 */
static int case_stack_absent_trap(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_alloca, 0) != 0) return 0;
  (void)rig_run(&rig, "use_alloca");
  rig_free(&rig);
  return 0;
}

/* 栈用完：第二次 alloca 必须拒（1007），且水位不能动。
 * 期望水位 8 是**算出来**的：第一次 `#alloca[#bits<64>](1)` 要 1 个 8 字节元素，
 * 起点对齐后仍是 0，于是水位 = 0 + 8 = 8。失败那一次要是也动了水位，读数会是 48。
 * （不能靠"先跑一个成功的再看水位"来标定：过程正常返回时水位会被 frame 的
 * stack_mark 回退成 0，那是**跑完**的状态，不是失败前那一刻的。） */
static int case_stack_exhaust_watermark(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_alloca_exhaust, 32) != 0) return 0;
  (void)rig_run(&rig, "two_alloca");
  if (g_obs.kind == 1 && g_obs.trap_code == 1007 && rig.tcb->stack_used != 8) {
    obs_facts("拒绝了，但水位被移动成 %llu（分配失败时必须保持 8）",
              (unsigned long long)rig.tcb->stack_used);
  }
  rig_free(&rig);
  return 0;
}

/* R08：count * 元素大小 溢出。期望拒绝。 */
static int case_stack_count_overflow(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_alloca_overflow, 4096) != 0) return 0;
  (void)rig_run(&rig, "alloca_overflow");
  rig_free(&rig);
  return 0;
}

/* 零 count：**验证器直接拒**（2024 #alloca count must be positive）。
 * 也就是说这条语义其实已经定了；引擎里那句 `count ? count : 1`（engine.c:484）
 * 是够不着的死分支。按 2024 钉住。 */
static int case_stack_zero_count(void) {
  Rig rig;
  int rc;

  rc = rig_load(&rig, k_prog_alloca_zero, 64);
  if (rc == -2) {
    obs_trap(0, rig.fail_code);
    rig_free(&rig);
    return 0;
  }
  if (rc != 0) return 0;
  (void)rig_run(&rig, "alloca_zero");
  rig_free(&rig);
  return 0;
}

/* 两个 TCB 共用一个 VSpace（= 同一地址空间里的两条执行流 / 两个线程）：
 * 各自的栈租约必须互不影响，先销毁哪一个都不能碰到另一个。
 * 这是"栈跟着 TCB 的身份走、不跟着空间里的位置走"的最小可测情形——
 * 也是迁徙线程那条性质（栈属于线程，不属于它此刻跑在哪里）在 VSpace 上的投影。 */
static int case_stack_two_tcbs_one_space(void) {
  Rig rig;
  LainVmTcb *t1, *t2;
  uintptr_t base1, base2;
  int ok = 1;

  if (rig_load(&rig, k_prog_alloca, 4096) != 0) return 0;
  t1 = rig.tcb; /* 台架已经建了一个 */
  base1 = t1->stack_base;
  t2 = lainvm_tcb_new(rig.image, &rig.space, 2, 2, 64, 4096, NULL);
  if (!t2) {
    rig_free(&rig);
    obs_facts("同一个地址空间里建第二个 TCB 失败");
    return 0;
  }
  base2 = t2->stack_base;
  if (base1 == 0 || base2 == 0 || base1 == base2) {
    ok = 0;
    obs_facts("两个 TCB 的栈不独立：base1=%llu base2=%llu",
              (unsigned long long)base1, (unsigned long long)base2);
  }
  if (ok) {
    /* 先销毁第二个：第一个的租约必须纹丝不动，而且它接着还能正常 alloca。 */
    lainvm_tcb_free(t2);
    if (t1->stack_base != base1 || t1->stack_size != 4096) {
      ok = 0;
      obs_facts("销毁 TCB2 之后 TCB1 的租约变成 base=%llu size=%llu",
                (unsigned long long)t1->stack_base,
                (unsigned long long)t1->stack_size);
    }
  }
  if (ok) {
    memset(&g_obs, 0, sizeof(g_obs));
    (void)rig_run(&rig, "use_alloca");
    if (g_obs.kind != 0 || g_obs.value != 7) {
      ok = 0;
      if (g_obs.kind == 1)
        obs_facts("销毁 TCB2 之后 TCB1 的 alloca 被拒（码 %d）", g_obs.trap_code);
      else
        obs_facts("销毁 TCB2 之后 TCB1 的结果/状态不对（kind=%d）", g_obs.kind);
    }
  }
  rig_free(&rig); /* 剩下的 TCB1 */
  if (!ok) return 0;
  obs_value(1);
  return 0;
}

/* seL4 的 TCB_SetSpace：换地址空间。栈句柄带着旧空间的身份，所以在新的空间里
 * 自动失效 —— `#alloca` 稳定拒 1006（不是悄悄用错内存）；换回来，租约还在。 */
static int case_space_switch(void) {
  Rig rig;
  LainVmSpace other;
  L1Diagnostic diag;
  uint64_t first, back;
  int32_t code_switch = 0;
  int kind_switch, kind_back;

  if (rig_load(&rig, k_prog_alloca, 4096) != 0) return 0;
  /* 1) 原来的空间里跑一次：应当成功 */
  (void)rig_run(&rig, "use_alloca");
  if (g_obs.kind != 0) {
    rig_free(&rig);
    obs_facts("原空间里就没跑通（kind=%d）", g_obs.kind);
    return 0;
  }
  first = g_obs.value;
  /* 2) 换到一个空的空间：栈租约失效，#alloca 必须稳定拒 */
  lainvm_space_init(&other);
  diag.code = 0;
  if (lainvm_tcb_set_space(rig.tcb, &other, &diag) != 0) {
    rig_free(&rig);
    obs_facts("set_space 失败：code=%d %s", diag.code, diag.message);
    return 0;
  }
  (void)rig_run(&rig, "use_alloca");
  kind_switch = g_obs.kind;
  if (kind_switch == 1) code_switch = g_obs.trap_code;
  /* 3) 换回原空间：那份租约仍然有效，应当又跑通 */
  diag.code = 0;
  if (lainvm_tcb_set_space(rig.tcb, &rig.space, &diag) != 0) {
    rig_free(&rig);
    obs_facts("换回原空间失败：code=%d %s", diag.code, diag.message);
    return 0;
  }
  (void)rig_run(&rig, "use_alloca");
  kind_back = g_obs.kind;
  back = (kind_back == 0) ? g_obs.value : 0;
  rig_free(&rig);
  if (kind_switch != 1 || code_switch != 1006) {
    obs_facts("换到空空间后 #alloca 没被拒：kind=%d code=%d（期望 trap 1006）",
              kind_switch, (int)code_switch);
    return 0;
  }
  if (kind_back != 0 || back != first) {
    obs_facts("换回原空间后没恢复：kind=%d 值=%llu（期望 %llu）", kind_back,
              (unsigned long long)back, (unsigned long long)first);
    return 0;
  }
  obs_value(1);
  return 0;
}

/* --- host 组 ---------------------------------------------------------------- */

/* R04：从合法区段的末尾读，长度超出一字节 —— 能力该不该拦？
 *
 * 交付 B 之后这里必须**先授权再测**：宿主服务要先 attach 到地址空间、目标区段
 * 要登记进去，否则"拒"的理由只是没授权，测不到范围检查。所以：
 *   正对照 区内 4 字节 → 必须写进输出（否则下面那条拒可能只是全都在拒）；
 *   负对照 末尾起 2 字节 → 必须拒，而且**输出一个字节都不能动**（先检后写）。 */
static int case_host_past_region(void) {
  LainMetaHost *host = NULL;
  LainVmCaps *caps = NULL;
  const LainVmCapEntry *entry;
  LainVmHostFn fn;
  LainVmSpace space;
  uint8_t *buf;
  uint64_t args[3];
  uint64_t result = 0;
  uint32_t status;
  uint32_t out_len;
  uint32_t ok_status;
  uint32_t ok_len;

  buf = new_buffer(4096);
  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  host = lainmeta_host_new();
  caps = lainvm_caps_new();
  if (!host || !caps || lainmeta_host_register(host, caps) != 0) {
    obs_facts("host / 能力表建立失败");
    goto done;
  }
  entry = lainvm_caps_find(caps, "lain_meta_emit_write");
  if (!entry) {
    obs_facts("能力表里没有 lain_meta_emit_write");
    goto done;
  }
  fn = lainvm_cap_fn(entry);
  lainvm_space_init(&space);
  /* 只把前 16 字节登记成区段；底下那块 4096 字节是真内存，所以越界读不会崩。 */
  if (add(&space, (uintptr_t)buf, 16, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 7) < 0) {
    obs_facts("登记 16 字节区段失败");
    goto done;
  }
  lainmeta_host_attach_space(host, &space);
  args[0] = (uint64_t)(uintptr_t)host;
  args[1] = (uint64_t)(uintptr_t)buf; /* 区内 4 字节：正对照 */
  args[2] = 4;
  ok_status = fn(args, 3, &result);
  ok_len = lainmeta_host_output_length(host);
  args[1] = (uint64_t)(uintptr_t)(buf + 15); /* 区段末尾的最后一个字节 */
  args[2] = 2;                               /* 读 2 字节：超出一字节 */
  status = fn(args, 3, &result);
  out_len = lainmeta_host_output_length(host);
  if (ok_status != 0 || ok_len != 4) {
    obs_facts("正对照失败：授权区内的 4 字节被拒（status=%u，输出长度=%u）",
              (unsigned)ok_status, (unsigned)ok_len);
  } else if (status != 0) {
    if (out_len != 4) {
      obs_facts("拒是拒了，但输出被动过：长度=%u（期望仍是 4）", (unsigned)out_len);
    } else {
      obs_trap(0, (int32_t)status);
    }
  } else {
    obs_facts("R04 复现：宿主按裸地址读了区段外的字节（status=0，输出长度=%u）",
              (unsigned)out_len);
  }

done:
  if (caps) lainvm_caps_free(caps);
  if (host) lainmeta_host_free(host);
  free(buf);
  return 0;
}

/* R05：把 host 参数换成另一个同样可读写的 host 对象 —— 该不该拒？
 *
 * 身份 = **这份宿主服务被授权到哪个地址空间**（驱动 attach）。所以这一条里
 * 只给真 host 授权、诱饵不给：
 *   正对照 真 host + 区内地址 → 必须过；
 *   负对照 诱饵 + 同一个地址   → 必须拒，且诱饵的输出一个字节都不能动。 */
static int case_host_wrong_identity(void) {
  LainMetaHost *real = NULL;
  LainMetaHost *decoy = NULL;
  LainVmCaps *caps = NULL;
  const LainVmCapEntry *entry;
  LainVmHostFn fn;
  LainVmSpace space;
  uint8_t *buf;
  uint64_t args[3];
  uint64_t result = 0;
  uint32_t status;
  uint32_t decoy_out;
  uint32_t real_out;

  buf = new_buffer(256);
  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  real = lainmeta_host_new();
  decoy = lainmeta_host_new();
  caps = lainvm_caps_new();
  if (!real || !decoy || !caps || lainmeta_host_register(real, caps) != 0) {
    obs_facts("host / 能力表建立失败");
    goto done;
  }
  entry = lainvm_caps_find(caps, "lain_meta_emit_write");
  if (!entry) {
    obs_facts("能力表里没有 lain_meta_emit_write");
    goto done;
  }
  fn = lainvm_cap_fn(entry);
  lainvm_space_init(&space);
  if (add(&space, (uintptr_t)buf, 256, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 7) < 0) {
    obs_facts("登记 256 字节区段失败");
    goto done;
  }
  lainmeta_host_attach_space(real, &space); /* 只给真 host 授权 */
  args[0] = (uint64_t)(uintptr_t)real;      /* 正对照：真 host 应当能写 */
  args[1] = (uint64_t)(uintptr_t)buf;
  args[2] = 4;
  status = fn(args, 3, &result);
  real_out = lainmeta_host_output_length(real);
  args[0] = (uint64_t)(uintptr_t)decoy; /* ← 没被授权的那个 host 对象 */
  status = fn(args, 3, &result);
  decoy_out = lainmeta_host_output_length(decoy);
  if (real_out != 4) {
    obs_facts("正对照失败：真 host 在区内写 4 字节却得到输出长度=%u",
              (unsigned)real_out);
  } else if (status != 0) {
    if (decoy_out != 0) {
      obs_facts("拒是拒了，但诱饵的输出被动过：长度=%u（期望 0）",
                (unsigned)decoy_out);
    } else {
      obs_trap(0, (int32_t)status);
    }
  } else {
    obs_facts("R05 复现：换了个没授权的 host 对象照样被接受（输出长度=%u）",
              (unsigned)decoy_out);
  }

done:
  if (caps) lainvm_caps_free(caps);
  if (real) lainmeta_host_free(real);
  if (decoy) lainmeta_host_free(decoy);
  free(buf);
  return 0;
}

/* 宿主边界的**正例**：授权区内读一段，必须成功、字节数对得上、内容对得上。 */
static int case_host_in_region_ok(void) {
  LainMetaHost *host = NULL;
  LainVmCaps *caps = NULL;
  const LainVmCapEntry *entry;
  LainVmHostFn fn;
  LainVmSpace space;
  uint8_t *buf = NULL;
  uint64_t args[3];
  uint64_t result = 0;
  uint32_t status;
  uint32_t out_len;

  buf = new_buffer(64);
  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  memcpy(buf, "lain", 4);
  host = lainmeta_host_new();
  caps = lainvm_caps_new();
  if (!host || !caps || lainmeta_host_register(host, caps) != 0) {
    obs_facts("host / 能力表建立失败");
    goto done;
  }
  entry = lainvm_caps_find(caps, "lain_meta_emit_write");
  if (!entry) {
    obs_facts("能力表里没有 lain_meta_emit_write");
    goto done;
  }
  fn = lainvm_cap_fn(entry);
  lainvm_space_init(&space);
  if (add(&space, (uintptr_t)buf, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 7) < 0) {
    obs_facts("登记 64 字节区段失败");
    goto done;
  }
  lainmeta_host_attach_space(host, &space);
  args[0] = (uint64_t)(uintptr_t)host;
  args[1] = (uint64_t)(uintptr_t)buf;
  args[2] = 4;
  status = fn(args, 3, &result);
  out_len = lainmeta_host_output_length(host);
  if (status != 0) {
    obs_facts("授权区内的读被拒了：status=%u", (unsigned)status);
  } else if (out_len != 4 || memcmp(lainmeta_host_output(host), "lain", 4) != 0) {
    obs_facts("读进来了但字节不对：长度=%u 内容=%.4s", (unsigned)out_len,
              lainmeta_host_output(host));
  } else {
    obs_value(1);
  }

done:
  if (caps) lainvm_caps_free(caps);
  if (host) lainmeta_host_free(host);
  free(buf);
  return 0;
}

/* §五：源码**索引越界**必须在解引用之前稳定拒绝，而且不产生任何副作用
 * （能力返回的格子一个字节都不能被写）。三个会返回源码地址/长度/路径的能力
 * 都要覆盖 —— 只修一个入口等于没修。索引越界不是"读到别人的内存"，
 * 而是"这份源码不存在"：拒绝码是 LAINMETA_ERR_NO_SOURCE。 */
static int case_host_source_index_bounds(void) {
  static const char *names[3] = {"lain_meta_source_data",
                                 "lain_meta_source_length",
                                 "lain_meta_source_path_data"};
  LainMetaHost *host = NULL;
  LainVmCaps *caps = NULL;
  const LainVmCapEntry *entry;
  LainVmHostFn fn;
  const char *path;
  const char *text;
  uint32_t len = 12345;
  uint64_t args[2];
  uint64_t result;
  uint64_t sentinel = 0xA5A5A5A5A5A5A5A5ull;
  uint32_t status;
  uint32_t i;
  int ok = 1;

  host = lainmeta_host_new();
  caps = lainvm_caps_new();
  if (!host || !caps || lainmeta_host_register(host, caps) != 0) {
    obs_facts("host / 能力表建立失败");
    goto done;
  }
  /* 一份源码：索引 0 合法，索引 1 越界。 */
  if (lainmeta_host_add_source(host, "std/prelude.lain", "let x = 1;\n", 11) !=
      0) {
    obs_facts("登记源码失败");
    goto done;
  }
  args[0] = (uint64_t)(uintptr_t)host;

  for (i = 0; i < 3 && ok; i++) {
    entry = lainvm_caps_find(caps, names[i]);
    if (!entry) {
      obs_facts("能力表里没有 %s", names[i]);
      ok = 0;
      break;
    }
    fn = lainvm_cap_fn(entry);
    /* 越界：非零状态，且**结果格子保持哨兵值**（没有被解引用、没有被写）。 */
    result = sentinel;
    args[1] = 1;
    status = fn(args, 2, &result);
    if (status == 0 || result != sentinel) {
      obs_facts("%s 对越界索引 index=1 没有稳定拒绝：status=%u result=%llu"
                "（期望非零状态且结果格子不变）",
                names[i], (unsigned)status, (unsigned long long)result);
      ok = 0;
      break;
    }
    /* 正对照：索引 0 必须成功 —— 否则上面的"拒绝"可能只是这个能力坏了。 */
    result = 0;
    args[1] = 0;
    status = fn(args, 2, &result);
    if (status != 0 || result == 0) {
      obs_facts("%s 对合法索引 index=0 也失败了：status=%u result=%llu",
                names[i], (unsigned)status, (unsigned long long)result);
      ok = 0;
      break;
    }
  }
  /* 内部 API 同样是入口：越界返回空串，长度写 0（不是留着调用方原来的值）。 */
  if (ok) {
    path = lainmeta_host_source_path(host, 1);
    text = lainmeta_host_source_text(host, 1, &len);
    if (path == NULL || path[0] != '\0' || text == NULL || text[0] != '\0' ||
        len != 0) {
      obs_facts("内部入口越界：path=\"%s\" text=\"%s\" len=%u（期望空串与 0）",
                path ? path : "(null)", text ? text : "(null)", (unsigned)len);
      ok = 0;
    }
  }
  if (ok) obs_value(1);

done:
  if (caps) lainvm_caps_free(caps);
  if (host) lainmeta_host_free(host);
  return 0;
}

/* --- lifetime 组 ------------------------------------------------------------ */

/* R06：alloca 地址经由模块级数据逃逸，调用方在返回后读它。期望拒绝。 */
static int case_lifetime_escape(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_escape, 4096) != 0) return 0;
  (void)rig_run(&rig, "use_after_return");
  rig_free(&rig);
  return 0;
}

/* R07：**裸地址不可区分**（作者 2026-09-21 校正）。
 *
 * 「同址复用后旧地址必须被拒」**不是** `#addr` 的契约：`#addr` 是无类型物理地址，
 * 没有对象身份与代数；那条要求只适用于高层 `ref(T)` 的物理表示（见 checked-ref 组）。
 * 所以要验证的是两件事：
 *   1. 第一次调用分配的那段地址，在返回后**不再被授权**（活窗口随水位收回）——
 *      这就是 R06 拒绝的原因；
 *   2. 第二次调用在同一数值地址上重新分配并授权之后，**旧的数值地址照样能访问到
 *      新分配写进去的值**：新旧裸地址不可区分。
 * 域外引用不靠 VM 里的身份挡，靠 Meta 的 `ref(T)` 生命周期规则挡。 */
static int case_lifetime_reuse(void) {
  Rig rig;
  int reused;

  /* 先确认这个台架里确实发生同址复用，否则"读到新值"可能只是巧合。 */
  if (rig_load(&rig, k_prog_reuse, 4096) != 0) return 0;
  (void)rig_run(&rig, "reuse");
  reused = (g_obs.kind == 0 && g_obs.value == 1);
  rig_free(&rig);

  if (!reused) {
    obs_facts("两次调用没有拿到同一地址，这一条打不到目标");
    return 0;
  }
  if (rig_load(&rig, k_prog_reuse_guard, 4096) != 0) return 0;
  (void)rig_run(&rig, "reuse_same_address");
  rig_free(&rig);
  if (g_obs.kind == 0 && g_obs.value == 9) {
    obs_value(9); /* 旧裸地址读到的正是**新分配**写的 9：不可区分 */
  } else if (g_obs.kind == 1) {
    obs_facts("同址重新授权后旧裸地址仍被拒（code=%d）—— 那是把身份塞进 `#addr`，"
              "不是裸地址的契约",
              (int)g_obs.trap_code);
  } else {
    obs_facts("同址重新授权后旧裸地址读到 %llu（期望 9 = 新分配写进去的值）",
              (unsigned long long)g_obs.value);
  }
  return 0;
}

/* --- lea 组 ---------------------------------------------------------------- */

/* D3：`#lea` **构造不查** —— 区段外偏移照样构造得出来，返回的偏移正好是 1000000。 */
static int case_lea_construct_only(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_lea_far, 1024) != 0) return 0;
  (void)rig_run(&rig, "lea_far");
  rig_free(&rig);
  return 0;
}

/* 配对：区段外地址**构造成功**（偏移 1000000），随后**访问稳定拒绝**（1004）。
 * 两半都要成立才算过 —— 只有"构造成功"或只有"访问被拒"都不够。 */
static int case_lea_construct_then_access(void) {
  Rig rig;
  uint64_t offset;

  if (rig_load(&rig, k_prog_lea_far, 1024) != 0) return 0;
  (void)rig_run(&rig, "lea_far");
  offset = g_obs.value;
  if (g_obs.kind != 0 || offset != 1000000) {
    rig_free(&rig);
    obs_facts("构造那一步没成功：kind=%d offset=%llu（期望 value:1000000）",
              g_obs.kind, (unsigned long long)offset);
    return 0;
  }
  rig_free(&rig);

  if (rig_load(&rig, k_prog_lea_far_access, 1024) != 0) return 0;
  (void)rig_run(&rig, "lea_far_access");
  rig_free(&rig);
  if (g_obs.kind == 1 && g_obs.trap_code == 1004) {
    obs_value(1000000); /* 两半都成立 */
  } else if (g_obs.kind == 0) {
    obs_facts("区段外地址上的 load 竟然读到了 %llu（期望拒 1004）",
              (unsigned long long)g_obs.value);
  } else {
    obs_facts("区段外地址上的 load 拒了，但码是 %d（期望 1004）",
              (int)g_obs.trap_code);
  }
  return 0;
}

/* 尾后地址：构造得出（偏移 1）。 */
static int case_lea_one_past_end(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_lea_one_past, 1024) != 0) return 0;
  (void)rig_run(&rig, "lea_one_past");
  rig_free(&rig);
  return 0;
}

/* 尾后地址上的访问：拒 1004（"允许构造"不等于"允许访问"）。 */
static int case_lea_one_past_access(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_lea_one_past_access, 1024) != 0) return 0;
  (void)rig_run(&rig, "lea_one_past_access");
  rig_free(&rig);
  return 0;
}

/* 回绕按地址宽度取模：idx=2^63、scale=2 → 结果正好回到 base，偏移 0。 */
static int case_lea_wraparound(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_lea_wrap, 1024) != 0) return 0;
  (void)rig_run(&rig, "lea_wrap");
  rig_free(&rig);
  return 0;
}

/* 回绕落到已授权地址上：访问正常通过并读到 42。 */
static int case_lea_wrap_lands_authorized(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_lea_wrap_read, 1024) != 0) return 0;
  (void)rig_run(&rig, "lea_wrap_read");
  rig_free(&rig);
  return 0;
}

/* --- quota 组（D5：分配配额，规范 §4/:105、§6/:162、§8.2/:212-214） ----------
 *
 * 单位是**字节**，记的是**实际承诺**的底层存储。扣费点是真正拿到存储的地方：
 * TCB 的栈租约（整块一次）与宿主暂存区/输出扩容。归还只发生在真的释放时。
 * 下面八条就是交接里点名要验的八件事。 */

/* 按指定 fuel 反复切片跑到结束（只为验"账目与切片无关"）。 */
static int rig_run_sliced(Rig *rig, const char *entry, uint64_t fuel) {
  L1Diagnostic diag;
  LainVmSliceResult slice;

  diag.code = 0;
  if (lainvm_tcb_start(rig->tcb, entry, NULL, 0, &diag) != 0) return -1;
  do {
    slice = lainvm_engine_run(rig->tcb, fuel);
  } while (slice == LAINVM_SLICE_RUNNABLE);
  return slice == LAINVM_SLICE_DONE ? 0 : 1;
}

/* 1) 恰好用完成功。 */
static int case_quota_exact_fit(void) {
  LainVmQuota q;

  lainvm_quota_init(&q, 4096);
  if (lainvm_quota_charge(&q, 4096) != 0) {
    obs_facts("恰好用完却没有成功");
    return 0;
  }
  if (q.used != 4096 || lainvm_quota_remaining(&q) != 0) {
    obs_facts("恰好用完：used=%llu remaining=%llu（期望 4096 / 0）",
              (unsigned long long)q.used,
              (unsigned long long)lainvm_quota_remaining(&q));
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 2) 多一字节失败，而且**账目不动**（预扣是原子的）。 */
static int case_quota_one_byte_over(void) {
  LainVmQuota q;
  int code;

  lainvm_quota_init(&q, 4096);
  if (lainvm_quota_charge(&q, 4096) != 0) {
    obs_facts("恰好用完却没有成功");
    return 0;
  }
  code = lainvm_quota_charge(&q, 1);
  if (code != LAINVM_QUOTA_TRAP) {
    obs_facts("多一字节没有被拒：返回 %d（期望 %d）", code,
              (int)LAINVM_QUOTA_TRAP);
    return 0;
  }
  if (q.used != 4096 || q.rejected != 1) {
    obs_facts("被拒之后账目动了：used=%llu rejected=%llu（期望 4096 / 1）",
              (unsigned long long)q.used, (unsigned long long)q.rejected);
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 3)+6)+7) 同一个账户在 TCB 生命周期里的完整账目：admit 扣整块、跑动（含
 * `#alloca` 与返回时的水位回退）不改账、**真的销毁**之后才归还。 */
static int case_quota_tcb_lifecycle(void) {
  Rig rig;
  LainVmQuota q;
  LainVmTcb *tcb;

  if (rig_load(&rig, k_prog_alloca, 0) != 0) return 0;
  lainvm_quota_init(&q, 4096);
  tcb = lainvm_tcb_new(rig.image, &rig.space, 2, 2, 64, 4096, &q);
  if (!tcb) {
    rig_free(&rig);
    obs_facts("限额刚好够却 admit 失败");
    return 0;
  }
  lainvm_tcb_free(rig.tcb);
  rig.tcb = tcb; /* 用挂了账户的那个跑 */
  (void)rig_run(&rig, "use_alloca");
  if (g_obs.kind != 0 || g_obs.value != 7) {
    rig_free(&rig);
    obs_facts("挂了账户的 TCB 没跑通（kind=%d value=%llu）", g_obs.kind,
              (unsigned long long)g_obs.value);
    return 0;
  }
  if (q.used != 4096 || q.charges != 1 || q.releases != 0) {
    rig_free(&rig);
    obs_facts("跑完账目不对：used=%llu charges=%llu releases=%llu"
              "（期望 4096 / 1 / 0）",
              (unsigned long long)q.used, (unsigned long long)q.charges,
              (unsigned long long)q.releases);
    return 0;
  }
  lainvm_tcb_free(rig.tcb);
  rig.tcb = NULL;
  if (q.used != 0 || q.releases != 1) {
    rig_free(&rig);
    obs_facts("销毁之后没有按规则归还：used=%llu releases=%llu（期望 0 / 1）",
              (unsigned long long)q.used, (unsigned long long)q.releases);
    return 0;
  }
  rig_free(&rig);
  obs_value(1);
  return 0;
}

/* 4)+3) 两个子执行共用一个账户：后一个只能看到余额，谁都不能各拿一份完整额度；
 * 被拒的 admit 不留扣账；先销毁的那个把额度还回来。 */
static int case_quota_children_share(void) {
  Rig rig;
  LainVmQuota q;
  LainVmTcb *a;
  LainVmTcb *b;

  if (rig_load(&rig, k_prog_alloca, 0) != 0) return 0;
  lainvm_quota_init(&q, 6144);
  a = lainvm_tcb_new(rig.image, &rig.space, 2, 2, 64, 4096, &q);
  if (!a) {
    rig_free(&rig);
    obs_facts("第一个子执行 admit 失败");
    return 0;
  }
  if (q.used != 4096) {
    lainvm_tcb_free(a);
    rig_free(&rig);
    obs_facts("第一个子执行之后账目是 %llu（期望 4096）",
              (unsigned long long)q.used);
    return 0;
  }
  b = lainvm_tcb_new(rig.image, &rig.space, 3, 3, 64, 4096, &q);
  if (b != NULL) {
    lainvm_tcb_free(b);
    lainvm_tcb_free(a);
    rig_free(&rig);
    obs_facts("第二个子执行拿到了完整额度（used=%llu，只该剩 2048）",
              (unsigned long long)q.used);
    return 0;
  }
  if (q.used != 4096) {
    lainvm_tcb_free(a);
    rig_free(&rig);
    obs_facts("被拒的 admit 留下了扣账：used=%llu（期望 4096）",
              (unsigned long long)q.used);
    return 0;
  }
  b = lainvm_tcb_new(rig.image, &rig.space, 3, 3, 64, 2048, &q); /* 只剩 2048 */
  if (!b) {
    lainvm_tcb_free(a);
    rig_free(&rig);
    obs_facts("余额够（2048）却 admit 失败");
    return 0;
  }
  if (q.used != 6144) {
    lainvm_tcb_free(b);
    lainvm_tcb_free(a);
    rig_free(&rig);
    obs_facts("两个子执行之后账目是 %llu（期望 6144）",
              (unsigned long long)q.used);
    return 0;
  }
  lainvm_tcb_free(a);
  if (q.used != 2048) {
    lainvm_tcb_free(b);
    rig_free(&rig);
    obs_facts("销毁一个子执行之后账目是 %llu（期望 2048）",
              (unsigned long long)q.used);
    return 0;
  }
  lainvm_tcb_free(b);
  rig_free(&rig);
  obs_value(1);
  return 0;
}

/* 5) 分配失败不残留扣账：预扣成功但底层分配失败时必须回滚。
 * 用 2^60 字节的栈让 calloc 在**任何**平台上都失败（不依赖 overcommit 行为）。 */
static int case_quota_failed_alloc_no_residue(void) {
  Rig rig;
  LainVmQuota q;
  LainVmTcb *tcb;

  if (rig_load(&rig, k_prog_alloca, 0) != 0) return 0;
  lainvm_quota_init(&q, 0); /* 不限额：预扣一定成功，失败必须发生在分配那一步 */
  tcb = lainvm_tcb_new(rig.image, &rig.space, 2, 2, 64, 1ull << 60, &q);
  if (tcb) {
    lainvm_tcb_free(tcb);
    rig_free(&rig);
    obs_facts("2^60 字节的栈居然分配成功了 —— 这条用例打不到回滚路径");
    return 0;
  }
  if (q.used != 0 || q.charges != 1 || q.releases != 1) {
    rig_free(&rig);
    obs_facts("分配失败留下了扣账：used=%llu charges=%llu releases=%llu"
              "（期望 0 / 1 / 1）",
              (unsigned long long)q.used, (unsigned long long)q.charges,
              (unsigned long long)q.releases);
    return 0;
  }
  rig_free(&rig);
  obs_value(1);
  return 0;
}

/* 8) 不同切片的 fuel 不改变总配额判断：同一份程序，一个每步一片、一个一次跑完。 */
static int case_quota_fuel_independent(void) {
  Rig a;
  Rig b;
  LainVmQuota qa;
  LainVmQuota qb;
  LainVmTcb *ta;
  LainVmTcb *tb;

  if (rig_load(&a, k_prog_alloca, 0) != 0) return 0;
  if (rig_load(&b, k_prog_alloca, 0) != 0) {
    rig_free(&a);
    return 0;
  }
  lainvm_quota_init(&qa, 8192);
  lainvm_quota_init(&qb, 8192);
  ta = lainvm_tcb_new(a.image, &a.space, 2, 2, 64, 4096, &qa);
  tb = lainvm_tcb_new(b.image, &b.space, 2, 2, 64, 4096, &qb);
  if (!ta || !tb) {
    if (ta) lainvm_tcb_free(ta);
    if (tb) lainvm_tcb_free(tb);
    rig_free(&a);
    rig_free(&b);
    obs_facts("挂了账户的 TCB 建不起来");
    return 0;
  }
  lainvm_tcb_free(a.tcb);
  a.tcb = ta;
  lainvm_tcb_free(b.tcb);
  b.tcb = tb;
  (void)rig_run_sliced(&a, "use_alloca", 1);        /* 每步一片 */
  (void)rig_run_sliced(&b, "use_alloca", 1000000);  /* 一次跑完 */
  if (qa.used != qb.used || qa.charges != qb.charges || qa.peak != qb.peak ||
      qa.used != 4096) {
    rig_free(&a);
    rig_free(&b);
    obs_facts("不同 fuel 下账目不同：used %llu/%llu charges %llu/%llu peak "
              "%llu/%llu（期望一样，且 used=4096）",
              (unsigned long long)qa.used, (unsigned long long)qb.used,
              (unsigned long long)qa.charges, (unsigned long long)qb.charges,
              (unsigned long long)qa.peak, (unsigned long long)qb.peak);
    return 0;
  }
  rig_free(&a);
  rig_free(&b);
  obs_value(1);
  return 0;
}

/* 4b) 宿主暂存区也要过账户：余额不够就**不拿内存**、账目不动、状态码是 1044；
 * 释放宿主时按已计账字节归还。 */
static int case_quota_host_scratch_charged(void) {
  LainMetaHost *host = NULL;
  LainMetaHost *small = NULL;
  LainVmQuota q;
  LainVmQuota tiny;
  uint32_t size;
  void *p;

  host = lainmeta_host_new();
  small = lainmeta_host_new();
  if (!host || !small) {
    obs_facts("host 建不起来");
    goto done;
  }
  if (lainmeta_host_add_source(host, "std/prelude.lain", "let x = 1;\n", 11) !=
          0 ||
      lainmeta_host_add_source(small, "std/prelude.lain", "let x = 1;\n", 11) !=
          0) {
    obs_facts("登记源码失败");
    goto done;
  }
  /* 不限额：先看它到底扣不扣账。 */
  lainvm_quota_init(&q, 0);
  lainmeta_host_attach_quota(host, &q);
  size = 0;
  p = lainmeta_host_scratch(host, &size);
  if (!p || size == 0) {
    obs_facts("暂存区拿不到");
    goto done;
  }
  if (q.used != size || q.charges != 1) {
    obs_facts("暂存区没有按实际承诺扣账：used=%llu size=%u charges=%llu",
              (unsigned long long)q.used, (unsigned)size,
              (unsigned long long)q.charges);
    goto done;
  }
  /* 限额远小于暂存区：必须拿不到、账目不动、状态是配额 trap。 */
  lainvm_quota_init(&tiny, 1024);
  lainmeta_host_attach_quota(small, &tiny);
  size = 12345;
  p = lainmeta_host_scratch(small, &size);
  if (p != NULL || tiny.used != 0 || size != 0) {
    obs_facts("限额不够时暂存区没有稳定拒绝：ptr=%p used=%llu size=%u",
              p, (unsigned long long)tiny.used, (unsigned)size);
    goto done;
  }
  if (lainmeta_host_status(small) != (uint32_t)LAINVM_QUOTA_TRAP) {
    obs_facts("暂存区被拒的状态是 %u（期望 %d）",
              (unsigned)lainmeta_host_status(small), (int)LAINVM_QUOTA_TRAP);
    goto done;
  }
  if (lainmeta_host_output_length(host) != 0) {
    obs_facts("还没写就有输出了（长度 %u）",
              (unsigned)lainmeta_host_output_length(host));
    goto done;
  }
  lainmeta_host_free(host);
  host = NULL;
  if (q.used != 0) {
    obs_facts("释放宿主之后没有归还：used=%llu（期望 0）",
              (unsigned long long)q.used);
    goto done;
  }
  obs_value(1);

done:
  if (small) lainmeta_host_free(small);
  if (host) lainmeta_host_free(host);
  return 0;
}

/* --- capability 组：最小内存能力模型 ----------------------------------------
 *
 * 这一组测的是**模型层**（seed/src/vm/memcap.c）的契约，**不是** VM 的 load/store
 * 通路。通路接线是交接 §6.5 的第 5 步，报告里「模型通过」与「实际 VM 通路通过」
 * 必须分开说，不能合并宣称完成。
 *
 * 台架里的"供给方"就是本文件：底层存储是一块真实 malloc 的缓冲，模型只登记、不分配。
 */
#define CAP_CTX 7u   /* 主 / 父上下文标签 */
#define CAP_CHILD 8u /* 子调用、别的上下文标签 */

typedef struct {
  LainVmMemTable table;
  LainVmSpace space;
  uint8_t *storage;
  uint64_t size;
  LainVmMemHandle object;
} CapRig;

/* `generation_base`：句柄里不带表身份，在同一个宿主位置重建上下文时靠**更大的
 * 代数基数**让旧引用失效（作者定的"大卡"：引用 16 B = 句柄 8 B + 偏移 8 B）。 */
static int cap_rig_open(CapRig *rig, uint64_t size, uint32_t space_rights,
                        uint64_t generation_base) {
  memset(rig, 0, sizeof(*rig));
  rig->size = size;
  rig->storage = new_buffer((size_t)size);
  if (rig->storage == NULL) return -1;
  lainvm_memcap_init(&rig->table, generation_base);
  lainvm_space_init(&rig->space);
  rig->object = lainvm_memcap_object_add(&rig->table, (uintptr_t)rig->storage,
                                         size, CAP_CTX);
  if (lainvm_memcap_handle_none(rig->object)) return -1;
  if (add(&rig->space, (uintptr_t)rig->storage, size, space_rights,
          CAP_CTX) != 0)
    return -1;
  return 0;
}

static void cap_rig_close(CapRig *rig) {
  free(rig->storage);
  rig->storage = NULL;
}

static LainVmMemHandle cap_grant(CapRig *rig, uint64_t offset, uint64_t length,
                                 uint32_t rights, uint64_t owner,
                                 int32_t *code) {
  LainVmMemHandle handle = lainvm_memcap_no_handle();
  int32_t result = lainvm_memcap_grant(&rig->table, rig->object, offset,
                                       length, rights, owner, &handle);
  if (code != NULL) *code = result;
  return handle;
}

static LainVmMemRef cap_ref(LainVmMemHandle cap, uint64_t offset) {
  LainVmMemRef ref;
  ref.cap = cap;
  ref.offset = offset;
  return ref;
}

static int32_t cap_read64(CapRig *rig, LainVmMemRef ref, uint64_t owner,
                          uint64_t *value, int32_t *code) {
  uint64_t got = 0;
  int32_t result = (int32_t)lainvm_memcap_read(
      &rig->table, &rig->space, ref, owner, sizeof(got), &got, code);
  if (value != NULL) *value = got;
  return result;
}

static int32_t cap_write64(CapRig *rig, LainVmMemRef ref, uint64_t owner,
                           uint64_t value, int32_t *code) {
  return (int32_t)lainvm_memcap_write(&rig->table, &rig->space, ref, owner,
                                      sizeof(value), &value, code);
}

/* 直接看宿主缓冲里有什么（证明"确实写下去了"或"确实没被改"）。 */
static uint64_t cap_host_u64(const CapRig *rig, uint64_t offset) {
  uint64_t seen = 0;
  if (offset + sizeof(seen) > rig->size) return 0;
  memcpy(&seen, rig->storage + offset, sizeof(seen));
  return seen;
}

/* 拒绝类用例的公共收尾：主码对得上、不变量也成立才算 PASS。 */
static void cap_verdict(int ok, int32_t code, const char *what) {
  if (ok) {
    obs_trap(0, code);
  } else {
    obs_facts("%s（实际码=%d）", what, (int)code);
  }
}

/* §5 cap_live_rw：活对象 + 正确权限 + 范围内访问 → 读写结果正确。 */
static int case_cap_live_rw(void) {
  CapRig rig;
  LainVmMemHandle cap;
  LainVmMemRef ref;
  int32_t code = 0;
  uint64_t got = 0;
  const uint64_t want = 0x1122334455667788ull;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  cap = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                  &code);
  ref = cap_ref(cap, 8);
  if (code == 0 && cap_write64(&rig, ref, CAP_CTX, want, &code) == 0 &&
      cap_read64(&rig, ref, CAP_CTX, &got, &code) == 0 && got == want &&
      cap_host_u64(&rig, 8) == want) {
    obs_value(1);
  } else {
    obs_facts("code=%d 读回=%llu 宿主=%llu（期望 %llu）", (int)code,
              (unsigned long long)got,
              (unsigned long long)cap_host_u64(&rig, 8),
              (unsigned long long)want);
  }
  cap_rig_close(&rig);
  return 0;
}

/* §5 cap_bounds：越界一字节 / 越过授权子范围 / 偏移不可表示 → 稳定拒 9210，
 * 且目标内存一个字节都没改。 */
static int case_cap_bounds(void) {
  CapRig rig;
  LainVmMemHandle full, sub;
  int32_t c1 = 0, c2 = 0, c3 = 0, grant_code = 0;
  uint64_t before;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  full = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                   &grant_code);
  sub = cap_grant(&rig, 4, 8, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                  &grant_code);
  before = cap_host_u64(&rig, 56);
  (void)cap_write64(&rig, cap_ref(full, 57), CAP_CTX, 1, &c1); /* 57+8 > 64 */
  (void)cap_write64(&rig, cap_ref(sub, 7), CAP_CTX, 1, &c2);   /* 7+8 > 8 */
  (void)cap_write64(&rig, cap_ref(full, UINT64_MAX), CAP_CTX, 1, &c3);
  cap_verdict(c1 == 9210 && c2 == 9210 && c3 == 9210 &&
                  cap_host_u64(&rig, 56) == before,
              9210, "越界没有被完全拦住");
  if (!(c1 == 9210 && c2 == 9210 && c3 == 9210))
    obs_facts("码=%d/%d/%d（期望都是 9210），越界写后目标%s", (int)c1, (int)c2,
              (int)c3,
              cap_host_u64(&rig, 56) == before ? "未变" : "被改了");
  cap_rig_close(&rig);
  return 0;
}

/* §5 cap_rights：只读能力执行写入 → 稳定拒 9209，原值不变，读仍然可以。 */
static int case_cap_rights(void) {
  CapRig rig;
  LainVmMemHandle cap;
  int32_t write_code = 0, read_code = 0, grant_code = 0;
  uint64_t before, got = 0;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  before = cap_host_u64(&rig, 0);
  cap = cap_grant(&rig, 0, 64, LAINVM_MEM_READ, CAP_CTX, &grant_code);
  if (grant_code != 0) {
    obs_facts("只读授权失败 code=%d", (int)grant_code);
    cap_rig_close(&rig);
    return 0;
  }
  (void)cap_write64(&rig, cap_ref(cap, 0), CAP_CTX, 0, &write_code);
  (void)cap_read64(&rig, cap_ref(cap, 0), CAP_CTX, &got, &read_code);
  if (write_code == 9209 && cap_host_u64(&rig, 0) == before &&
      read_code == 0 && got == before) {
    obs_trap(0, 9209);
  } else {
    obs_facts("写码=%d（期望 9209）读码=%d 原值%s 读回=%llu", (int)write_code,
              (int)read_code,
              cap_host_u64(&rig, 0) == before ? "未变" : "被改了",
              (unsigned long long)got);
  }
  cap_rig_close(&rig);
  return 0;
}

/* §5 cap_revoke：保存引用后撤销 → 读、写都拒 9207。 */
static int case_cap_revoke(void) {
  CapRig rig;
  LainVmMemHandle cap;
  LainVmMemRef ref;
  uint32_t revoked = 0;
  int32_t code = 0, read_code = 0, write_code = 0;
  uint64_t got = 0;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  cap = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                  &code);
  ref = cap_ref(cap, 0);
  if (cap_write64(&rig, ref, CAP_CTX, 0x1234, &code) != 0 ||
      lainvm_memcap_revoke_owner(&rig.table, CAP_CTX, &revoked) != 0 ||
      revoked != 1) {
    obs_facts("撤销前就写不了（code=%d）或撤销计数=%u", (int)code,
              (unsigned)revoked);
    cap_rig_close(&rig);
    return 0;
  }
  (void)cap_read64(&rig, ref, CAP_CTX, &got, &read_code);
  (void)cap_write64(&rig, ref, CAP_CTX, 0x9999, &write_code);
  if (read_code == 9207 && write_code == 9207 &&
      cap_host_u64(&rig, 0) == 0x1234) {
    obs_trap(0, 9207);
  } else {
    obs_facts("读码=%d 写码=%d（期望都是 9207）内存=%llu", (int)read_code,
              (int)write_code, (unsigned long long)cap_host_u64(&rig, 0));
  }
  cap_rig_close(&rig);
  return 0;
}

/* §5 cap_same_address_reuse：撤销后**在同址**创建新对象 → 新引用成功，旧引用失败。
 * 同址是主动安排的：供给方把同一块存储再给一次，不靠分配器碰巧。 */
static int case_cap_same_address_reuse(void) {
  CapRig rig;
  LainVmMemHandle cap, fresh;
  LainVmMemRef old_ref, new_ref;
  uint32_t revoked = 0;
  int32_t code = 0, old_code = 0;
  uint64_t got = 0;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  cap = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                  &code);
  old_ref = cap_ref(cap, 0);
  if (cap_write64(&rig, old_ref, CAP_CTX, 0x1111, &code) != 0 ||
      lainvm_memcap_revoke_owner(&rig.table, CAP_CTX, &revoked) != 0 ||
      lainvm_memcap_object_release(&rig.table, rig.object) != 0) {
    obs_facts("前置失败 code=%d", (int)code);
    cap_rig_close(&rig);
    return 0;
  }
  rig.object = lainvm_memcap_object_add(&rig.table, (uintptr_t)rig.storage, 64,
                                        CAP_CTX);
  fresh = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                    &code);
  new_ref = cap_ref(fresh, 0);
  (void)cap_read64(&rig, old_ref, CAP_CTX, &got, &old_code);
  if (code == 0 && cap_write64(&rig, new_ref, CAP_CTX, 0x2222, &code) == 0 &&
      cap_read64(&rig, new_ref, CAP_CTX, &got, &code) == 0 && got == 0x2222 &&
      old_code == 9208 && cap_host_u64(&rig, 0) == 0x2222) {
    obs_value(1);
  } else {
    obs_facts("旧引用码=%d（期望 9208）新引用码=%d 读回=%llu 内存=%llu",
              (int)old_code, (int)code, (unsigned long long)got,
              (unsigned long long)cap_host_u64(&rig, 0));
  }
  cap_rig_close(&rig);
  return 0;
}

/* §5 cap_copy_revoke：复制引用（直接赋值 / 经内存往返）后撤销 → 所有副本都失效。 */
static int case_cap_copy_revoke(void) {
  CapRig rig;
  LainVmMemHandle cap;
  LainVmMemRef ref, copy, via_memory;
  uint8_t bytes[sizeof(LainVmMemRef)];
  uint32_t revoked = 0;
  int32_t code = 0, c1 = 0, c2 = 0, c3 = 0;
  uint64_t got = 0;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  cap = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                  &code);
  ref = cap_ref(cap, 0);
  copy = ref;
  memcpy(bytes, &ref, sizeof(ref));
  memcpy(&via_memory, bytes, sizeof(via_memory));
  if (cap_write64(&rig, ref, CAP_CTX, 5, &code) != 0 ||
      lainvm_memcap_revoke_owner(&rig.table, CAP_CTX, &revoked) != 0 ||
      revoked != 1) {
    obs_facts("前置失败（写码=%d 撤销计数=%u）", (int)code, (unsigned)revoked);
    cap_rig_close(&rig);
    return 0;
  }
  (void)cap_read64(&rig, ref, CAP_CTX, &got, &c1);
  (void)cap_read64(&rig, copy, CAP_CTX, &got, &c2);
  (void)cap_read64(&rig, via_memory, CAP_CTX, &got, &c3);
  if (c1 == 9207 && c2 == 9207 && c3 == 9207) {
    obs_trap(0, 9207);
  } else {
    obs_facts("三个副本的码=%d/%d/%d（期望都是 9207）", (int)c1, (int)c2,
              (int)c3);
  }
  cap_rig_close(&rig);
  return 0;
}

/* §5 cap_memory_roundtrip + §4.3：受检引用写进内存再取回。
 * 存活时可用；部分覆盖只能**变坏**（不能扩大授权）；撤销 + 同址复用后不能访问新对象。 */
static int case_cap_memory_roundtrip(void) {
  CapRig rig;
  LainVmMemHandle cap, fresh;
  LainVmMemRef ref, back, damaged;
  uint8_t bytes[sizeof(LainVmMemRef)];
  uint32_t revoked = 0;
  int32_t code = 0, live_code = 0, damage_code = 0, after_code = 0;
  uint64_t live_value = 0, damage_value = 0, after_value = 0;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  cap = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                  &code);
  ref = cap_ref(cap, 16);
  if (cap_write64(&rig, ref, CAP_CTX, 0xABCD, &code) != 0) {
    obs_facts("前置写失败 code=%d", (int)code);
    cap_rig_close(&rig);
    return 0;
  }
  /* 受检引用经普通字节复制往返（"写进内存再取回"） */
  memcpy(bytes, &ref, sizeof(ref));
  memcpy(&back, bytes, sizeof(back));
  (void)cap_read64(&rig, back, CAP_CTX, &live_value, &live_code);
  /* 部分覆盖：改掉偏移字段的最低字节 → 只能变坏 */
  bytes[0] = (uint8_t)(bytes[0] ^ 0xFFu);
  memcpy(&damaged, bytes, sizeof(damaged));
  (void)cap_read64(&rig, damaged, CAP_CTX, &damage_value, &damage_code);
  bytes[0] = (uint8_t)(bytes[0] ^ 0xFFu);
  /* 撤销 + 同址复用，再拿内存里取回的老引用去访问 */
  if (lainvm_memcap_revoke_owner(&rig.table, CAP_CTX, &revoked) != 0 ||
      lainvm_memcap_object_release(&rig.table, rig.object) != 0) {
    obs_facts("撤销 / 释放失败");
    cap_rig_close(&rig);
    return 0;
  }
  rig.object = lainvm_memcap_object_add(&rig.table, (uintptr_t)rig.storage, 64,
                                        CAP_CTX);
  fresh = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                    &code);
  (void)cap_write64(&rig, cap_ref(fresh, 16), CAP_CTX, 0x7777, &code);
  memcpy(&back, bytes, sizeof(back)); /* 老引用：来自内存，代数已过期 */
  (void)cap_read64(&rig, back, CAP_CTX, &after_value, &after_code);
  if (live_code == 0 && live_value == 0xABCD && damage_code != 0 &&
      after_code == 9208 && after_value == 0 &&
      cap_host_u64(&rig, 16) == 0x7777) {
    obs_value(1);
  } else {
    obs_facts("存活码=%d 值=%llu 覆盖码=%d（值=%llu）复用后码=%d 值=%llu 内存=%llu",
              (int)live_code, (unsigned long long)live_value, (int)damage_code,
              (unsigned long long)damage_value, (int)after_code,
              (unsigned long long)after_value,
              (unsigned long long)cap_host_u64(&rig, 16));
  }
  cap_rig_close(&rig);
  return 0;
}

/* §5 cap_forged_reference + §2：改代数 / 范围 / 槽号、拿别的上下文的能力、伪造整数，
 * 都不能扩大授权。**句柄不带表身份**（作者定的大卡），所以"假表号"这一路没了：
 * 同址重建改由代数基数检出（见 cap_context_reuse）。主断言：拿**别的上下文**的
 * 能力记录来用 → 9206。 */
static int case_cap_forged_reference(void) {
  CapRig rig;
  CapRig other;
  LainVmMemHandle mine, theirs, foreign;
  LainVmMemRef widened, bumped, forged_slot, forged_int;
  int32_t code = 0, foreign_code = 0, widen_code = 0, gen_code = 0;
  int32_t slot_code = 0, int_code = 0, cross_code = 0;
  uint64_t before, other_before, got = 0;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  mine = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                   &code);
  theirs = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE,
                     CAP_CHILD, &code);
  before = cap_host_u64(&rig, 0);
  (void)cap_read64(&rig, cap_ref(theirs, 0), CAP_CTX, &got, &foreign_code);
  widened = cap_ref(mine, 65);
  (void)cap_read64(&rig, widened, CAP_CTX, &got, &widen_code);
  bumped = cap_ref(mine, 0);
  bumped.cap.generation += 1;
  (void)cap_read64(&rig, bumped, CAP_CTX, &got, &gen_code);
  /* 句柄不带表身份了，所以伪造的对象是**槽号**（越界 → 9207）。 */
  forged_slot = cap_ref(mine, 0);
  forged_slot.cap.slot = 0xFFFFFFFFu;
  (void)cap_read64(&rig, forged_slot, CAP_CTX, &got, &slot_code);
  /* 伪造整数：位布局是 代数(32) | 槽号(32)。槽号是真的、代数不是 → 9208。 */
  forged_int = lainvm_memcap_int_to_ref((uint64_t)0xDEADBEEFull << 32, 0);
  (void)cap_read64(&rig, forged_int, CAP_CTX, &got, &int_code);

  /* 另一张表（另一个上下文的空间）里的活能力，也不能拿来用 */
  if (cap_rig_open(&other, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 2) != 0) {
    obs_facts("第二台架起不来");
    cap_rig_close(&rig);
    return 0;
  }
  foreign = cap_grant(&other, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE,
                      CAP_CTX, &code);
  other_before = cap_host_u64(&other, 0);
  (void)cap_read64(&rig, cap_ref(foreign, 0), CAP_CTX, &got, &cross_code);
  if (foreign_code == 9206 && widen_code == 9210 && gen_code == 9208 &&
      slot_code == 9207 && int_code == 9208 && cross_code == 9208 &&
      cap_host_u64(&rig, 0) == before &&
      cap_host_u64(&other, 0) == other_before) {
    obs_trap(0, 9206);
  } else {
    obs_facts("别人的能力=%d 越界=%d 改代数=%d 假槽号=%d 假整数=%d 跨上下文=%d"
              "（期望 9206/9210/9208/9207/9208/9208）",
              (int)foreign_code, (int)widen_code, (int)gen_code,
              (int)slot_code, (int)int_code, (int)cross_code);
  }
  cap_rig_close(&other);
  cap_rig_close(&rig);
  return 0;
}

/* §5 cap_int_roundtrip + §4.2：对象存活时，引用转整数再转回来 → 有确定结果。 */
static int case_cap_int_roundtrip(void) {
  CapRig rig;
  LainVmMemHandle cap;
  LainVmMemRef ref, back;
  uint64_t bits, again, got = 0;
  const uint64_t want = 0x0123456789ABCDEFull;
  int32_t code = 0;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  cap = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                  &code);
  ref = cap_ref(cap, 24);
  bits = lainvm_memcap_ref_to_int(ref);
  again = lainvm_memcap_ref_to_int(ref);
  back = lainvm_memcap_int_to_ref(bits, 24);
  if (code == 0 && cap_write64(&rig, ref, CAP_CTX, want, &code) == 0 &&
      cap_read64(&rig, back, CAP_CTX, &got, &code) == 0 && got == want &&
      bits == again && back.cap.slot == ref.cap.slot &&
      back.cap.generation == ref.cap.generation) {
    obs_value(1);
  } else {
    obs_facts("code=%d 读回=%llu 位=%llx/%llx 句柄%s", (int)code,
              (unsigned long long)got, (unsigned long long)bits,
              (unsigned long long)again,
              back.cap.slot == ref.cap.slot ? "一致" : "不一致");
  }
  cap_rig_close(&rig);
  return 0;
}

/* §5 cap_int_after_revoke + §4.2：保留整数，撤销并复用对象后再转换 → 不能恢复权限。 */
static int case_cap_int_after_revoke(void) {
  CapRig rig;
  LainVmMemHandle cap, fresh;
  LainVmMemRef back;
  uint64_t bits, got = 0;
  uint32_t revoked = 0;
  int32_t code = 0, after_code = 0;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  cap = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                  &code);
  bits = lainvm_memcap_ref_to_int(cap_ref(cap, 0));
  if (lainvm_memcap_revoke_owner(&rig.table, CAP_CTX, &revoked) != 0 ||
      lainvm_memcap_object_release(&rig.table, rig.object) != 0) {
    obs_facts("撤销 / 释放失败");
    cap_rig_close(&rig);
    return 0;
  }
  rig.object = lainvm_memcap_object_add(&rig.table, (uintptr_t)rig.storage, 64,
                                        CAP_CTX);
  fresh = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                    &code);
  (void)cap_write64(&rig, cap_ref(fresh, 0), CAP_CTX, 0x31, &code);
  back = lainvm_memcap_int_to_ref(bits, 0);
  (void)cap_read64(&rig, back, CAP_CTX, &got, &after_code);
  if (after_code == 9208 && cap_host_u64(&rig, 0) == 0x31) {
    obs_trap(0, 9208);
  } else {
    obs_facts("整数复原后的码=%d（期望 9208）读到 %llu", (int)after_code,
              (unsigned long long)got);
  }
  cap_rig_close(&rig);
  return 0;
}

/* §5 cap_context_reuse：销毁并在同一宿主位置重建对象表 / 空间（代数基数更大）→
 * 旧上下文的引用不能在新上下文里复活，**槽位被重新占用之后也不行**。 */
static int case_cap_context_reuse(void) {
  CapRig rig;
  LainVmMemHandle cap, fresh;
  LainVmMemRef ref, back;
  uint64_t bits, got = 0;
  int32_t code = 0, stale_code = 0, ref_code = 0, int_code = 0, fresh_code = 0;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  cap = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                  &code);
  ref = cap_ref(cap, 0);
  bits = lainvm_memcap_ref_to_int(ref);
  /* 同一块内存、更高的代数基数：旧上下文的一切都不再有效 */
  lainvm_memcap_init(&rig.table, 1000);
  (void)cap_read64(&rig, ref, CAP_CTX, &got, &stale_code);
  /* 新表自己必须能用（否则"拒绝"可能只是因为表坏了）；槽位在这里被重新占用 */
  rig.object = lainvm_memcap_object_add(&rig.table, (uintptr_t)rig.storage, 64,
                                        CAP_CTX);
  fresh = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                    &fresh_code);
  if (fresh_code == 0)
    fresh_code = cap_write64(&rig, cap_ref(fresh, 0), CAP_CTX, 0x0BAD, &code);
  back = lainvm_memcap_int_to_ref(bits, 0);
  (void)cap_read64(&rig, back, CAP_CTX, &got, &int_code);
  (void)cap_read64(&rig, ref, CAP_CTX, &got, &ref_code);
  if (stale_code == 9207 && ref_code == 9208 && int_code == 9208 &&
      fresh_code == 0 && cap_host_u64(&rig, 0) == 0x0BAD) {
    obs_trap(0, 9208);
  } else {
    obs_facts("重建后槽未占用=%d（期望 9207）槽复用后旧引用=%d 旧整数=%d "
              "新表可用性=%d（期望 9208/9208/0）",
              (int)stale_code, (int)ref_code, (int)int_code, (int)fresh_code);
  }
  cap_rig_close(&rig);
  return 0;
}

/* §5 cap_child_borrow + §3：把父对象授权给同步子调用 → 子期间可用，父对象仍有效；
 * 子调用结束（按上下文撤销）不得撤销父对象的能力。 */
static int case_cap_child_borrow(void) {
  CapRig rig;
  LainVmMemHandle parent, child;
  uint32_t revoked = 0;
  int32_t code = 0, child_code = 0, parent_read = 0, parent_read2 = 0;
  int32_t parent_write = 0;
  uint64_t committed_before, child_value = 0, got = 0;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  parent = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                     &code);
  child = cap_grant(&rig, 0, 16, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CHILD,
                    &code);
  committed_before = rig.table.committed;
  if (cap_write64(&rig, cap_ref(child, 0), CAP_CHILD, 0x55, &code) != 0) {
    obs_facts("子调用期间就写不了 code=%d", (int)code);
    cap_rig_close(&rig);
    return 0;
  }
  if (lainvm_memcap_revoke_owner(&rig.table, CAP_CHILD, &revoked) != 0 ||
      revoked != 1) {
    obs_facts("子上下文撤销失败（计数=%u）", (unsigned)revoked);
    cap_rig_close(&rig);
    return 0;
  }
  (void)cap_read64(&rig, cap_ref(child, 0), CAP_CHILD, &got, &child_code);
  (void)cap_read64(&rig, cap_ref(parent, 0), CAP_CTX, &child_value,
                   &parent_read);
  (void)cap_write64(&rig, cap_ref(parent, 0), CAP_CTX, 0x66, &parent_write);
  (void)cap_read64(&rig, cap_ref(parent, 0), CAP_CTX, &got, &parent_read2);
  if (child_code == 9207 && parent_read == 0 && child_value == 0x55 &&
      parent_write == 0 && parent_read2 == 0 && got == 0x66 &&
      rig.table.committed == committed_before) {
    obs_value(1);
  } else {
    obs_facts("子码=%d 父读=%d（值=%llu）父写=%d 父再读=%d（值=%llu）账=%llu"
              "（期望 9207/0/0x55/0/0/0x66/%llu）",
              (int)child_code, (int)parent_read,
              (unsigned long long)child_value, (int)parent_write,
              (int)parent_read2, (unsigned long long)got,
              (unsigned long long)rig.table.committed,
              (unsigned long long)committed_before);
  }
  cap_rig_close(&rig);
  return 0;
}

/* §5 cap_space_switch：能力有效，但对应的存储在新空间里没被授权 → 拒 9212；
 * 换空间不会给旧引用自动补权限。 */
static int case_cap_space_switch(void) {
  CapRig rig;
  LainVmSpace other;
  LainVmMemHandle cap;
  int32_t code = 0, other_code = 0, home_code = 0;
  uint64_t got = 0;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  cap = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                  &code);
  lainvm_space_init(&other);
  (void)lainvm_memcap_read(&rig.table, &other, cap_ref(cap, 0), CAP_CTX,
                           sizeof(got), &got, &other_code);
  (void)lainvm_memcap_read(&rig.table, &rig.space, cap_ref(cap, 0), CAP_CTX,
                           sizeof(got), &got, &home_code);
  if (other_code == 9212 && home_code == 0) {
    obs_trap(0, 9212);
  } else {
    obs_facts("新空间码=%d 原空间码=%d（期望 9212/0）", (int)other_code,
              (int)home_code);
  }
  cap_rig_close(&rig);
  return 0;
}

/* §5 cap_host_access + §7：统一受检宿主适配器 —— **先解析、再产生副作用**。
 * 失效引用与越界引用都必须在回调产生任何副作用之前被拒。 */
static int case_cap_host_access(void) {
  CapRig rig;
  LainVmMemHandle cap, live;
  LainVmMemRef ref;
  uint64_t side = 0, before;
  const uint64_t payload = 0xA5A5A5A5A5A5A5A5ull;
  uint32_t revoked = 0;
  int32_t code = 0, revoked_code = 0, oob_code = 0;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  cap = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                  &code);
  live = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CHILD,
                   &code);
  ref = cap_ref(cap, 0);
  before = cap_host_u64(&rig, 0);
  if (lainvm_memcap_revoke_owner(&rig.table, CAP_CTX, &revoked) != 0) {
    obs_facts("撤销失败");
    cap_rig_close(&rig);
    return 0;
  }
  (void)lainvm_memcap_host_write(&rig.table, &rig.space, ref, CAP_CTX,
                                 sizeof(payload), &payload, &side,
                                 &revoked_code);
  (void)lainvm_memcap_host_write(&rig.table, &rig.space, cap_ref(live, 60),
                                 CAP_CHILD, sizeof(payload), &payload, &side,
                                 &oob_code);
  if (revoked_code == 9207 && oob_code == 9210 && side == 0 &&
      cap_host_u64(&rig, 0) == before) {
    obs_trap(0, 9207);
  } else {
    obs_facts("失效码=%d 越界码=%d 副作用=%llu（期望 9207/9210/0）",
              (int)revoked_code, (int)oob_code, (unsigned long long)side);
  }
  cap_rig_close(&rig);
  return 0;
}

/* §5 cap_failure_cleanup：创建中途失败 → 不留活对象、不留活能力、不错误扣账。 */
static int case_cap_failure_cleanup(void) {
  CapRig rig;
  LainVmMemHandle handle, cap;
  uint64_t committed0, committed1;
  uint32_t objects0, i;
  int32_t code = 0, size0_bad = 0, wrap_bad = 0, range_code = 0;
  int32_t rights_code = 0, full_code = 0;
  uint64_t got = 0;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  committed0 = rig.table.committed;
  objects0 = rig.table.objects_live;
  /* (1) size == 0 */
  handle = lainvm_memcap_object_add(&rig.table, (uintptr_t)rig.storage, 0,
                                    CAP_CTX);
  if (!lainvm_memcap_handle_none(handle)) size0_bad = 1;
  /* (2) base + size 不可表示 */
  handle = lainvm_memcap_object_add(&rig.table, UINTPTR_MAX - 3u, 8, CAP_CTX);
  if (!lainvm_memcap_handle_none(handle)) wrap_bad = 1;
  /* (3) 授权范围越出对象 */
  range_code = lainvm_memcap_grant(&rig.table, rig.object, 60, 8,
                                   LAINVM_MEM_READ, CAP_CTX, &handle);
  /* (4) 权限为空 */
  rights_code = lainvm_memcap_grant(&rig.table, rig.object, 0, 64, 0, CAP_CTX,
                                    &handle);
  /* (5) 能力表满：灌满之后再要一条 */
  for (i = 0; i < LAINVM_MEMCAP_MAX_CAPS; i++) {
    int32_t made = lainvm_memcap_grant(&rig.table, rig.object, 0, 64,
                                       LAINVM_MEM_READ, CAP_CTX, &handle);
    if (made != 0) break;
  }
  full_code = lainvm_memcap_grant(&rig.table, rig.object, 0, 64,
                                  LAINVM_MEM_READ, CAP_CTX, &handle);
  committed1 = rig.table.committed;
  /* (6) 合法路径仍然可用：清掉旧上下文的能力，重新发一条并读写 */
  if (lainvm_memcap_revoke_owner(&rig.table, CAP_CTX, NULL) != 0) code = -1;
  cap = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                  &code);
  if (code == 0)
    code = cap_write64(&rig, cap_ref(cap, 0), CAP_CTX, 0x7E, &code);
  if (code == 0) code = cap_read64(&rig, cap_ref(cap, 0), CAP_CTX, &got, &code);
  if (committed0 == 64 && committed1 == committed0 &&
      rig.table.objects_live == objects0 && size0_bad == 0 && wrap_bad == 0 &&
      range_code == 9203 && rights_code == 9204 && full_code == 9202 &&
      code == 0 && got == 0x7E) {
    obs_value(1);
  } else {
    obs_facts("账 %llu→%llu 对象 %u→%u 空大小坏=%d 回绕坏=%d 越界=%d "
              "无权限=%d 满=%d 可用码=%d 读=%llu",
              (unsigned long long)committed0, (unsigned long long)committed1,
              (unsigned)objects0, (unsigned)rig.table.objects_live,
              (int)size0_bad, (int)wrap_bad, (int)range_code, (int)rights_code,
              (int)full_code, (int)code, (unsigned long long)got);
  }
  cap_rig_close(&rig);
  return 0;
}

/* §7：撤销访问权和释放存储是**两个动作** —— 撤销后额度不能归还；存储释放后
 * 手里还留着的能力必须被判「对象已不存活」（9211）。 */
static int case_cap_quota_two_actions(void) {
  CapRig rig;
  LainVmMemHandle survivor, doomed;
  uint64_t after_revoke, after_release;
  uint32_t revoked = 0;
  int32_t code = 0, gone_code = 0, doomed_code = 0;
  uint64_t got = 0;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  survivor = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE,
                       CAP_CTX, &code);
  doomed = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE,
                     CAP_CHILD, &code);
  if (lainvm_memcap_revoke_owner(&rig.table, CAP_CHILD, &revoked) != 0) {
    obs_facts("撤销失败");
    cap_rig_close(&rig);
    return 0;
  }
  after_revoke = rig.table.committed;
  (void)cap_read64(&rig, cap_ref(doomed, 0), CAP_CHILD, &got, &doomed_code);
  if (lainvm_memcap_object_release(&rig.table, rig.object) != 0) {
    obs_facts("释放失败");
    cap_rig_close(&rig);
    return 0;
  }
  after_release = rig.table.committed;
  (void)cap_read64(&rig, cap_ref(survivor, 0), CAP_CTX, &got, &gone_code);
  if (after_revoke == 64 && after_release == 0 && rig.table.released == 64 &&
      doomed_code == 9207 && gone_code == 9211) {
    obs_value(1);
  } else {
    obs_facts("撤销后账=%llu（期望 64）被撤销的能力码=%d（期望 9207）释放后账="
              "%llu（期望 0）已归还=%llu 存储没了的能力码=%d（期望 9211）",
              (unsigned long long)after_revoke, (int)doomed_code,
              (unsigned long long)after_release,
              (unsigned long long)rig.table.released, (int)gone_code);
  }
  cap_rig_close(&rig);
  return 0;
}

/* §7：切换 VSpace 之后**直接销毁 TCB** 的释放路径。
 * 契约：活窗口属于**租约所在的空间**，销毁时要在那个空间里精确撤销并释放；
 * 换到的那个空间一个字节都不能碰。（只测「切出再切回」是打不到这里的。）
 *
 * 所以这里先造出一个**Trap 时仍然活着**的窗口：`#alloca` 之后去踩一个没有授权的
 * 地址（1004）。Trap 不缩小水位，窗口就留在租约空间里 —— 然后换空间、直接销毁。 */
static const char *k_prog_alloc_then_trap =
    "#proc alloc_then_trap() -> #bits<64> {\n"
    "  %a = #alloca[#bits<8>](4)\n"
    "  #store[#bits<8>](7, %a)\n"
    "  %p = #int2ptr[#addr](4096)\n"
    "  %b = #load[#bits<8>](%p)\n"
    "  %w = #zext[#bits<64>](%b)\n"
    "  #return %w\n"
    "}\n";

static int case_cap_tcb_destroy_after_switch(void) {
  Rig rig;
  LainVmSpace other;
  L1Diagnostic diag;
  uint32_t before, after, other_live;

  if (rig_load(&rig, k_prog_alloc_then_trap, 4096) != 0) return 0;
  (void)rig_run(&rig, "alloc_then_trap");
  if (g_obs.kind != 1 || g_obs.trap_code != 1004) {
    rig_free(&rig);
    obs_facts("期望「先 alloca 再在域外地址上被拒 1004」，实际 kind=%d code=%d",
              g_obs.kind, (int)g_obs.trap_code);
    return 0;
  }
  if (lainvm_space_handle_none(rig.tcb->stack_window)) {
    rig_free(&rig);
    obs_facts("Trap 之后活窗口没了 —— 这条用例打不到「销毁时窗口还在」的路径");
    return 0;
  }
  lainvm_space_init(&other);
  diag.code = 0;
  if (lainvm_tcb_set_space(rig.tcb, &other, &diag) != 0) {
    rig_free(&rig);
    obs_facts("set_space 失败：code=%d %s", diag.code, diag.message);
    return 0;
  }
  before = rig.space.live_count;
  lainvm_tcb_free(rig.tcb);
  rig.tcb = NULL; /* 已经销毁，rig_free 不要再碰 */
  after = rig.space.live_count;
  other_live = other.live_count;
  rig_free(&rig);
  if (before >= 1 && after == before - 1 && other_live == 0) {
    obs_value(1);
  } else {
    obs_facts("销毁后租约所在空间的区段 %u→%u（期望少 1 段：活窗口要在它自己的"
              "空间里撤销），换到的空间 %u 段",
              (unsigned)before, (unsigned)after, (unsigned)other_live);
  }
  return 0;
}

/* §4.1 的第二个原型（探针）：**保留地址值**，另用 VM 元数据跟踪它对应哪条能力。
 *
 * 最小跟踪就是一张 {地址 -> 能力句柄} 的旁表。它必须回答的是「参数传递、返回、
 * 复制、写入内存再读出」时关联怎么保住。§4.1 已经写明：只跟踪临时值槽，或者按
 * 数值地址重新查询"现在的能力"，都**不能满足同址复用验收** —— 这一条把那个
 * "不能"量出来：
 *
 *   撤销 + 同址复用之后，地址这个**数字**没变。逃逸出去的旧地址再回来，旁表按
 *   地址查到的是**新对象**的能力，于是旧引用自动拿到新对象的权限（fail open）。
 *   旁表没有别的键可用 —— 两条地址相同的引用在 B 里根本无法区分。
 *
 * 这一条 PASS = 缺口按预期复现（B 这条路走不通），**不是**"B 正确"。 */
#define PROTO_B_SLOTS 8

typedef struct {
  uintptr_t address;
  LainVmMemHandle cap;
  uint64_t offset;
} ProtoBSlot;

typedef struct {
  ProtoBSlot slots[PROTO_B_SLOTS];
  uint32_t count;
} ProtoB;

/* 旁表按**地址**唯一：新登记覆盖旧的 —— 这正是"按数值地址查询**现在**的能力"。
 * 只有地址这一个键，两条地址相同的引用在 B 里无法区分，所以覆盖是唯一说得通的做法。 */
static void proto_b_track(ProtoB *probe, uintptr_t address,
                          LainVmMemHandle cap, uint64_t offset) {
  uint32_t i;
  for (i = 0; i < probe->count; i++) {
    if (probe->slots[i].address != address) continue;
    probe->slots[i].cap = cap;
    probe->slots[i].offset = offset;
    return;
  }
  if (probe->count >= PROTO_B_SLOTS) return;
  probe->slots[probe->count].address = address;
  probe->slots[probe->count].cap = cap;
  probe->slots[probe->count].offset = offset;
  probe->count += 1;
}

static int proto_b_find(const ProtoB *probe, uintptr_t address,
                        LainVmMemRef *out) {
  uint32_t i;
  for (i = 0; i < probe->count; i++) {
    if (probe->slots[i].address != address) continue;
    out->cap = probe->slots[i].cap;
    out->offset = probe->slots[i].offset;
    return 0;
  }
  return -1;
}

static int case_cap_protoB_stale_address_probe(void) {
  CapRig rig;
  ProtoB probe;
  LainVmMemHandle cap, fresh;
  LainVmMemRef via_probe;
  uintptr_t address, revived = 0;
  uint8_t escaped[sizeof(uintptr_t)];
  uint64_t got = 0;
  uint32_t revoked = 0;
  int32_t code = 0, live_code = 0, stale_code = 0;

  if (cap_rig_open(&rig, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1) != 0) {
    obs_facts("台架起不来");
    return 0;
  }
  memset(&probe, 0, sizeof(probe));
  cap = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                  &code);
  address = (uintptr_t)rig.storage;
  proto_b_track(&probe, address, cap, 0);
  /* (1) 直路：按地址查旁表 → 能用（B 在简单路径上没问题） */
  if (proto_b_find(&probe, address, &via_probe) != 0) {
    obs_facts("旁表连自己的地址都查不到");
    cap_rig_close(&rig);
    return 0;
  }
  (void)cap_read64(&rig, via_probe, CAP_CTX, &got, &live_code);
  /* 旧地址经内存往返逃逸出去（普通字节复制） */
  memcpy(escaped, &address, sizeof(address));
  memcpy(&revived, escaped, sizeof(revived));
  /* (2) 撤销 + 同址复用：地址这个数字没变 */
  if (lainvm_memcap_revoke_owner(&rig.table, CAP_CTX, &revoked) != 0 ||
      lainvm_memcap_object_release(&rig.table, rig.object) != 0) {
    obs_facts("撤销 / 释放失败");
    cap_rig_close(&rig);
    return 0;
  }
  rig.object = lainvm_memcap_object_add(&rig.table, (uintptr_t)rig.storage, 64,
                                        CAP_CTX);
  fresh = cap_grant(&rig, 0, 64, LAINVM_MEM_READ | LAINVM_MEM_WRITE, CAP_CTX,
                    &code);
  proto_b_track(&probe, address, fresh, 0);
  (void)cap_write64(&rig, cap_ref(fresh, 0), CAP_CTX, 0x4242, &code);
  got = 0;
  if (proto_b_find(&probe, revived, &via_probe) == 0)
    (void)cap_read64(&rig, via_probe, CAP_CTX, &got, &stale_code);
  if (live_code == 0 && stale_code == 0 && got == 0x4242) {
    obs_value(1); /* 缺口复现：逃逸的旧地址拿到了新对象的权限 */
  } else {
    obs_facts("探针没复现缺口：直路码=%d 陈旧地址码=%d 读到=%llu", (int)live_code,
              (int)stale_code, (unsigned long long)got);
  }
  cap_rig_close(&rig);
  return 0;
}

/* --- 用例表 ----------------------------------------------------------------- */
static const Case k_cases[] = {
    /* region */
    {"region_valid_read", "region", EXP_VALUE, 42, 0, case_region_valid_read},
    {"load_outside_region", "region", EXP_TRAP, 0, 1004,
     case_load_outside_region},
    {"region_insert_identity", "region", EXP_VALUE, 1, 0,
     case_region_insert_identity},
    {"region_remove_identity", "region", EXP_VALUE, 1, 0,
     case_region_remove_identity},
    {"region_remove_precise", "region", EXP_VALUE, 1, 0,
     case_region_remove_precise},
    {"region_full_64", "region", EXP_VALUE, 1, 0, case_region_full_64},
    {"region_overlap_reject", "region", EXP_VALUE, 1, 0,
     case_region_overlap_reject},
    {"region_adjacent_allow", "region", EXP_VALUE, 1, 0,
     case_region_adjacent_allow},
    {"region_bad_size_reject", "region", EXP_VALUE, 1, 0,
     case_region_bad_size_reject},
    {"region_range_tail", "region", EXP_VALUE, 1, 0, case_region_range_tail},
    {"region_handle_stale", "region", EXP_VALUE, 1, 0,
     case_region_handle_stale},
    {"region_handle_cross_space", "region", EXP_VALUE, 1, 0,
     case_region_handle_cross_space},
    {"region_handle_double_remove", "region", EXP_VALUE, 1, 0,
     case_region_handle_double_remove},
    /* stack */
    {"stack_lease_identity", "stack", EXP_VALUE, 1, 0,
     case_stack_lease_identity},
    {"stack_absent_trap", "stack", EXP_TRAP, 0, 1006, case_stack_absent_trap},
    {"stack_exhaust_watermark", "stack", EXP_TRAP, 0, 1007,
     case_stack_exhaust_watermark},
    {"stack_count_overflow", "stack", EXP_TRAP, 0, 1035,
     case_stack_count_overflow},
    {"stack_zero_count", "stack", EXP_TRAP, 0, 2024, case_stack_zero_count},
    {"stack_two_tcbs_one_space", "stack", EXP_VALUE, 1, 0,
     case_stack_two_tcbs_one_space},
    /* host */
    {"host_past_region", "host", EXP_TRAP, 0, 0, case_host_past_region},
    {"host_wrong_identity", "host", EXP_TRAP, 0, 0, case_host_wrong_identity},
    {"host_in_region_ok", "host", EXP_VALUE, 1, 0, case_host_in_region_ok},
    {"host_source_index_bounds", "host", EXP_VALUE, 1, 0,
     case_host_source_index_bounds},
    /* lifetime */
    /* owned storage（VSpace 申请、清零、登记、释放；按原账户归还） */
    {"owned_alloc_zeroed", "lease", EXP_VALUE, 1, 0, case_owned_alloc_zeroed},
    {"owned_alloc_quota_exact", "lease", EXP_VALUE, 1, 0,
     case_owned_alloc_quota_exact},
    {"owned_alloc_quota_reject_unchanged", "lease", EXP_VALUE, 1, 0,
     case_owned_alloc_quota_reject_unchanged},
    {"owned_alloc_failure_rolls_back_quota", "lease", EXP_VALUE, 1, 0,
     case_owned_alloc_failure_rolls_back_quota},
    {"owned_free_returns_quota", "lease", EXP_VALUE, 1, 0,
     case_owned_free_returns_quota},
    {"owned_double_free_reject", "lease", EXP_VALUE, 1, 0,
     case_owned_double_free_reject},
    {"owned_cross_space_reject", "lease", EXP_VALUE, 1, 0,
     case_owned_cross_space_reject},
    {"owned_free_while_borrowed_reject", "lease", EXP_VALUE, 1, 0,
     case_owned_free_while_borrowed_reject},
    /* external mapping（只授权与撤销：不 free、不扣账） */
    {"external_map_access", "lease", EXP_VALUE, 1, 0, case_external_map_access},
    {"external_unmap_rejects_access", "lease", EXP_VALUE, 1, 0,
     case_external_unmap_rejects_access},
    {"external_unmap_does_not_free_backing", "lease", EXP_VALUE, 1, 0,
     case_external_unmap_does_not_free_backing},
    {"external_mapping_does_not_double_charge", "lease", EXP_VALUE, 1, 0,
     case_external_mapping_does_not_double_charge},
    {"external_cross_space_reject", "lease", EXP_VALUE, 1, 0,
     case_external_cross_space_reject},
    /* lease：capacity 与可访问窗口是两件事（窗口更新不 remove/add） */
    {"owned_accessible_grow", "lease", EXP_VALUE, 1, 0, case_owned_accessible_grow},
    {"owned_accessible_shrink_rejects_tail", "lease", EXP_VALUE, 1, 0,
     case_owned_accessible_shrink_rejects_tail},
    {"owned_accessible_over_capacity_reject", "lease", EXP_VALUE, 1, 0,
     case_owned_accessible_over_capacity_reject},
    {"owned_accessible_failure_unchanged", "lease", EXP_VALUE, 1, 0,
     case_owned_accessible_failure_unchanged},
    /* activation：alloca 属于 procedure activation（结构化区域退出不结束它） */
    {"activation_if_survives", "activation", EXP_VALUE, 42, 0,
     case_activation_if_survives},
    {"activation_loop_continue", "activation", EXP_VALUE, 11, 0,
     case_activation_loop_continue},
    {"activation_callee_return", "activation", EXP_TRAP, 0, 1004,
     case_activation_callee_return},
    {"activation_root_done", "activation", EXP_TRAP, 0, 1004,
     case_activation_root_done},
    {"lifetime_escape", "lifetime", EXP_TRAP, 0, 1004, case_lifetime_escape},
    {"lifetime_reuse", "lifetime", EXP_VALUE, 9, 0, case_lifetime_reuse},
    {"space_switch", "lifetime", EXP_VALUE, 1, 0, case_space_switch},
    /* lea（D3 已定：构造不查、访问查、按地址宽度取模） */
    {"lea_construct_only", "lea", EXP_VALUE, 1000000, 0, case_lea_construct_only},
    {"lea_construct_then_access", "lea", EXP_VALUE, 1000000, 0,
     case_lea_construct_then_access},
    {"lea_one_past_end", "lea", EXP_VALUE, 1, 0, case_lea_one_past_end},
    {"lea_one_past_access", "lea", EXP_TRAP, 0, 1004,
     case_lea_one_past_access},
    {"lea_wraparound", "lea", EXP_VALUE, 0, 0, case_lea_wraparound},
    {"lea_wrap_lands_authorized", "lea", EXP_VALUE, 42, 0,
     case_lea_wrap_lands_authorized},
    /* budget */
    /* quota（D5：单位字节、扣在真正承诺存储处、归还只在真正释放时） */
    {"quota_exact_fit", "quota", EXP_VALUE, 1, 0, case_quota_exact_fit},
    {"quota_one_byte_over", "quota", EXP_VALUE, 1, 0,
     case_quota_one_byte_over},
    {"quota_tcb_lifecycle", "quota", EXP_VALUE, 1, 0,
     case_quota_tcb_lifecycle},
    {"quota_children_share", "quota", EXP_VALUE, 1, 0,
     case_quota_children_share},
    {"quota_failed_alloc_no_residue", "quota", EXP_VALUE, 1, 0,
     case_quota_failed_alloc_no_residue},
    {"quota_fuel_independent", "quota", EXP_VALUE, 1, 0,
     case_quota_fuel_independent},
    {"quota_host_scratch_charged", "quota", EXP_VALUE, 1, 0,
     case_quota_host_scratch_charged},

    /* capability（最小内存能力模型；模型层，不是 VM 的 load/store 通路）
     * ↑ 组名已改为 checked-ref：它是 Meta `ref(T)` 的候选 lowering，
     *   **不是** `#addr` 的规范（见 §五 与 docs/spec/vm.md）。 */
    {"cap_live_rw", "checked-ref", EXP_VALUE, 1, 0, case_cap_live_rw},
    {"cap_bounds", "checked-ref", EXP_TRAP, 0, 9210, case_cap_bounds},
    {"cap_rights", "checked-ref", EXP_TRAP, 0, 9209, case_cap_rights},
    {"cap_revoke", "checked-ref", EXP_TRAP, 0, 9207, case_cap_revoke},
    {"cap_same_address_reuse", "checked-ref", EXP_VALUE, 1, 0,
     case_cap_same_address_reuse},
    {"cap_copy_revoke", "checked-ref", EXP_TRAP, 0, 9207, case_cap_copy_revoke},
    {"cap_memory_roundtrip", "checked-ref", EXP_VALUE, 1, 0,
     case_cap_memory_roundtrip},
    {"cap_forged_reference", "checked-ref", EXP_TRAP, 0, 9206,
     case_cap_forged_reference},
    {"cap_int_roundtrip", "checked-ref", EXP_VALUE, 1, 0,
     case_cap_int_roundtrip},
    {"cap_int_after_revoke", "checked-ref", EXP_TRAP, 0, 9208,
     case_cap_int_after_revoke},
    {"cap_context_reuse", "checked-ref", EXP_TRAP, 0, 9208,
     case_cap_context_reuse},
    {"cap_child_borrow", "checked-ref", EXP_VALUE, 1, 0, case_cap_child_borrow},
    {"cap_space_switch", "checked-ref", EXP_TRAP, 0, 9212,
     case_cap_space_switch},
    {"cap_host_access", "checked-ref", EXP_TRAP, 0, 9207, case_cap_host_access},
    {"cap_failure_cleanup", "checked-ref", EXP_VALUE, 1, 0,
     case_cap_failure_cleanup},
    /* §7 要求同时可推进的两件 */
    {"cap_quota_two_actions", "checked-ref", EXP_VALUE, 1, 0,
     case_cap_quota_two_actions},
    {"cap_tcb_destroy_after_switch", "checked-ref", EXP_VALUE, 1, 0,
     case_cap_tcb_destroy_after_switch},
    /* §4.1 第二个原型（旁表跟踪地址）的探针：PASS = 缺口按预期复现 */
    {"cap_protoB_stale_address_probe", "checked-ref", EXP_VALUE, 1, 0,
     case_cap_protoB_stale_address_probe},
    /* `#addr` 的语义案：单字裸地址、int2ptr 不授予权限、alloca 出来的当场能用 */
    {"addr_slot_roundtrip", "addr", EXP_VALUE, 51, 0, case_addr_slot_roundtrip},
    {"alloca_rw_works", "addr", EXP_VALUE, 7, 0, case_alloca_rw_works},
    {"int2ptr_no_grant", "addr", EXP_TRAP, 0, 1004, case_int2ptr_no_grant},
};

static const size_t k_case_count = sizeof(k_cases) / sizeof(k_cases[0]);

/* --- 判定与打印 ------------------------------------------------------------- */
static void spec_of_expect(const Case *c, char *out, size_t cap) {
  switch (c->expect) {
    case EXP_VALUE:
      snprintf(out, cap, "value:%llu", (unsigned long long)c->expect_value);
      break;
    case EXP_TRAP:
      if (c->expect_code == 0)
        snprintf(out, cap, "trap:any");
      else
        snprintf(out, cap, "trap:%d", (int)c->expect_code);
      break;
    default:
      snprintf(out, cap, "blocked");
      break;
  }
}

static void spec_of_obs(char *out, size_t cap) {
  switch (g_obs.kind) {
    case 0:
      snprintf(out, cap, "value:%llu", (unsigned long long)g_obs.value);
      break;
    case 1:
      snprintf(out, cap, "trap:%d", (int)g_obs.trap_code);
      break;
    default:
      snprintf(out, cap, "undecided");
      break;
  }
}

/* 返回 0 = PASS，1 = FAIL，2 = BLOCKED。 */
static int verdict(const Case *c) {
  if (c->expect == EXP_BLOCKED) return 2;
  if (c->run == NULL) return 2; /* 未实现：BLOCKED，不是 PASS */
  if (c->expect == EXP_VALUE) {
    return (g_obs.kind == 0 && g_obs.value == c->expect_value) ? 0 : 1;
  }
  if (g_obs.kind != 1) return 1;
  if (c->expect_code == 0) return 0; /* 只要求「拒」 */
  return g_obs.trap_code == c->expect_code ? 0 : 1;
}

static const char *verdict_name(int v) {
  if (v == 0) return "PASS";
  if (v == 1) return "FAIL";
  return "BLOCKED";
}

static int run_one(const Case *c, int overridden) {
  char expect_spec[64];
  char obs_spec[64];
  int v;

  memset(&g_obs, 0, sizeof(g_obs));
  g_obs.detail[0] = '\0';
  if (c->run != NULL) (void)c->run();
  if (c->expect == EXP_BLOCKED && c->run == NULL && g_obs.detail[0] == '\0') {
    snprintf(g_obs.detail, sizeof(g_obs.detail), "未实施");
  }
  v = verdict(c);
  if (overridden) {
    snprintf(expect_spec, sizeof(expect_spec), "override");
  } else {
    spec_of_expect(c, expect_spec, sizeof(expect_spec));
  }
  spec_of_obs(obs_spec, sizeof(obs_spec));
  printf("CASE %s group=%s expect=%s actual=%s result=%s", c->id, c->group,
         expect_spec, obs_spec, verdict_name(v));
  if (g_obs.detail[0] != '\0') printf(" detail=%s", g_obs.detail);
  printf("\n");
  return v;
}

/* --- 命令行 ----------------------------------------------------------------- */
static void usage(void) {
  printf("usage: vspace_checks [--list] [--group G] [--case ID]\n"
         "                     [--expect-value N | --expect-trap N]\n"
         "  --expect-* 只能和 --case 一起用（负对照）。\n");
}

int main(int argc, char **argv) {
  const char *group = NULL;
  const char *only = NULL;
  int list = 0;
  int has_override = 0;
  uint64_t override_value = 0;
  size_t i;
  int pass = 0, fail = 0, blocked = 0, total = 0;

  setvbuf(stdout, NULL, _IONBF, 0);

  for (i = 1; i < (size_t)argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--list") == 0) {
      list = 1;
    } else if (strcmp(a, "--group") == 0 && i + 1 < (size_t)argc) {
      group = argv[++i];
    } else if (strcmp(a, "--case") == 0 && i + 1 < (size_t)argc) {
      only = argv[++i];
    } else if (strcmp(a, "--expect-value") == 0 && i + 1 < (size_t)argc) {
      has_override = 1;
      override_value = strtoull(argv[++i], NULL, 0);
    } else if (strcmp(a, "--expect-trap") == 0 && i + 1 < (size_t)argc) {
      has_override = 1;
      override_value = strtoull(argv[++i], NULL, 0);
    } else {
      usage();
      return 2;
    }
  }
  if (has_override && only == NULL) {
    printf("--expect-value / --expect-trap 只能和 --case 一起用\n");
    return 2;
  }
  if (list) {
    for (i = 0; i < k_case_count; i++)
      printf("CASE %s %s\n", k_cases[i].id, k_cases[i].group);
    return 0;
  }

  for (i = 0; i < k_case_count; i++) {
    const Case *c = &k_cases[i];
    Case adjusted;
    if (only != NULL && strcmp(c->id, only) != 0) continue;
    if (group != NULL && strcmp(c->group, group) != 0) continue;
    adjusted = *c;
    if (has_override) {
      /* 负对照：故意把期望改错，用来证明断言真的在生效。 */
      if (adjusted.expect == EXP_VALUE) {
        adjusted.expect_value = override_value;
      } else if (adjusted.expect == EXP_TRAP) {
        adjusted.expect_code = (int32_t)override_value;
      }
      total++;
      if (run_one(&adjusted, 1) == 0) pass++; else fail++;
      continue;
    }
    total++;
    switch (run_one(c, 0)) {
      case 0: pass++; break;
      case 1: fail++; break;
      default: blocked++; break;
    }
  }

  if (total == 0) {
    printf("没有匹配的用例（group=%s case=%s）\n", group ? group : "-",
           only ? only : "-");
    return 2;
  }
  printf("SUMMARY total=%d pass=%d fail=%d blocked=%d\n", total, pass, fail,
         blocked);
  return (fail == 0 && blocked == 0) ? 0 : 1;
}
