/* 驱动：端到端跑通 Meta 机制。
 *
 * 链路（seed/bootstrap/ 是手写 LAINIR，用来自举的那一层）：
 *
 *   1. 把 bootstrap 的源文件按 SOURCE_ORDER 拼成一份 LAINIR 文本，
 *      这就是**初代 Meta**，它自己也是 LAINIR —— 打破鸡生蛋的那一步。
 *   2. 解析 / 验证 / 装载它，admit 一个 TCB。
 *   3. 登记宿主能力（源码读入 + 产物写出），把 host 的地址作为 #addr
 *      传给 lain_std_lower。
 *   4. Meta 读源码、按语言规则产出 canonical LAINIR 文本。
 *   5. 驱动拿这段文本再 parse + verify + 装载 + 执行，检查结果。
 *
 * 这个测试证明的不是「Meta 认识多少 Lain」，而是**机制通了**：
 * 语言规则住在 LAINIR 写的库里，底座只提供源码、产物和 `#eval` 三类原语。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lainir/build.h"
#include "lainir/parse.h"
#include "lainir/print.h"
#include "lainir/verify.h"
#include "lainbackend/emit.h"
#include "lainbackend/target.h"
#include "lainmeta/host.h"
#include "lainvm/engine.h"
#include "lainvm/space.h"

/* 出 C 模式用：把后端写出来的字节直接打到 stdout。 */
static void stdout_write(void *user, const char *bytes, uint32_t size) {
  (void)user;
  fwrite(bytes, 1, size, stdout);
}

static void ignore_symbol(void *user, const char *symbol, bool is_extern) {
  (void)user;
  (void)symbol;
  (void)is_extern;
}

static int failures = 0;

static void report_fail(const char *what, const char *why) {
  printf("FAIL %-22s %s\n", what, why);
  failures++;
}

static char *read_text_file(const char *path, uint32_t *length_out) {
  FILE *file = fopen(path, "rb");
  long size;
  char *text;
  if (!file) return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  size = ftell(file);
  if (size < 0) {
    fclose(file);
    return NULL;
  }
  rewind(file);
  text = (char *)malloc((size_t)size + 1);
  if (!text) {
    fclose(file);
    return NULL;
  }
  if (fread(text, 1, (size_t)size, file) != (size_t)size) {
    free(text);
    fclose(file);
    return NULL;
  }
  text[size] = '\0';
  fclose(file);
  if (length_out) *length_out = (uint32_t)size;
  return text;
}

/* 按清单把 Meta 的源文件拼成一份文本。
 *
 * 清单里的路径是**相对于清单文件所在目录**的，不是相对仓库根、也不是硬拼
 * `seed/bootstrap/`。这条是"换一份 Meta"能成立的前提之一：候选 Meta 的清单
 * 可以放在任何地方，不必挤进主线那个目录。 */
static char *load_bootstrap(const char *order_path, uint32_t *length_out) {
  char *order = read_text_file(order_path, NULL);
  char *joined;
  size_t used = 0;
  size_t cap = 4096;
  char *line;
  char prefix[512];
  const char *slash_fwd;
  const char *slash_bwd;
  const char *last_slash;
  if (!order) return NULL;
  /* 清单路径的目录部分。没有目录分隔符时前缀为空。 */
  slash_fwd = strrchr(order_path, '/');
  slash_bwd = strrchr(order_path, '\\');
  last_slash = slash_fwd;
  if (slash_bwd && (!last_slash || slash_bwd > last_slash)) last_slash = slash_bwd;
  if (last_slash && (size_t)(last_slash - order_path) + 2u < sizeof(prefix)) {
    size_t n = (size_t)(last_slash - order_path) + 1u;
    memcpy(prefix, order_path, n);
    prefix[n] = '\0';
  } else {
    prefix[0] = '\0';
  }
  joined = (char *)malloc(cap);
  if (!joined) {
    free(order);
    return NULL;
  }
  joined[0] = '\0';
  line = strtok(order, "\r\n");
  while (line) {
    char path[512];
    char *text;
    size_t need;
    if (line[0] != '#' && line[0] != '\0') {
      uint32_t text_length = 0;
      snprintf(path, sizeof(path), "%s%s", prefix, line);
      text = read_text_file(path, &text_length);
      if (!text) {
        printf("cannot read bootstrap source: %s\n", path);
        free(joined);
        free(order);
        return NULL;
      }
      need = used + text_length + 2u;
      if (need > cap) {
        char *grown;
        while (cap < need) cap *= 2u;
        grown = (char *)realloc(joined, cap);
        if (!grown) {
          free(text);
          free(joined);
          free(order);
          return NULL;
        }
        joined = grown;
      }
      memcpy(joined + used, text, text_length);
      used += text_length;
      joined[used++] = '\n';
      joined[used] = '\0';
      free(text);
    }
    line = strtok(NULL, "\r\n");
  }
  free(order);
  if (length_out) *length_out = (uint32_t)used;
  return joined;
}

