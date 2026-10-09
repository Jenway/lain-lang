/* Meta 驱动：把 bootstrap 拼成的编译单元跑起来，lower 一份源码。
 *
 * 它只做「装配 + 推进 + 记账」，不含语言知识：编译单元来自 SOURCE_ORDER 清单，
 * 输入是 .lain 源码，产出是 Meta 写进宿主输出缓冲的 canonical LAINIR 文本。
 *
 * 用法（在仓库根目录跑）：
 *
 *   lain-meta [选项] <逻辑路径> <源文件> [<逻辑路径> <源文件> ...]
 *
 * 逻辑路径是 import 解析用的注册表（`import("std::prelude")` 规范化成
 * `std/prelude.lain` 之后与它逐字节比较），源文件是实际字节所在。
 *
 * 选项：
 *   --unit <文件>     编译单元清单，`#` 开头为注释（默认 bootstrap/SOURCE_ORDER）
 *   --entry <名字>    Meta 入口（默认 lain_std_lower）
 *   --lower <n>       交给入口的源码下标（默认 0）
 *   --out <文件>      把产物写成文件；不给则打到 stdout
 *   --fuel <n>        引擎燃料上限（默认 40000000）
 *   --slice <n>       每次 engine_run 的燃料片（默认 2000000）
 *   --stack <字节>    栈区大小（默认 4194304）
 *   --quota <字节>    宿主分配账户（默认 268435456；0 = 不限额）
 *   --caps <n>        能力表容量（默认 64，登记 45 项）
 *   --depth <n>       最大调用深度（默认 64）
 *   --expect-status <n>  要求 Meta 报回的状态码等于 n，否则退出码 1
 *   --max-output <n>  不写文件时最多打印多少字节产物（默认 4096）
 *   -q, --quiet       只打摘要，不打产物
 *   -v, --verbose     多打逐份源码的段号与映射
 *
 * 退出码：0 = 跑到底（或 `--expect-status` 满足）；1 = 编译/执行失败；2 = 用法或 IO。
 *
 * 装配顺序是有约束的，见 seed/include/lain/meta/host.h：
 *   配额 → enable_ast_out → add_source → register → attach_space → 映射 → admit。
 * 源码文本、逻辑路径、AstIn、AstOut、暂存区都以裸地址交给 Meta，驱动必须逐块显式
 * 授权；字符串的可读长度含结尾 NUL。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lain/ir/build.h"
#include "lain/ir/value.h"
#include "lain/ir/verify.h"
#include "lain/meta/apply.h"
#include "lain/meta/expand.h"
#include "lain/meta/host.h"
#include "lain/text/parse.h"
#include "lain/vm/caps.h"
#include "lain/vm/engine.h"
#include "lain/vm/quota.h"
#include "lain/vm/space.h"
#include "lain/vm/tcb.h"
#include "lain/vm/trap.h"

#define DRIVER_MAX_SOURCES 32

typedef struct {
  char *path;  /* 逻辑路径，也是登记用的字符串 */
  char *text;  /* 源码字节，活到运行结束 */
  uint32_t length;
} SourceFile;

static char *read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  char *buf;
  long end;
  size_t n;
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  end = ftell(f);
  if (end < 0) { fclose(f); return NULL; }
  n = (size_t)end;
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
  buf = (char *)malloc(n + 1);
  if (!buf) { fclose(f); return NULL; }
  if (n && fread(buf, 1, n, f) != n) { fclose(f); free(buf); return NULL; }
  fclose(f);
  buf[n] = 0;
  if (out_len) *out_len = n;
  return buf;
}

