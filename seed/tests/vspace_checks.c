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

static int32_t add(LainVmSpace *space, uintptr_t base, uint64_t size,
                   uint32_t rights, uint64_t owner) {
  return lainvm_space_add_region(space, base, size, rights, owner);
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
  rig->tcb = lainvm_tcb_new(rig->image, &rig->space, 1, 1, 64, stack_bytes);
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

/* 一个从来没登记过的地址上读一个字节。期望：拒绝（当前 load 码 1004）。 */
static const char *k_prog_outside_region =
    "data bytes ro { 42 }\n"
    "#proc outside() -> #bits<64> {\n"
    "  %p = #int2ptr[#addr](4096)\n"
    "  %b = #load[#bits<8>](%p)\n"
    "  %w = #zext[#bits<64>](%b)\n"
    "  #return %w\n"
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

/* 只构造区段外地址、不访问。 */
static const char *k_prog_lea_far =
    "#proc lea_far() -> #bits<64> {\n"
    "  %p = #int2ptr[#addr](4096)\n"
    "  %q = #lea(%p, 0, 1, 1000000)\n"
    "  %w = #ptr2int[#bits<64>](%q)\n"
    "  %z = #sub[#bits<64>](%w, %w)\n"
    "  #return %z\n"
    "}\n";

/* 乘法/加法回绕：idx * scale 溢出。 */
static const char *k_prog_lea_wrap =
    "#proc lea_wrap() -> #bits<64> {\n"
    "  %p = #int2ptr[#addr](4096)\n"
    "  %q = #lea(%p, 0xFFFFFFFFFFFFFFFF, 8, 0)\n"
    "  %w = #ptr2int[#bits<64>](%q)\n"
    "  #return %w\n"
    "}\n";

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

/* 从未登记过的地址读：期望拒绝（1004）。 */
static int case_load_outside_region(void) {
  Rig rig;
  int rc;

  if (rig_load(&rig, k_prog_outside_region, 1024) != 0) return 0;
  rc = rig_run(&rig, "outside");
  rig_free(&rig);
  (void)rc;
  return 0;
}

/* R01：先登记高地址，再登记低地址（低地址插到前面）—— 旧引用还指得到原对象吗？ */
static int case_region_insert_identity(void) {
  LainVmSpace space;
  uint8_t *buf = new_buffer(4096);
  uintptr_t low, high, high_base, seen;
  int32_t ref_high;

  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  low = (uintptr_t)buf;
  high = (uintptr_t)buf + 2048;
  lainvm_space_init(&space);
  ref_high = add(&space, high, 1024, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 21);
  if (ref_high < 0) {
    free(buf);
    obs_facts("登记高地址段失败");
    return 0;
  }
  high_base = space.regions[ref_high].base;
  if (add(&space, low, 1024, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 22) < 0) {
    free(buf);
    obs_facts("登记低地址段失败");
    return 0;
  }
  seen = space.regions[ref_high].base;
  free(buf);
  if (seen != high_base) {
    obs_facts("R01 复现：插入低地址后，引用 %d 从 base=%llu 变成 base=%llu",
              (int)ref_high, (unsigned long long)high_base,
              (unsigned long long)seen);
    return 0;
  }
  obs_value(1);
  return 0;
}

/* R02：三段，移除第一段，后两段的旧引用还指得到原对象吗？ */
static int case_region_remove_identity(void) {
  LainVmSpace space;
  uint8_t *buf = new_buffer(4096);
  int32_t ref_b, ref_c;
  uintptr_t base_b, seen;

  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  lainvm_space_init(&space);
  if (add(&space, (uintptr_t)buf, 1024, LAINVM_MEM_READ, 11) < 0 ||
      add(&space, (uintptr_t)buf + 1024, 1024, LAINVM_MEM_READ, 12) < 0 ||
      add(&space, (uintptr_t)buf + 2048, 1024, LAINVM_MEM_READ, 13) < 0) {
    free(buf);
    obs_facts("登记三段失败");
    return 0;
  }
  ref_b = 1;
  ref_c = 2;
  base_b = space.regions[ref_b].base;
  lainvm_space_release_owner(&space, 11); /* 移除第一段（owner=11） */
  seen = space.regions[ref_b].base;
  free(buf);
  if (seen != base_b) {
    obs_facts("R02 复现：移除第一段后，引用 %d（base=%llu）现在指向 base=%llu",
              (int)ref_b, (unsigned long long)base_b, (unsigned long long)seen);
    return 0;
  }
  (void)ref_c;
  obs_value(1);
  return 0;
}

/* owner=0 是「模块/装载器的」保留值。一次 release_owner(0) 该不该把它们清掉？ */
static int case_region_owner_zero(void) {
  LainVmSpace space;
  uint8_t *buf = new_buffer(4096);
  uint32_t before, after;

  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  lainvm_space_init(&space);
  if (add(&space, (uintptr_t)buf, 1024, LAINVM_MEM_READ, 0) < 0 ||
      add(&space, (uintptr_t)buf + 2048, 1024, LAINVM_MEM_READ, 0) < 0 ||
      add(&space, (uintptr_t)buf + 1024, 512, LAINVM_MEM_READ, 7) < 0) {
    free(buf);
    obs_facts("登记失败");
    return 0;
  }
  before = space.region_count;
  lainvm_space_release_owner(&space, 0);
  after = space.region_count;
  free(buf);
  if (after != before - 1) {
    obs_facts("owner=0 被当成普通 owner：一次 release_owner(0) 把 %u 段里的 %u 段删了"
              "（只剩 %u；按契约只该删 owner=7 那一段）",
              (unsigned)before, (unsigned)(before - after), (unsigned)after);
    return 0;
  }
  obs_value(1);
  return 0;
}

/* 表满：64 段能登记，第 65 段必须被拒且前 64 段不变。 */
static int case_region_full_64(void) {
  LainVmSpace space;
  uint8_t *buf = new_buffer(64 * 64 + 64);
  int32_t idx;
  uint32_t i;
  uintptr_t first_base, last_base;

  if (!buf) {
    obs_facts("malloc 失败");
    return 0;
  }
  lainvm_space_init(&space);
  for (i = 0; i < 64; i++) {
    idx = add(&space, (uintptr_t)buf + i * 64, 64, LAINVM_MEM_READ, 1);
    if (idx < 0) {
      obs_facts("第 %u 段就登记失败了（应当能到 64）", (unsigned)(i + 1));
      free(buf);
      return 0;
    }
  }
  first_base = space.regions[0].base;
  last_base = space.regions[63].base;
  idx = add(&space, (uintptr_t)buf + 64 * 64, 64, LAINVM_MEM_READ, 1);
  if (idx >= 0) {
    obs_facts("第 65 段被接受了（region_count=%u）", (unsigned)space.region_count);
    free(buf);
    return 0;
  }
  if (space.region_count != 64 || space.regions[0].base != first_base ||
      space.regions[63].base != last_base) {
    obs_facts("第 65 段被拒，但前 64 段被改动了（count=%u）",
              (unsigned)space.region_count);
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

/* --- stack 组 --------------------------------------------------------------- */

/* R03：TCB 缓存的栈区段引用，在表被插入后还指得到它自己的栈吗？ */
static int case_stack_lease_identity(void) {
  Rig rig;
  uintptr_t stack_base, seen, restored;
  int32_t ref;
  int rc;

  if (rig_load(&rig, k_prog_alloca, 4096) != 0) return 0;
  ref = rig.tcb->stack_region;
  if (ref < 0) {
    rig_free(&rig);
    obs_facts("TCB 没有栈区段（stack_bytes=4096 却拿到 NO_REGION）");
    return 0;
  }
  stack_base = rig.space.regions[ref].base;
  /* 在这块栈的**正下方**登记一段合成区段：base 更低 → 有序插入会把它放到前面。
     只登记、不解引用，所以不碰任何未映射内存。 */
  rc = add(&rig.space, stack_base - 4096, 4096, LAINVM_MEM_READ, 99);
  if (rc < 0) {
    rig_free(&rig);
    obs_facts("在栈下方登记合成区段失败（base=%llu）",
              (unsigned long long)stack_base);
    return 0;
  }
  seen = rig.space.regions[ref].base;
  if (seen != stack_base) {
    /* 复现了。销毁之前必须把这次插入**撤回去**：lainvm_tcb_free 会按那个已经错位
       的缓存下标取地址去 free（tcb.c:83-85），实测直接把进程打成堆损坏
       （0xC0000374）。方案 §3.2 要求这类复现「检查出下标已指错对象后安全退出」，
       所以这里先还原表、再销毁。 */
    lainvm_space_release_owner(&rig.space, 99);
    restored = (ref < (int32_t)rig.space.region_count)
                   ? rig.space.regions[ref].base
                   : (uintptr_t)0;
    rig_free(&rig);
    obs_facts("R03 复现：栈区段引用 %d 从 base=%llu 变成 base=%llu（指到别人）；"
              "撤销插入后恢复成 base=%llu",
              (int)ref, (unsigned long long)stack_base, (unsigned long long)seen,
              (unsigned long long)restored);
    return 0;
  }
  rig_free(&rig);
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

/* --- host 组 ---------------------------------------------------------------- */

/* R04：从合法区段的末尾读，长度超出一字节 —— 能力该不该拦？ */
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
  args[0] = (uint64_t)(uintptr_t)host;
  args[1] = (uint64_t)(uintptr_t)(buf + 15); /* 区段末尾的最后一个字节 */
  args[2] = 2;                               /* 读 2 字节：超出一字节 */
  status = fn(args, 3, &result);
  out_len = lainmeta_host_output_length(host);
  if (status == 0 && out_len == 2) {
    obs_facts("R04 复现：宿主按裸地址读了区段外的字节（status=0，输出长度=%u）",
              (unsigned)out_len);
  } else if (status != 0) {
    obs_trap(0, (int32_t)status);
  } else {
    obs_facts("status=0 但输出长度=%u（预期 2 才是复现）", (unsigned)out_len);
  }

done:
  if (caps) lainvm_caps_free(caps);
  if (host) lainmeta_host_free(host);
  free(buf);
  return 0;
}

/* R05：把 host 参数换成另一个同样可读写的 host 对象 —— 该不该拒？ */
static int case_host_wrong_identity(void) {
  LainMetaHost *real = NULL;
  LainMetaHost *decoy = NULL;
  LainVmCaps *caps = NULL;
  const LainVmCapEntry *entry;
  LainVmHostFn fn;
  uint8_t *buf;
  uint64_t args[3];
  uint64_t result = 0;
  uint32_t status;
  uint32_t decoy_out;

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
  args[0] = (uint64_t)(uintptr_t)decoy; /* ← 不是注册时绑定的那个 host */
  args[1] = (uint64_t)(uintptr_t)buf;
  args[2] = 4;
  status = fn(args, 3, &result);
  decoy_out = lainmeta_host_output_length(decoy);
  if (status == 0 && decoy_out == 4) {
    obs_facts("R05 复现：换了个 host 对象照样被接受，还写进了它的输出（长度=%u）",
              (unsigned)decoy_out);
  } else if (status != 0) {
    obs_trap(0, (int32_t)status);
  } else {
    obs_facts("status=0 但 decoy 输出长度=%u（预期 4 才是复现）",
              (unsigned)decoy_out);
  }

done:
  if (caps) lainvm_caps_free(caps);
  if (real) lainmeta_host_free(real);
  if (decoy) lainmeta_host_free(decoy);
  free(buf);
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

/* R07：同一地址会不会被发两次？返回 1 = 会（旧引用不可区分）。 */
static int case_lifetime_reuse(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_reuse, 4096) != 0) return 0;
  (void)rig_run(&rig, "reuse");
  rig_free(&rig);
  return 0;
}

/* --- lea 组 ---------------------------------------------------------------- */

/* 只构造区外地址、不访问：D3 未定，只记录。 */
static int case_lea_construct_only(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_lea_far, 1024) != 0) return 0;
  (void)rig_run(&rig, "lea_far");
  if (g_obs.kind == 0) {
    obs_facts("构造区段外地址（+1000000）没有被拒，过程返回 %llu —— 是否允许要 D3 定",
              (unsigned long long)g_obs.value);
  }
  rig_free(&rig);
  return 0;
}