/* Meta 的编译期状态：类型注册表。
 *
 * 布局是 Meta 侧定的（`seed/bootstrap/std/registry.l1` 头注释里的分段表），这里
 * 复述一遍。驱动读它不是在解释语言语义，是在**检查编译期状态**——产出的 LAINIR
 * 里看不见类型 id，所以「同一个类型查两次是不是同一个」只能从这里看。
 *
 * 这是 harness 和 Meta 之间唯一的内部耦合，就这三行。 */
#define META_REG_COUNT_OFF 512u
#define META_REG_ENTRY_OFF 520u
#define META_REG_ENTRY_BYTES 64u

/* Meta 的编译期状态：语法树（布局见 `seed/bootstrap/std/parse.l1` 头注释）。
 * 和类型注册表一样，这是 harness 在**检查编译期状态**——树长什么样，产出的
 * LAINIR 里看不出来。 */
#define META_TREE_BASE_OFF 112u
#define META_TREE_COUNT_OFF 120u
#define META_TREE_ROOTS_OFF 128u
/* 暂存区碰撞式分配器的两个格子（布局见 bootstrap/std/scope.l1 文件头）。 */
#define META_BUMP_WATER_OFF 200u
#define META_BUMP_PEAK_OFF 208u
#define META_NODE_BYTES 48u
#define META_NODE_WORDS (META_NODE_BYTES / 8u)
#define META_NIL 0xFFFFFFFFFFFFFFFFull

/* 节点种类。1/2 是建树时给的，3 起是识别 pass 写上去的。 */
static const char *node_kind_name(uint64_t k) {
  switch (k) {
  case 1: return "词";
  case 2: return "组";
  default: return NULL;
  }
}

/* 印一棵子树。
 *
 * 文本必须按**这份源码自己的**缓冲读，而且读之前核对范围：节点偏移只在它所属
 * 那份源码里有意义。之前这里对所有节点一律用第 0 份源码的缓冲，prelude 的节点
 * 偏移（最大 944）落在 26 字节的用户源码之外，印出来的是堆上邻居 —— 实测印出了
 * 一段 PATH（`C:\Program Files\PowerShell\7;…`）。树没错，是这里读错了缓冲。
 * 下标也要核：坏下标照样乘 48 就是读到暂存区外面。 */
static void dump_subtree(const unsigned char *base, uint64_t tree_base,
                         uint64_t count, uint64_t index, const char *text,
                         uint64_t text_len) {
  const unsigned char *node;
  uint64_t w[META_NODE_WORDS], k;
  const char *kn;
  if (index >= count) {
    printf("  #%-3llu <坏下标，>= 节点总数 %llu>\n", (unsigned long long)index,
           (unsigned long long)count);
    return;
  }
  node = base + tree_base + index * META_NODE_BYTES;
  memcpy(w, node, sizeof(w));
  kn = node_kind_name(w[0]);
  if (w[0] == 2) {
    printf("  #%-3llu 组   [%4llu..%4llu] ty=%-2llu 孩子:",
           (unsigned long long)index, (unsigned long long)w[1],
           (unsigned long long)(w[1] + w[2]), (unsigned long long)w[5]);
    for (k = w[3]; k != META_NIL;) {
      uint64_t cw[META_NODE_WORDS];
      printf(" #%llu", (unsigned long long)k);
      memcpy(cw, base + tree_base + k * META_NODE_BYTES, sizeof(cw));
      k = cw[4];
    }
    printf("\n");
    for (k = w[3]; k != META_NIL;) {
      uint64_t cw[META_NODE_WORDS];
      dump_subtree(base, tree_base, count, k, text, text_len);
      memcpy(cw, base + tree_base + k * META_NODE_BYTES, sizeof(cw));
      k = cw[4];
    }
  } else if (w[1] + w[2] > text_len) {
    printf("  #%-3llu %-4s [%4llu..%4llu] <越界，不印>\n",
           (unsigned long long)index, kn ? kn : "?", (unsigned long long)w[1],
           (unsigned long long)(w[1] + w[2]));
  } else if (kn) {
    printf("  #%-3llu %-4s [%4llu..%4llu] ty=%-2llu \"%.*s\"\n",
           (unsigned long long)index, kn, (unsigned long long)w[1],
           (unsigned long long)(w[1] + w[2]), (unsigned long long)w[5],
           (int)w[2], text + w[1]);
  } else {
    printf("  #%-3llu 种类%-3llu [%4llu..%4llu] ty=%-2llu \"%.*s\"\n",
           (unsigned long long)index, (unsigned long long)w[0],
           (unsigned long long)w[1], (unsigned long long)(w[1] + w[2]),
           (unsigned long long)w[5], (int)w[2], text + w[1]);
  }
}