static char *dup_cstr(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = (char *)malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

static void usage(FILE *out) {
  fputs("用法: lain-meta [选项] <逻辑路径> <源文件> [...]\n"
        "选项: --unit --entry --lower --out --fuel --slice --stack --quota\n"
        "      --caps --depth --expect-status --max-output -q/--quiet -v/--verbose\n",
        out);
}

static int map_region(LainVmSpace *space, const void *base, uint64_t bytes,
                      uint64_t accessible, uint32_t rights, const char *what) {
  if (!base || !bytes) {
    printf("map %s: skipped (empty)\n", what);
    return 0;
  }
  if (lainvm_space_handle_none(lainvm_space_map_external(
          space, (uintptr_t)base, bytes, accessible, rights, 0))) {
    printf("map %s failed (%llu bytes)\n", what, (unsigned long long)bytes);
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  const char *unit_path = "bootstrap/SOURCE_ORDER";
  const char *entry = "lain_std_lower";
  const char *out_path = NULL;
  uint64_t lower_index = 0;
  uint64_t fuel = 40000000;
  uint64_t slice_fuel = 2000000;
  uint64_t stack_bytes = 4u << 20;
  uint64_t quota_bytes = 256u << 20;
  uint64_t max_output = 4096;
  uint32_t caps_capacity = 64;
  uint32_t max_call_depth = 64;
  int quiet = 0, verbose = 0, expect_status = -1, i;
  SourceFile sources[DRIVER_MAX_SOURCES];
  uint32_t source_count = 0;

  L1Builder *builder = NULL;
  const L1Module *module = NULL;
  L1Diagnostic diag;
  LainMetaHost *host = NULL;
  LainVmCaps caps;
  LainVmCapEntry *entries = NULL;
  LainExpandLimits expand_limits;
  LainMetaApplyLimits apply_limits;
  LainVmSpace space;
  LainVmQuota quota;
  LainVmImage *image = NULL;
  LainVmTcb *tcb = NULL;
  LainVmRegionHandle sh;
  LainVmStackLease lease;
  L1Value arg;
  LainVmSliceResult slice = LAINVM_SLICE_DONE;
  char *unit_text = NULL;
  uint64_t fuel_used = 0;
  unsigned rounds = 0;
  int rc = 0;

  memset(sources, 0, sizeof(sources));

  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) { quiet = 1; continue; }
    if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) { verbose = 1; continue; }
    if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
#define TAKE(name)                                                            \
    do {                                                                      \
      if (i + 1 >= argc) { printf("%s 缺少实参\n", name); return 2; }         \
      i++;                                                                    \
    } while (0)
    if (!strcmp(a, "--unit")) { TAKE(a); unit_path = argv[i]; continue; }
    if (!strcmp(a, "--entry")) { TAKE(a); entry = argv[i]; continue; }
    if (!strcmp(a, "--out")) { TAKE(a); out_path = argv[i]; continue; }
    if (!strcmp(a, "--lower")) { TAKE(a); lower_index = strtoull(argv[i], NULL, 0); continue; }
    if (!strcmp(a, "--fuel")) { TAKE(a); fuel = strtoull(argv[i], NULL, 0); continue; }
    if (!strcmp(a, "--slice")) { TAKE(a); slice_fuel = strtoull(argv[i], NULL, 0); continue; }
    if (!strcmp(a, "--stack")) { TAKE(a); stack_bytes = strtoull(argv[i], NULL, 0); continue; }
    if (!strcmp(a, "--quota")) { TAKE(a); quota_bytes = strtoull(argv[i], NULL, 0); continue; }
    if (!strcmp(a, "--caps")) { TAKE(a); caps_capacity = (uint32_t)strtoul(argv[i], NULL, 0); continue; }
    if (!strcmp(a, "--depth")) { TAKE(a); max_call_depth = (uint32_t)strtoul(argv[i], NULL, 0); continue; }
    if (!strcmp(a, "--max-output")) { TAKE(a); max_output = strtoull(argv[i], NULL, 0); continue; }
    if (!strcmp(a, "--expect-status")) { TAKE(a); expect_status = (int)strtol(argv[i], NULL, 0); continue; }
#undef TAKE
    if (a[0] == '-' && a[1]) { printf("未知选项 %s\n", a); usage(stderr); return 2; }
    /* 位置参数成对出现：逻辑路径、源文件。 */
    if (i + 1 >= argc) { printf("%s 缺少源文件\n", a); usage(stderr); return 2; }
    if (source_count >= DRIVER_MAX_SOURCES) { printf("源码太多\n"); return 2; }
    sources[source_count].path = dup_cstr(a);
    {
      size_t n = 0;
      sources[source_count].text = read_file(argv[i + 1], &n);
      if (n > 0xFFFFFFFFu) { printf("源文件过大 %s\n", argv[i + 1]); return 2; }
      sources[source_count].length = (uint32_t)n;
    }
    if (!sources[source_count].text || !sources[source_count].path) {
      printf("读不到 %s\n", argv[i + 1]);
      return 2;
    }
    if (verbose) printf("source[%u] path=%s file=%s bytes=%u\n", source_count,
                        a, argv[i + 1], sources[source_count].length);
    source_count++;
    i++;
  }
  if (source_count == 0) { usage(stderr); return 2; }
  if (lower_index >= source_count) { printf("--lower 越界\n"); return 2; }

  /* 1. 链接编译单元。 */
  {
    FILE *order = fopen(unit_path, "rb");
    char line[512];
    size_t used = 0;
    unsigned files = 0;
    if (!order) { printf("读不到 %s\n", unit_path); return 2; }
    unit_text = (char *)malloc(8u << 20);
    if (!unit_text) { printf("分配编译单元缓冲失败\n"); return 2; }
    while (fgets(line, sizeof(line), order)) {
      char path[512];
      size_t len = strlen(line), plen = 0;
      char *part;
      while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
      if (len == 0 || line[0] == '#') continue;
      snprintf(path, sizeof(path), "bootstrap/%s", line);
      part = read_file(path, &plen);
      if (!part) { printf("编译单元缺 %s\n", path); fclose(order); return 2; }
      memcpy(unit_text + used, part, plen);
      used += plen;
      unit_text[used++] = '\n';
      free(part);
      files++;
    }
    fclose(order);
    unit_text[used] = 0;
    printf("unit files=%u bytes=%lu\n", files, (unsigned long)used);
  }

  /* 2. parse + verify：编译单元本身必须过语言契约。 */
  builder = lainir_builder_new();
  if (!builder) { printf("builder_new 失败\n"); return 1; }
  memset(&diag, 0, sizeof(diag));
  module = lainir_parse(builder, unit_text, &diag);
  if (!module) {
    printf("PARSE FAILED code=%d line=%u\n", diag.code, diag.line);
    return 1;
  }
  if (lainir_verify(module, &diag) != 0) {
    printf("VERIFY FAILED code=%d line=%u\n", diag.code, diag.line);
    return 1;
  }
  printf("verify subroutines=%u data=%u\n", module->subroutine_count,
         module->data_count);

  /* 3. 装配宿主。顺序见文件头注释。 */
  host = lainmeta_host_new();
  if (!host) { printf("host_new 失败\n"); return 1; }
  entries = (LainVmCapEntry *)calloc(caps_capacity ? caps_capacity : 1,
                                     sizeof(LainVmCapEntry));
  if (!entries) { printf("分配能力表失败\n"); return 1; }
  lainvm_caps_init(&caps, entries, caps_capacity);

  memset(&expand_limits, 0, sizeof(expand_limits));
  expand_limits.max_depth = 64;
  expand_limits.max_visits = 4000000;
  expand_limits.max_handler_calls = 1000000;
  expand_limits.max_replacement_refs = 65536;
  expand_limits.ast_out_capacity = 1u << 20;
  expand_limits.ast_out_node_capacity = 8192;

  lainvm_quota_init(&quota, quota_bytes);
  lainmeta_host_attach_quota(host, &quota);
  {
    uint32_t e = lainmeta_host_enable_ast_out(host, &expand_limits);
    if (e) { printf("enable_ast_out -> %u\n", e); return 1; }
  }
  {
    uint32_t s;
    for (s = 0; s < source_count; s++) {
      int add = lainmeta_host_add_source(host, sources[s].path, sources[s].text,
                                         sources[s].length);
      if (add) { printf("add_source[%u] -> %d\n", s, add); return 1; }
    }
    printf("sources count=%u\n", source_count);
  }
  {
    int reg = lainmeta_host_register(host, &caps);
    if (reg) { printf("register -> %d (caps 容量 %u 不足或重名)\n", reg, caps_capacity); return 1; }
    if (verbose) printf("caps count=%u\n", (unsigned)caps.count);
  }

  memset(&apply_limits, 0, sizeof(apply_limits));
  apply_limits.caps = &caps;
  apply_limits.max_call_depth = max_call_depth;
  apply_limits.stack_bytes = stack_bytes;
  apply_limits.fuel = fuel;
  apply_limits.quota_bytes = 0;

  lainvm_space_init(&space);
  lainmeta_host_attach_space(host, &space);
  lainmeta_host_set_apply_limits(host, &apply_limits);

  /* 4. 逐块授权：Meta 只拿得到裸地址，能读什么由驱动说了算。 */
  {
    uint32_t s;
    for (s = 0; s < source_count; s++) {
      const char *text;
      uint32_t length = 0;
      const char *logical;
      LainAstArenaView arena;
      char what[64];
      if (verbose) printf("source[%u] segment=%u tree_segment=%u\n", s,
                          lainmeta_host_source_segment(host, s),
                          lainmeta_host_tree_segment(host, s));
      text = lainmeta_host_source_text(host, s, &length);
      snprintf(what, sizeof(what), "source[%u] text", s);
      if (map_region(&space, text, (uint64_t)length + 1, (uint64_t)length + 1,
                     LAINVM_MEM_READ, what)) return 1;
      logical = lainmeta_host_source_path(host, s);
      snprintf(what, sizeof(what), "source[%u] path", s);
      if (map_region(&space, logical, strlen(logical) + 1, strlen(logical) + 1,
                     LAINVM_MEM_READ, what)) return 1;
      if (!lainmeta_host_arena(host, s, &arena)) {
        printf("source[%u] 没有 AstIn（登记即拒）\n", s);
        return 1;
      }
      snprintf(what, sizeof(what), "source[%u] ast_in", s);
      if (map_region(&space, arena.data, arena.capacity, arena.used,
                     LAINVM_MEM_READ, what)) return 1;
    }
  }
  {
    LainAstArenaView out_view;
    if (lainmeta_host_ast_out(host, &out_view) && out_view.data) {
      if (map_region(&space, out_view.data, out_view.capacity, out_view.capacity,
                     LAINVM_MEM_READ | LAINVM_MEM_WRITE, "ast_out")) return 1;
    } else {
      printf("host_ast_out refused\n");
      return 1;
    }
  }
  {
    uint32_t scratch_size = 0;
    void *scratch = lainmeta_host_scratch(host, &scratch_size);
    if (!scratch || !scratch_size) { printf("scratch 分配失败\n"); return 1; }
    printf("scratch bytes=%u\n", scratch_size);
    if (map_region(&space, scratch, scratch_size, scratch_size,
                   LAINVM_MEM_READ | LAINVM_MEM_WRITE, "scratch")) return 1;
  }

  /* 5. admit 到镜像，起 TCB。 */
  memset(&diag, 0, sizeof(diag));
  image = lainvm_image_load(module, &space, &diag);
  if (!image) { printf("IMAGE FAILED code=%d line=%u\n", diag.code, diag.line); return 1; }
  printf("image ok\n");

  sh = lainvm_space_alloc_stack(&space, stack_bytes, 1, &quota);
  if (lainvm_space_handle_none(sh)) { printf("没有栈区\n"); return 1; }
  lease.space = &space;
  lease.region = sh;

  tcb = lainvm_tcb_new(image, &space, 1, 1, max_call_depth, lease, &quota);
  if (!tcb) { printf("tcb_new 失败\n"); return 1; }
  lainvm_caps_freeze(&caps);
  memset(&diag, 0, sizeof(diag));
  if (lainvm_tcb_set_caps(tcb, &caps, &diag) != 0) {
    printf("set_caps FAILED code=%d line=%u\n", diag.code, diag.line);
    return 1;
  }
  arg.kind = L1_VALUE_BITS;
  arg.bit_width = 64;
  arg.as.bits = lower_index;
  memset(&diag, 0, sizeof(diag));
  if (lainvm_tcb_start(tcb, entry, &arg, 1, &diag) != 0) {
    printf("start FAILED code=%d line=%u\n", diag.code, diag.line);
    return 1;
  }

  /* 6. 推进到不 RUNNABLE 或燃料耗尽。 */
  while (rounds * slice_fuel < fuel) {
    uint64_t this_slice = fuel - fuel_used;
    if (this_slice > slice_fuel) this_slice = slice_fuel;
    slice = lainvm_engine_run(tcb, this_slice);
    fuel_used += this_slice;
    rounds++;
    if (slice != LAINVM_SLICE_RUNNABLE) break;
  }
  printf("run slice=%d rounds=%u steps=%llu host_status=%u\n", (int)slice,
         rounds, (unsigned long long)lainvm_tcb_steps(tcb),
         lainmeta_host_status(host));

  {
    const LainVmTrap *trap = lainvm_tcb_trap(tcb);
    if (trap && trap->active) {
      printf("trap kind=%d status=%d line=%u column=%u region=%u position=%u\n",
             (int)trap->kind, trap->status, trap->line, trap->column,
             trap->region, trap->position);
      rc = 1;
    } else {
      printf("trap none\n");
    }
  }
  if (lainvm_tcb_has_result(tcb)) {
    L1Value r = lainvm_tcb_result(tcb);
    printf("result kind=%d width=%u bits=%llu\n", (int)r.kind, r.bit_width,
           (unsigned long long)r.as.bits);
  } else {
    printf("result none\n");
  }
  {
    LainMetaDiagnostic md;
    if (lainmeta_host_diagnostic(host, &md)) {
      printf("diagnostic code=%u source=%llu offset=%llu\n", md.code,
             (unsigned long long)md.source, (unsigned long long)md.offset);
    }
  }
  {
    uint64_t peak = 0;
    uint64_t ref = 0;
    uint32_t bad_code = 0;
    uint32_t count = 0;
    if (lainmeta_host_scratch_peak(host, &peak)) {
      printf("scratch peak=%llu\n", (unsigned long long)peak);
    }
    printf("region grants=%llu\n",
           (unsigned long long)lainmeta_host_region_grants(host));
    lainmeta_host_trace_bad(host, &ref, &bad_code, &count);
    printf("apply requests=%u deny=%u status5=%u trace_bad=%llu/%u/%u\n",
           lainmeta_host_apply_requests(host), lainmeta_host_deny_count(host),
           lainmeta_host_status5_count(host), (unsigned long long)ref,
           bad_code, count);
    printf("ast handler_calls=%llu visits=%llu\n",
           (unsigned long long)lainmeta_host_ast_handler_calls(host),
           (unsigned long long)lainmeta_host_ast_visits(host));
  }
  {
    uint32_t n = lainmeta_host_output_length(host);
    const char *out = lainmeta_host_output(host);
    printf("output bytes=%u\n", n);
    if (n && !quiet) {
      if (out_path) {
        FILE *f = fopen(out_path, "wb");
        if (!f) { printf("写不到 %s\n", out_path); return 2; }
        if (fwrite(out, 1, n, f) != n) { fclose(f); printf("写 %s 失败\n", out_path); return 2; }
        fclose(f);
        printf("output file=%s\n", out_path);
      } else {
        uint32_t show = n;
        if ((uint64_t)show > max_output) show = (uint32_t)max_output;
        printf("---- output ----\n");
        fwrite(out, 1, show, stdout);
        if (show < n) printf("\n---- 截断，共 %u 字节（--out 或 --max-output 调整）----\n", n);
        else printf("\n---- end ----\n");
      }
    } else if (n && out_path) {
      FILE *f = fopen(out_path, "wb");
      if (!f) { printf("写不到 %s\n", out_path); return 2; }
      if (fwrite(out, 1, n, f) != n) { fclose(f); printf("写 %s 失败\n", out_path); return 2; }
      fclose(f);
      printf("output file=%s\n", out_path);
    }
  }

  if (expect_status >= 0 && (int)lainmeta_host_status(host) != expect_status) {
    printf("EXPECT-STATUS FAILED 期望 %d 实得 %u\n", expect_status,
           lainmeta_host_status(host));
    rc = 1;
  }
  if (slice == LAINVM_SLICE_RUNNABLE) {
    printf("RUN INCOMPLETE 燃料耗尽\n");
    rc = 1;
  }
  return rc;
}