/* idx * scale 回绕：D3 未定，只记录。 */
static int case_lea_wraparound(void) {
  Rig rig;

  if (rig_load(&rig, k_prog_lea_wrap, 1024) != 0) return 0;
  (void)rig_run(&rig, "lea_wrap");
  if (g_obs.kind == 0) {
    obs_facts("#lea(idx=0xFFFFFFFFFFFFFFFF, scale=8) 回绕成 %llu，没有被拒 —— 要 D3 定",
              (unsigned long long)g_obs.value);
  }
  rig_free(&rig);
  return 0;
}

/* --- budget 组 -------------------------------------------------------------- */

/* 配额：规范 §4/:105、§6/:162、§8.2/:212-214 要求；实现里没有任何账。 */
static int case_budget_unimplemented(void) {
  obs_facts("规范要求 TCB 有 allocation quota 的预算与消耗（04-lain-vm.md:105/:162/"
            ":212-214），实现里 grep quota|budget 在 seed/src/vm/** 与 "
            "seed/include/lainvm/** 零命中；单位与扣费点等 D5 定");
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
    {"region_owner_zero", "region", EXP_VALUE, 1, 0, case_region_owner_zero},
    {"region_full_64", "region", EXP_VALUE, 1, 0, case_region_full_64},
    {"region_overlap_reject", "region", EXP_VALUE, 1, 0,
     case_region_overlap_reject},
    {"region_adjacent_allow", "region", EXP_VALUE, 1, 0,
     case_region_adjacent_allow},
    {"region_bad_size_reject", "region", EXP_VALUE, 1, 0,
     case_region_bad_size_reject},
    {"region_range_tail", "region", EXP_VALUE, 1, 0, case_region_range_tail},
    /* stack */
    {"stack_lease_identity", "stack", EXP_VALUE, 1, 0,
     case_stack_lease_identity},
    {"stack_absent_trap", "stack", EXP_TRAP, 0, 1006, case_stack_absent_trap},
    {"stack_exhaust_watermark", "stack", EXP_TRAP, 0, 1007,
     case_stack_exhaust_watermark},
    {"stack_count_overflow", "stack", EXP_TRAP, 0, 0, case_stack_count_overflow},
    {"stack_zero_count", "stack", EXP_TRAP, 0, 2024, case_stack_zero_count},
    /* host */
    {"host_past_region", "host", EXP_TRAP, 0, 0, case_host_past_region},
    {"host_wrong_identity", "host", EXP_TRAP, 0, 0, case_host_wrong_identity},
    {"host_source_index_bounds", "host", EXP_BLOCKED, 0, 0, NULL},
    /* lifetime */
    {"lifetime_escape", "lifetime", EXP_TRAP, 0, 0, case_lifetime_escape},
    {"lifetime_reuse", "lifetime", EXP_VALUE, 0, 0, case_lifetime_reuse},
    /* lea */
    {"lea_construct_only", "lea", EXP_BLOCKED, 0, 0, case_lea_construct_only},
    {"lea_wraparound", "lea", EXP_BLOCKED, 0, 0, case_lea_wraparound},
    /* budget */
    {"budget_unimplemented", "budget", EXP_BLOCKED, 0, 0,
     case_budget_unimplemented},
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