static void report_tree(LainMetaHost *host) {
  uint32_t scratch_size = 0;
  void *scratch = lainmeta_host_scratch(host, &scratch_size);
  const unsigned char *base = (const unsigned char *)scratch;
  uint64_t tree_base = 0, count = 0, roots_base = 0;
  uint32_t sources, s;
  if (!scratch || scratch_size < META_TREE_ROOTS_OFF + 8u) return;
  memcpy(&tree_base, base + META_TREE_BASE_OFF, 8);
  memcpy(&count, base + META_TREE_COUNT_OFF, 8);
  /* 每份源码各有一棵树，根下标按源码下标存在 roots 数组里。 */
  memcpy(&roots_base, base + META_TREE_ROOTS_OFF, 8);
  sources = lainmeta_host_source_count(host);
  printf("tree:      %llu nodes, %u source(s), %u bytes/node\n",
         (unsigned long long)count, sources, META_NODE_BYTES);
  for (s = 0; s < sources; s++) {
    uint64_t root = META_NIL;
    uint32_t text_len = 0;
    const char *text = lainmeta_host_source_text(host, s, &text_len);
    const char *path = lainmeta_host_source_path(host, s);
    memcpy(&root, base + roots_base + (uint64_t)s * 8u, 8);
    printf("  source %u: %s (%u bytes) root #%llu\n", s, path ? path : "?",
           text_len, (unsigned long long)root);
    if (!text || root == META_NIL || root >= count) continue;
    dump_subtree(base, tree_base, count, root, text, text_len);
  }
}

static uint64_t meta_reg_count(LainMetaHost *host) {
  uint32_t scratch_size = 0;
  void *scratch = lainmeta_host_scratch(host, &scratch_size);
  uint64_t count = 0;
  if (!scratch || scratch_size < META_REG_ENTRY_OFF) return 0;
  memcpy(&count, (const unsigned char *)scratch + META_REG_COUNT_OFF, sizeof(count));
  return count;
}

static void report_type_registry(LainMetaHost *host, int verbose) {
  uint32_t scratch_size = 0;
  void *scratch = lainmeta_host_scratch(host, &scratch_size);
  const unsigned char *base = (const unsigned char *)scratch;
  uint64_t count = meta_reg_count(host);
  uint64_t i;
  printf("types:     %llu registered\n", (unsigned long long)count);
  if (!verbose || !scratch) return;
  for (i = 0; i < count; i++) {
    uint64_t w[8];
    memcpy(w, base + META_REG_ENTRY_OFF + i * META_REG_ENTRY_BYTES, sizeof(w));
    printf("type %llu: kind=%llu repr=%llu/%llu owner=%llu ns=%llu args=%llu+%llu\n",
           (unsigned long long)w[0], (unsigned long long)w[1],
           (unsigned long long)w[2], (unsigned long long)w[3],
           (unsigned long long)w[4], (unsigned long long)w[5],
           (unsigned long long)w[6], (unsigned long long)w[7]);
  }
}

/* 暂存区分配器的水位 / 历史峰值。
 *
 * **「够不够」读 peak，不要读 water** —— release 会把水位退回去，所以水位只反映
 * "此刻用了多少"；peak 只增，是这一趟真正要过的最大值。 */
