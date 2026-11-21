/* Lain-Meta v2 的驱动：把一份 .l1 单元装进 VM，把 v2 宿主挂在能力表上跑。
 *
 * 装配顺序（每一步都依赖前一步）：
 *   1. 读 .l1，解析并验证；
 *   2. lainvm_space_init，再建宿主并挂上同一个 space 与 quota；
 *   3. 符号池、AstOut 区、工作区（都在 space 里，guest 只拿到裸地址）；
 *   4. 逐份登记源码（Host 解析 RawAst 并驻留符号）；
 *   5. 登记能力、冻结、装载映像、给栈、起 TCB；
 *   6. 推进到不 RUNNABLE 或燃料耗尽。
 *
 * 用法：lain-meta-v2 <unit.l1> [<unit.l1> ...] [源码.lain ...] [选项]
 *   --entry <名>      入口过程（默认 lain_meta_v2_entry）
 *   --source <路径>   追加一份源码（与位置参数等价）
 *   --out <路径>      把宿主输出写进文件（默认打到标准输出）
 *   --fuel <n>        总燃料（默认 8000000）
 *   --slice <n>       单次推进步数上限（默认 4000000）
 *   --stack <n>       栈字节（默认 4 MiB）
 *   --quota <n>       配额字节（默认 2 GiB）
 *   --workspace <n>   工作区字节（默认 384 MiB）
 *   --caps <n>        能力表容量（默认 128）
 *   --depth <n>       最大调用深度（默认 1024）
 *   --symbols <n>     符号容量（默认 131072）
 *   --symbol-bytes<n> 符号字节池（默认 8 MiB）
 *   -q 安静；-v 打印装配细节
 *
 * 退出码：0 成功、1 装配失败、3 Trap、4 没有结果、5 燃料耗尽。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lain/ir/build.h"
#include "lain/ir/verify.h"
#include "lain/meta/tree.h"
#include "lain/meta/host_v2.h"
#include "lain/vm/caps.h"
#include "lain/vm/engine.h"
#include "lain/vm/quota.h"
#include "lain/vm/space.h"
#include "lain/vm/tcb.h"
#include "lain/text/parse.h"

#define DRIVER_MAX_SOURCES 128
#define DRIVER_MAX_UNITS 128

static int g_verbose = 0;
static int g_quiet = 0;

static void usage(void) {
  printf("lain-meta-v2 <unit.l1> [<unit.l1> ...] [源码.lain ...] [--entry 名] [--out 路径]\n");
  printf("             [--fuel n] [--slice n] [--stack n] [--quota n]\n");
  printf("             [--workspace n] [--caps n] [--depth n]\n");
  printf("             [--symbols n] [--symbol-bytes n] [-q] [-v]\n");
}

static char *read_text(const char *path, uint32_t *length_out) {
  FILE *file = fopen(path, "rb");
  long size;
  char *buffer;
  if (!file) {
    printf("打不开 %s\n", path);
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
  size = ftell(file);
  if (size < 0) { fclose(file); return NULL; }
  if (fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
  buffer = (char *)malloc((size_t)size + 1);
  if (!buffer) { fclose(file); return NULL; }
  if (size && fread(buffer, 1, (size_t)size, file) != (size_t)size) {
    free(buffer);
    fclose(file);
    return NULL;
  }
  buffer[size] = 0;
  fclose(file);
  if (length_out) *length_out = (uint32_t)size;
  return buffer;
}

/* 一份程序可能被 lain0 --split 拆成多份单元：按 lainir-verify 的同一规则拼成一个模块。 */
static char *join_units(const char *const *paths, uint32_t count,
                        uint32_t *length_out) {
  uint32_t index;
  size_t total = 0, at = 0;
  char *joined;
  for (index = 0; index < count; index++) {
    uint32_t length = 0;
    char *body = read_text(paths[index], &length);
    if (!body) return NULL;
    total += (size_t)length + 1;
    free(body);
  }
  joined = (char *)malloc(total + 1);
  if (!joined) return NULL;
  for (index = 0; index < count; index++) {
    uint32_t length = 0;
    char *body = read_text(paths[index], &length);
    if (!body) { free(joined); return NULL; }
    memcpy(joined + at, body, length);
    at += length;
    joined[at++] = '\n';
    free(body);
  }
  joined[at] = 0;
  if (length_out) *length_out = (uint32_t)at;
  return joined;
}

static int parse_u64(const char *text, uint64_t *out) {
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (!end || *end) return 0;
  *out = (uint64_t)value;
  return 1;
}

static void print_diags(LainMetaHostV2 *host) {
  uint32_t d, diags = lainv2_host_diag_count(host);
  for (d = 0; d < diags; d++) {
    LainMetaV2Diag item;
    if (!lainv2_host_diag_at(host, d, &item)) continue;
    printf("diag level=%u source=%d offset=%llu %s\n", item.level,
           (int)item.source, (unsigned long long)item.offset, item.message);
  }
}