static void report_scratch_bump(LainMetaHost *host) {
  uint32_t scratch_size = 0;
  void *scratch = lainmeta_host_scratch(host, &scratch_size);
  uint64_t water = 0, peak = 0;
  if (!scratch || scratch_size < META_BUMP_PEAK_OFF + 8u) return;
  memcpy(&water, (const unsigned char *)scratch + META_BUMP_WATER_OFF, 8);
  memcpy(&peak, (const unsigned char *)scratch + META_BUMP_PEAK_OFF, 8);
  printf("scratch:   %u bytes, bump peak %llu (%.1f%%)\n", scratch_size,
         (unsigned long long)peak,
         scratch_size ? 100.0 * (double)peak / (double)scratch_size : 0.0);
}

/* 解析 + 验证 + 装载 + admit，返回可执行的 TCB。 */
static LainVmTcb *prepare(L1Builder *builder, const char *text,
                          LainVmSpace *space, LainVmImage **image_out,
                          const char *what) {
  L1Diagnostic diag;
  const L1Module *module;
  LainVmImage *image;
  LainVmTcb *tcb;
  diag.code = 0;
  module = lainir_parse(builder, text, &diag);
  if (!module) {
    char buffer[192];
    snprintf(buffer, sizeof(buffer), "parse: %d line %u %s", diag.code,
             diag.line, diag.message);
    report_fail(what, buffer);
    return NULL;
  }
  if (lainir_verify(module, &diag) != 0) {
    char buffer[192];
    snprintf(buffer, sizeof(buffer), "verify: %d line %u %s", diag.code,
             diag.line, diag.message);
    report_fail(what, buffer);
    return NULL;
  }
  image = lainvm_image_load(module, space, &diag);
  if (!image) {
    char buffer[192];
    snprintf(buffer, sizeof(buffer), "load: %d %s", diag.code, diag.message);
    report_fail(what, buffer);
    return NULL;
  }
  /* 递归深度是 admit 参数，不是硬上限：Meta 的十进制输出是递归的，
   * 二十来层足够，给 64 留余量。 */
  tcb = lainvm_tcb_new(image, space, 1, 1, 64, 4096, 1);
  if (!tcb) {
    report_fail(what, "admit failed");
    lainvm_image_free(image);
    return NULL;
  }
  *image_out = image;
  return tcb;
}

int main(int argc, char **argv) {
  const char *source_path = argc > 1 ? argv[1] : "seed/tests/meta_source.lain";
  /* 入口名由调用方给：名字是从源码里**搬过来**的，不是写死的，
   * 换个源码就要换个入口。 */
  const char *entry = argc > 2 ? argv[2] : "main";
  long want = argc > 3 ? strtol(argv[3], NULL, 10) : 42;
  /* 可选：断言产物里出现某段文本。类型表的验收靠它——表少一个字节
   * 就会产出错的 repr，光看返回值看不出来。 */
  const char *want_text = argc > 4 ? argv[4] : NULL;
  /* 第 5 个参数是 "c" 时：把产物编成 C 打出来，不执行它。 */
  const char *mode = argc > 5 ? argv[5] : NULL;
  L1Builder *meta_builder = lainir_builder_new();
  L1Builder *out_builder = lainir_builder_new();
  LainMetaHost *host = lainmeta_host_new();
  LainVmSpace meta_space;
  LainVmSpace out_space;
  LainVmImage *meta_image = NULL;
  LainVmImage *out_image = NULL;
  LainVmTcb *meta_tcb = NULL;
  LainVmTcb *out_tcb = NULL;
  LainVmCaps *caps = NULL;
  LainVmSliceResult slice;
  L1Diagnostic diag;
  char *bootstrap;
  char *source;
  uint32_t source_length = 0;
  char *produced;
  uint32_t produced_length = 0;
  const char *output;
  int rc = 1;

  setvbuf(stdout, NULL, _IONBF, 0);

  /* 换一份 Meta：清单路径由 `LAIN_META_MANIFEST` 给，默认是主线那份。
   * 用环境变量而不是 argv，是为了不动已经在用的位置参数契约
   * （`<src> <入口> <期望值> [断言文本] [模式] [逻辑路径=文件]...`）——
   * 那套契约有 80 多条既有调用，加一个位置参数会把它们全部错位。 */
  {
    const char *manifest = getenv("LAIN_META_MANIFEST");
    if (!manifest || !*manifest) manifest = "seed/bootstrap/SOURCE_ORDER";
    printf("meta order: %s\n", manifest);
    bootstrap = load_bootstrap(manifest, NULL);
  }
  if (!bootstrap) {
    printf("cannot load the Meta sources\n");
    return 1;
  }
  source = read_text_file(source_path, &source_length);
  if (!source) {
    printf("cannot read the source: %s\n", source_path);
    return 1;
  }
  printf("bootstrap: %u bytes of hand-written LAINIR\n",
         (unsigned)strlen(bootstrap));
  printf("source:    %s (%u bytes)\n", source_path, source_length);

  if (lainmeta_host_add_source(host, source_path, source, source_length) != 0) {
    printf("cannot register the source\n");
    return 1;
  }

  /* 第 7 个参数起是模块：`逻辑路径=文件`。逻辑路径必须是规范化之后的样子
   * （`std/math.lain`），因为 import 解析是**全路径逐字节匹配**——写成别的
   * 形状就会解析失败，那是要的行为，不是缺陷。
   *
   * 真驱动会从一个模块根递归收集 .lain 自动算相对路径；这里显式给，
   * 免掉目录递归，也让注册表的内容在命令行上看得见。 */
  {
    int i;
    for (i = 6; i < argc; i++) {
      const char *spec = argv[i];
      const char *eq = strchr(spec, '=');
      char logical[512];
      char file[512];
      char *module_text;
      uint32_t module_length = 0;
      size_t logical_length;
      if (!eq || eq == spec) {
        printf("bad module argument (want 逻辑路径=文件): %s\n", spec);
        return 1;
      }
      logical_length = (size_t)(eq - spec);
      if (logical_length >= sizeof(logical)) {
        printf("module logical path too long: %s\n", spec);
        return 1;
      }
      memcpy(logical, spec, logical_length);
      logical[logical_length] = '\0';
      snprintf(file, sizeof(file), "%s", eq + 1);
      module_text = read_text_file(file, &module_length);
      if (!module_text) {
        printf("cannot read module source: %s\n", file);
        return 1;
      }
      /* 文本按引用持有：这块内存要活到 Meta 跑完，所以不 free。 */
      if (lainmeta_host_add_source(host, logical, module_text,
                                   module_length) != 0) {
        printf("cannot register module: %s\n", logical);
        return 1;
      }
      printf("module:    %s <- %s (%u bytes)\n", logical, file,
             (unsigned)module_length);
    }
  }

  /* --- 1. Meta 自己 --- */
  lainvm_space_init(&meta_space);
  /* 源码字节**必须显式授权**：Meta 的 TCB 不自带地址空间，源码地址
   * 也不是它自己的映像的一部分。不授权的话它第一次 #load 就会被
   * 确定性拒绝（1004）——这是设计要的行为，不是缺陷。
   *
   * import 解析要读**注册表里的路径**，所以每份源码的路径也要授权——
   * 它同样在宿主那边，不在 Meta 的映像里。少授权一次就是 1004。 */
  {
    uint32_t count = lainmeta_host_source_count(host);
    uint32_t index;
    for (index = 0; index < count; index++) {
      uint32_t text_length = 0;
      const char *text = lainmeta_host_source_text(host, index, &text_length);
      const char *path = lainmeta_host_source_path(host, index);
      size_t path_length = strlen(path) + 1u; /* 连 NUL 一起授权 */
      if (!text ||
          lainvm_space_handle_none(lainvm_space_add(
              &meta_space, (uintptr_t)text, text_length, LAINVM_MEM_READ, 0)) ||
          lainvm_space_handle_none(lainvm_space_add(
              &meta_space, (uintptr_t)path, (uint32_t)path_length,
              LAINVM_MEM_READ, 0))) {
        report_fail("source grant", "the address space rejected a source");
        goto cleanup;
      }
    }
  }
  /* Meta 的暂存区（表格建在这里）也要授权，而且是**可写**的。
   * 它不属于 Meta 的映像，所以不授权就写不了。 */
  {
    uint32_t scratch_size = 0;
    void *scratch = lainmeta_host_scratch(host, &scratch_size);
    if (!scratch ||
        lainvm_space_handle_none(
            lainvm_space_add(&meta_space, (uintptr_t)scratch, scratch_size,
                             LAINVM_MEM_READ | LAINVM_MEM_WRITE, 0))) {
      report_fail("scratch grant", "the address space rejected the scratch");
      goto cleanup;
    }
  }
  meta_tcb = prepare(meta_builder, bootstrap, &meta_space, &meta_image, "meta");
  if (!meta_tcb) goto cleanup;

  /* 宿主服务也要授权：能力只拿得到裸地址，所以"这个地址能不能读"由宿主这一侧判，
   * 而它要判就得知道是哪个地址空间。没 attach 的宿主对象在 emit_write 上会拒
   * （LAINMETA_ERR_DENIED），而不是照裸地址读。 */
  lainmeta_host_attach_space(host, &meta_space);

  caps = lainvm_caps_new();
  if (!caps || lainmeta_host_register(host, caps) != 0) {
    report_fail("caps", "cannot register the host services");
    goto cleanup;
  }
  diag.code = 0;
  if (lainvm_tcb_set_caps(meta_tcb, caps, &diag) != 0) {
    report_fail("caps", "cannot resolve the host services");
    goto cleanup;
  }
  printf("caps:      %u registered, %u resolved\n", lainvm_caps_count(caps),
         meta_tcb->resolved_count);

  /* --- 2. 跑 Meta --- */
  {
    L1Value args[2];
    memset(args, 0, sizeof(args));
    args[0] = (L1Value){L1_VALUE_ADDR, 0, {.addr = (void *)host}};
    args[1] = (L1Value){L1_VALUE_BITS, 64, {.bits = 0}};
    diag.code = 0;
    if (lainvm_tcb_start(meta_tcb, "lain_std_lower", args, 2, &diag) != 0) {
      char buffer[192];
      snprintf(buffer, sizeof(buffer), "start: %d %s", diag.code, diag.message);
      report_fail("meta run", buffer);
      goto cleanup;
    }
    slice = lainvm_engine_run(meta_tcb, 10000000);
    if (slice != LAINVM_SLICE_DONE) {
      char buffer[192];
      snprintf(buffer, sizeof(buffer),
               "slice=%d trap kind=%d status=%d region=%u pos=%u steps=%llu",
               (int)slice, (int)meta_tcb->trap.kind, meta_tcb->trap.status,
               meta_tcb->trap.region, meta_tcb->trap.position,
               (unsigned long long)meta_tcb->steps);
      report_fail("meta run", buffer);
      goto cleanup;
    }
    printf("meta:      %llu steps, host status %u\n",
           (unsigned long long)meta_tcb->steps,
           lainmeta_host_status(host));
    /* 编译期状态先打：跑挂了的时候，最想看的正是「它把源码读成了什么样」。 */
    report_type_registry(host, mode && strcmp(mode, "types") == 0);
    report_scratch_bump(host);
    if (mode && strcmp(mode, "tree") == 0) report_tree(host);
    if (meta_tcb->result.as.bits != 0) {
      char buffer[192];
      snprintf(buffer, sizeof(buffer), "Meta returned status %llu",
               (unsigned long long)meta_tcb->result.as.bits);
      report_fail("meta run", buffer);
      goto cleanup;
    }
    /* 宿主状态是**诊断通道**：`lain_meta_fail` 里的码记在这里。深层的辅助
     * 过程（比如算布局的）只能回一个打包值，没法把状态带上来，所以它们
     * 用这个通道报错。不看它的话那些错误就没人管了——实测过一次：
     * 类型名没解析出来，布局照算，`__size` 静静变成 62。 */
    if (lainmeta_host_status(host) != 0) {
      char buffer[192];
      snprintf(buffer, sizeof(buffer), "host status %u",
               lainmeta_host_status(host));
      report_fail("meta run", buffer);
      goto cleanup;
    }
  }

  /* --- 3. 产物 --- */
  output = lainmeta_host_output(host);
  produced_length = lainmeta_host_output_length(host);
  produced = (char *)malloc(produced_length + 1u);
  if (!produced) goto cleanup;
  memcpy(produced, output, produced_length);
  produced[produced_length] = '\0';
  printf("--- produced LAINIR (%u bytes) ---\n%s", produced_length, produced);
  printf("---\n");

  if (want_text && strstr(produced, want_text) == NULL) {
    char buffer[192];
    snprintf(buffer, sizeof(buffer), "produced text lacks `%s`", want_text);
    report_fail("repr", buffer);
    goto cleanup;
  }

  /* 规范形：把 Meta 产出的文本 parse + verify 再打一遍。
   *
   * 语料比较要的是**唯一**的规范形——C 侧的 `lainir-print` 定义它（平坦、类型
   * 实参总写出来、十进制、模块项之间一个空行）。有了它，"两份不同的 Meta 产出
   * 同一份 IR"才是**逐字节可比**的，而不是"看起来差不多"。 */
  if (mode && strcmp(mode, "canonical") == 0) {
    const L1Module *canon_module;
    char *canonical;
    diag.code = 0;
    canon_module = lainir_parse(out_builder, produced, &diag);
    if (!canon_module) {
      char buffer[192];
      snprintf(buffer, sizeof(buffer), "canonical parse: %d %s", diag.code,
               diag.message);
      report_fail("canonical", buffer);
      goto cleanup;
    }
    if (lainir_verify(canon_module, &diag) != 0) {
      char buffer[192];
      snprintf(buffer, sizeof(buffer), "canonical verify: %d %s", diag.code,
               diag.message);
      report_fail("canonical", buffer);
      goto cleanup;
    }
    canonical = lainir_print_to_string(canon_module);
    if (!canonical) {
      report_fail("canonical", "cannot print the canonical form");
      goto cleanup;
    }
    printf("--- canonical LAINIR ---\n%s", canonical);
    printf("--- end canonical ---\n");
    free(canonical);
  }

  /* 出 C：把 Meta 产出的 LAINIR 交给 C 后端。这是整条链的最后一跳。 */
  if (mode && strcmp(mode, "c") == 0) {
    const L1Module *prod_module;
    static const uint32_t ints[4] = {8, 16, 32, 64};
    static const uint32_t floats[2] = {32, 64};
    LainTarget target;
    LainBackendSink sink;
    LainBackend *backend;

    diag.code = 0;
    prod_module = lainir_parse(out_builder, produced, &diag);
    if (!prod_module) {
      char buffer[192];
      snprintf(buffer, sizeof(buffer), "parse: %d %s", diag.code, diag.message);
      report_fail("produced parse", buffer);
      goto cleanup;
    }
    if (lainir_verify(prod_module, &diag) != 0) {
      char buffer[192];
      snprintf(buffer, sizeof(buffer), "verify: %d %s", diag.code,
               diag.message);
      report_fail("produced verify", buffer);
      goto cleanup;
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
    sink.write = stdout_write;
    sink.symbol = ignore_symbol;
    sink.user = NULL;
    diag.code = 0;
    backend = lainbackend_new(&target, &sink, &diag);
    if (!backend) {
      report_fail("backend", "cannot create");
      goto cleanup;
    }
    if (lainbackend_emit(backend, prod_module) != 0) {
      char buffer[192];
      snprintf(buffer, sizeof(buffer), "emit: %d %s", diag.code, diag.message);
      report_fail("backend", buffer);
    }
    lainbackend_free(backend);
    rc = failures == 0 ? 0 : 1;
    goto cleanup;
  }

  lainvm_space_init(&out_space);
  out_tcb = prepare(out_builder, produced, &out_space, &out_image, "produced");
  if (!out_tcb) goto cleanup;

  {
    LainVmSliceResult out_slice;
    diag.code = 0;
    if (lainvm_tcb_start(out_tcb, entry, NULL, 0, &diag) != 0) {
      char buffer[192];
      snprintf(buffer, sizeof(buffer), "start: %d %s", diag.code, diag.message);
      report_fail("produced run", buffer);
      goto cleanup;
    }
    out_slice = lainvm_engine_run(out_tcb, 1000000);
    if (out_slice != LAINVM_SLICE_DONE) {
      report_fail("produced run", "trapped");
      goto cleanup;
    }
    printf("%s()%*s= %llu\n", entry, (int)(10 - strlen(entry)) > 0 ? (int)(10 - strlen(entry)) : 0, " ",
           (unsigned long long)out_tcb->result.as.bits);
    if ((long)out_tcb->result.as.bits != want) {
      char buffer[192];
      snprintf(buffer, sizeof(buffer), "expected %ld", want);
      report_fail(entry, buffer);
      goto cleanup;
    }
  }

  printf("---\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
  rc = failures == 0 ? 0 : 1;

cleanup:
  free(bootstrap);
  free(source);
  if (caps) lainvm_caps_free(caps);
  if (meta_tcb) lainvm_tcb_free(meta_tcb);
  if (out_tcb) lainvm_tcb_free(out_tcb);
  if (meta_image) lainvm_image_free(meta_image);
  if (out_image) lainvm_image_free(out_image);
  lainmeta_host_free(host);
  lainir_builder_free(meta_builder);
  lainir_builder_free(out_builder);
  return rc;
}