int main(int argc, char **argv) {
  const char *unit_paths[DRIVER_MAX_UNITS];
  uint32_t unit_count = 0;
  const char *out_path = NULL;
  const char *entry = "lain_meta_v2_entry";
  const char *source_paths[DRIVER_MAX_SOURCES];
  char *source_text[DRIVER_MAX_SOURCES] = {0};
  uint32_t source_count = 0;
  /* 默认值按 scripts/verify_all.py 全量用例的实测峰值定在 4~8 倍：
     步数峰值 1268250、工作区水位峰值 89298752、配额峰值（工作区整块入账后）
     约 421700000。工作区是唯一预先切块的项，配额与步数只是上限，不预占内存。 */
  uint64_t fuel = 8000000ull, slice_fuel = 4000000ull;
  uint64_t stack_bytes = 4ull << 20;
  uint64_t quota_bytes = 2147483648ull;
  uint64_t workspace_bytes = 402653184ull;
  uint64_t caps_capacity = 128;
  /* 编译期的递归下降（表达式、注入生成器）都要落成普通的 #call 帧，
     32 层连带的区域帧不够用；这一层上限只防失控递归。 */
  uint64_t max_call_depth = 1024;
  uint64_t symbol_capacity = 131072;
  uint64_t symbol_bytes = 8ull << 20;
  LainExpandLimits expand_limits;
  LainMetaApplyLimits apply_limits;
  LainVmQuota quota;
  LainVmSpace space;
  LainVmCaps caps;
  LainVmCapEntry *entries = NULL;
  LainVmStackLease lease;
  LainVmRegionHandle stack_handle;
  LainMetaHostV2 *host = NULL;
  L1Builder *builder = NULL;
  const L1Module *module = NULL;
  LainVmImage *image = NULL;
  LainVmTcb *tcb = NULL;
  L1Diagnostic diag;
  L1Value arg;
  char *unit_text = NULL;
  uint32_t unit_length = 0;
  uint64_t fuel_used = 0;
  int i, exit_code = 1;

  setbuf(stdout, NULL);
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "-q") == 0) { g_quiet = 1; continue; }
    if (strcmp(a, "-v") == 0) { g_verbose = 1; continue; }
    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(); return 0; }
    /* 位置参数先处理：不带前缀的词是文件名，不能落进下面的缺实参检查。 */
    if (a[0] != '-') {
      size_t len = strlen(a);
      if (len > 3 && strcmp(a + len - 3, ".l1") == 0) {
        if (unit_count == DRIVER_MAX_UNITS) { printf("单元份数超限\n"); return 2; }
        unit_paths[unit_count++] = a;
        continue;
      }
      if (source_count == DRIVER_MAX_SOURCES) { printf("源码份数超限\n"); return 2; }
      source_paths[source_count++] = a;
      continue;
    }
    if (i + 1 >= argc) { printf("选项 %s 缺少实参\n", a); return 2; }
    if (strcmp(a, "--entry") == 0) { entry = argv[++i]; continue; }
    if (strcmp(a, "--out") == 0) { out_path = argv[++i]; continue; }
    if (strcmp(a, "--source") == 0) {
      if (source_count == DRIVER_MAX_SOURCES) { printf("源码份数超限\n"); return 2; }
      source_paths[source_count++] = argv[++i];
      continue;
    }
    if (strcmp(a, "--fuel") == 0) { if (!parse_u64(argv[++i], &fuel)) return 2; continue; }
    if (strcmp(a, "--slice") == 0) { if (!parse_u64(argv[++i], &slice_fuel)) return 2; continue; }
    if (strcmp(a, "--stack") == 0) { if (!parse_u64(argv[++i], &stack_bytes)) return 2; continue; }
    if (strcmp(a, "--quota") == 0) { if (!parse_u64(argv[++i], &quota_bytes)) return 2; continue; }
    if (strcmp(a, "--workspace") == 0) { if (!parse_u64(argv[++i], &workspace_bytes)) return 2; continue; }
    if (strcmp(a, "--caps") == 0) { if (!parse_u64(argv[++i], &caps_capacity)) return 2; continue; }
    if (strcmp(a, "--depth") == 0) { if (!parse_u64(argv[++i], &max_call_depth)) return 2; continue; }
    if (strcmp(a, "--symbols") == 0) { if (!parse_u64(argv[++i], &symbol_capacity)) return 2; continue; }
    if (strcmp(a, "--symbol-bytes") == 0) { if (!parse_u64(argv[++i], &symbol_bytes)) return 2; continue; }
    printf("未知选项 %s\n", a);
    return 2;
  }
  if (!unit_count) { usage(); return 2; }
  if (!slice_fuel) slice_fuel = 4000000ull;
  if (!fuel) fuel = 8000000ull;

  /* 1. 编译单元：.l1 文本必须过解析与验证。 */
  unit_text = join_units(unit_paths, unit_count, &unit_length);
  if (!unit_text) return 1;
  builder = lainir_builder_new();
  if (!builder) { printf("builder_new 失败\n"); free(unit_text); return 1; }
  memset(&diag, 0, sizeof(diag));
  module = lainir_parse(builder, unit_text, &diag);
  if (!module) {
    printf("PARSE FAILED code=%d line=%u\n", diag.code, diag.line);
    goto done;
  }
  if (lainir_verify(module, &diag) != 0) {
    printf("VERIFY FAILED code=%d line=%u\n", diag.code, diag.line);
    goto done;
  }
  if (!g_quiet)
    printf("units %u files subroutines=%u data=%u\n", unit_count,
           module->subroutine_count, module->data_count);

  /* 2. 地址空间与额度先就位：宿主的每一块存储都要落进这个 space。 */
  lainvm_space_init(&space);
  lainvm_quota_init(&quota, quota_bytes);
  host = lainv2_host_new();
  if (!host) { printf("host_new 失败\n"); goto done; }
  lainv2_host_attach_space(host, &space);
  lainv2_host_attach_quota(host, &quota);
  if (!lainv2_host_init_symbols(host, (uint32_t)symbol_capacity,
                               (uint32_t)symbol_bytes)) {
    printf("符号池分配失败\n");
    goto done;
  }

  /* 3. AstOut 区、工作区。 */
  memset(&expand_limits, 0, sizeof(expand_limits));
  expand_limits.max_depth = 64;
  expand_limits.max_visits = 4000000;
  expand_limits.max_handler_calls = 1000000;
  expand_limits.max_replacement_refs = 65536;
  expand_limits.ast_out_capacity = 1u << 20;
  expand_limits.ast_out_node_capacity = 8192;
  if (!lainv2_host_init_ast_out(host, &expand_limits)) {
    printf("AstOut 区分配失败\n");
    goto done;
  }
  if (!lainv2_host_alloc_workspace(host, workspace_bytes, 64)) {
    printf("工作区分配失败\n");
    goto done;
  }

  /* 4. 逐份登记源码。 */
  {
    uint32_t s;
    for (s = 0; s < source_count; s++) {
      uint32_t length = 0, index = 0;
      char *text = read_text(source_paths[s], &length);
      if (!text) goto done;
      if (!lainv2_host_add_source(host, source_paths[s], text, length, &index)) {
        printf("登记源码失败 %s\n", source_paths[s]);
        free(text);
        goto done;
      }
      /* 登记只借用文本：驱动持有它直到进程结束。 */
      source_text[index] = text;
      if (g_verbose)
        printf("source[%u] %s bytes=%u\n", index, source_paths[s], length);
    }
    if (!g_quiet) printf("sources count=%u\n", source_count);
  }

  /* 5. 能力表与宿主登记。 */
  entries = (LainVmCapEntry *)calloc((size_t)caps_capacity, sizeof(*entries));
  if (!entries) { printf("分配能力表失败\n"); goto done; }
  lainvm_caps_init(&caps, entries, (uint32_t)caps_capacity);
  if (lainv2_host_register(host, &caps) != 0) {
    printf("能力登记失败（容量 %llu 不足？）\n",
           (unsigned long long)caps_capacity);
    goto done;
  }
  if (g_verbose) printf("caps count=%u\n", (unsigned)caps.count);

  memset(&apply_limits, 0, sizeof(apply_limits));
  apply_limits.caps = NULL;  /* apply 在隔离空间里跑，不给宿主能力 */
  /* 生成器常写成递归的字段遍历，沙箱里每一层普通调用与每一层分支区域都占一帧，
     16 层连一个三字段 struct 的展开都不够。 */
  apply_limits.max_call_depth = 128;
  /* 隔离执行实测：步数峰值 67462、栈水位峰值 8760、配额峰值（栈整块入账后）
     约 73728；同样取 4~8 倍。 */
  apply_limits.stack_bytes = 65536;
  apply_limits.fuel = 500000;
  apply_limits.quota_bytes = 524288;
  lainv2_host_set_apply_limits(host, &apply_limits);

  /* 6. 装载映像、起 TCB。 */
  memset(&diag, 0, sizeof(diag));
  image = lainvm_image_load(module, &space, &diag);
  if (!image) {
    printf("IMAGE FAILED code=%d line=%u\n", diag.code, diag.line);
    goto done;
  }
  stack_handle = lainvm_space_alloc_stack(&space, stack_bytes, 1, &quota);
  if (lainvm_space_handle_none(stack_handle)) {
    printf("没有栈区\n");
    goto done;
  }
  lease.space = &space;
  lease.region = stack_handle;
  tcb = lainvm_tcb_new(image, &space, 1, 1, (uint32_t)max_call_depth, lease,
                       &quota);
  if (!tcb) { printf("tcb_new 失败\n"); goto done; }
  lainvm_caps_freeze(&caps);
  memset(&diag, 0, sizeof(diag));
  if (lainvm_tcb_set_caps(tcb, &caps, &diag) != 0) {
    printf("set_caps FAILED code=%d line=%u\n", diag.code, diag.line);
    goto done;
  }
  arg.kind = L1_VALUE_BITS;
  arg.bit_width = 64;
  arg.as.bits = source_count;
  memset(&diag, 0, sizeof(diag));
  if (lainvm_tcb_start(tcb, entry, &arg, 1, &diag) != 0) {
    printf("start FAILED code=%d line=%u\n", diag.code, diag.line);
    goto done;
  }

  /* 7. 推进。 */
  {
    LainVmSliceResult slice = LAINVM_SLICE_RUNNABLE;
    while (slice == LAINVM_SLICE_RUNNABLE && fuel_used < fuel) {
      uint64_t before = lainvm_tcb_steps(tcb);
      uint64_t this_slice = fuel - fuel_used;
      if (this_slice > slice_fuel) this_slice = slice_fuel;
      slice = lainvm_engine_run(tcb, this_slice);
      /* 燃料按真实推进的步数记账：切片只是单次推进的上限。 */
      fuel_used += lainvm_tcb_steps(tcb) - before;
    }
    if (slice == LAINVM_SLICE_RUNNABLE) {
      printf("RUN INCOMPLETE steps=%llu\n",
             (unsigned long long)lainvm_tcb_steps(tcb));
      exit_code = 5;
      goto done;
    }
    if (slice == LAINVM_SLICE_TRAPPED) {
      const LainVmTrap *trap = lainvm_tcb_trap(tcb);
      if (trap) {
        uint32_t si = lainvm_image_region_sub(image, trap->region);
        const char *nm = (si == 0xFFFFFFFFu) ? "?" : lainvm_image_sub_name(image, si);
        printf("TRAP kind=%d status=%d region=%u position=%u sub=%s\n",
               (int)trap->kind, (int)trap->status, trap->region, trap->position,
               nm ? nm : "?");
      }
      /* 走出异常路径时也要看得到 Meta 侧留下的诊断。 */
      print_diags(host);
      exit_code = 3;
      goto done;
    }
  }

  /* 8. 收尾：结果、诊断、输出。 */
  {
    uint32_t length = 0;
    const char *output = lainv2_host_output(host, &length);
    print_diags(host);
    if (out_path) {
      FILE *file = fopen(out_path, "wb");
      if (!file) { printf("写不了 %s\n", out_path); goto done; }
      if (length) fwrite(output, 1, length, file);
      fclose(file);
    }
    if (!g_quiet) {
      printf("steps=%llu\n", (unsigned long long)lainvm_tcb_steps(tcb));
      printf("output bytes=%u\n", length);
      printf("stack peak=%llu\n",
             (unsigned long long)lainvm_tcb_stack_peak(tcb));
      printf("quota peak=%llu\n", (unsigned long long)quota.peak);
      printf("workspace peak=%llu\n",
             (unsigned long long)lainv2_host_workspace_peak(host));
      printf("apply steps peak=%llu\n",
             (unsigned long long)lainv2_host_apply_fuel_peak(host));
      printf("apply quota peak=%llu\n",
             (unsigned long long)lainv2_host_apply_quota_peak(host));
      printf("apply stack peak=%llu\n",
             (unsigned long long)lainv2_host_apply_stack_peak(host));
    }
    if (!out_path && length) {
      fwrite(output, 1, length, stdout);
      if (output[length - 1] != '\n') fputc('\n', stdout);
    }
    if (lainvm_tcb_has_result(tcb)) {
      L1Value r = lainvm_tcb_result(tcb);
      uint64_t bits = r.kind == L1_VALUE_ADDR ? (uint64_t)(uintptr_t)r.as.addr
                                              : r.as.bits;
      if (!g_quiet) printf("result=%llu\n", (unsigned long long)bits);
      exit_code = 0;
    } else {
      printf("NO RESULT\n");
      exit_code = 4;
    }
  }

done:
  {
    uint32_t s;
    for (s = 0; s < DRIVER_MAX_SOURCES; s++) free(source_text[s]);
  }
  if (tcb) lainvm_tcb_free(tcb);
  if (image) lainvm_image_free(image);
  if (host) lainv2_host_free(host);
  free(entries);
  if (builder) lainir_builder_free(builder);
  free(unit_text);
  return exit_code;
}
