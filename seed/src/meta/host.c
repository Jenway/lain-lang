/* lainmeta/host.h 的实现。
 *
 * 每个宿主函数都是 LainVmHostFn：第一个参数是能力槽上绑定的 context
 * （这里的 `LainMetaHost *`），随后是**净化后的业务参数**，返回 0 = 成功。
 * context 由可信的注册路径在 `lainmeta_host_register` 时绑到每一项上，
 * Meta 的业务参数里没有它，也无法指定或伪造宿主。 */
#include "lainmeta/host.h"
#include "lainmeta/tree.h"
#include "ast_out.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

typedef struct {
  const char *path;
  const char *text;
  uint32_t length;
  LainMetaTree *tree;
  /* 段号在登记时定，规则见 segment_pair_for：主源码 1/2，额外源码成对 4/5、
   * 6/7、……。按**注册顺序**发号，不按宿主地址排序；编号不回收、不复用。 */
  uint16_t source_segment;
  uint16_t tree_segment;
  LainAstRef current_root;
} LainMetaSource;

/* 只分配进程内编译会话编号；不会把宿主地址当成身份，也不提供权限。 */
static atomic_uint_fast64_t g_meta_session_next = ATOMIC_VAR_INIT(1);

/* 到达编号上限后永久失败，不能回绕并重新发出已用过的会话身份。 */
static uint64_t allocate_session_id(void) {
  uint_fast64_t next = atomic_load_explicit(&g_meta_session_next,
                                            memory_order_relaxed);
  while (next != 0 && next != UINT64_MAX) {
    if (atomic_compare_exchange_weak_explicit(
            &g_meta_session_next, &next, next + 1,
            memory_order_relaxed, memory_order_relaxed))
      return (uint64_t)next;
  }
  return 0;
}

/* 一份源码最多能拿到的下标：段号是 16 位，额外源码第 i 份的 AstIn 段号是
 * `2*i + 3`，所以 `i <= 32766` 时最大段号正好 65535。超过就在登记时拒。 */
#define LAIN_META_SOURCE_MAX 32766u

/* 段号分配（规范 §1.2）：主源码 Source=1、AstIn=2；**3 是 AstOut 的保留编号**，
 * 第一期没有消费者也不许被源码占用；额外源码按注册顺序成对发 4/5、6/7、……
 * 失败源码同样占住它的段号对，不回收给后面的源码。
 *
 * 先算后验：段号必须落在 1..65535，两号必须不同，谁也不能等于 AstOut 的 3。
 * 这里判不出来就返回 false，登记失败——绝不做 16 位截断。 */
static bool segment_pair_for(uint32_t index, uint16_t *source_out,
                             uint16_t *tree_out) {
  uint32_t source, tree;
  if (index == 0) {
    source = LAIN_AST_SOURCE;
    tree = LAIN_AST_IN;
  } else {
    source = 2u * index + 2u;
    tree = 2u * index + 3u;
  }
  if (source == 0 || source > UINT16_MAX) return false;
  if (tree == 0 || tree > UINT16_MAX) return false;
  if (source == tree) return false;
  if (source == LAIN_AST_OUT || tree == LAIN_AST_OUT) return false;
  *source_out = (uint16_t)source;
  *tree_out = (uint16_t)tree;
  return true;
}

/* 段表：引用里的段号在这里解。第一期只有 Source 与 AstIn 两种段
 *（AstOut 的编号 3 保留，没有消费者）。表按 (kind, source) **推导**出段号再散列，
 * 不按插入顺序 —— 相同注册顺序必须得到相同编号。 */
typedef enum {
  LAIN_SEGMENT_NONE = 0,
  LAIN_SEGMENT_SOURCE = 1,
  LAIN_SEGMENT_AST = 2,
} LainSegmentKind;

typedef struct {
  uint16_t id;
  uint16_t kind;
  uint32_t source;
  bool live;
} LainMetaSegment;

#define LAIN_META_SEGMENT_CAP 256u

_Static_assert(sizeof(LainMetaTypeInfo) == 64,
               "Meta 类型摘要必须占八个 64 位字");

struct LainMetaHost {
  uint64_t session_id;
  LainAstOutput *ast_out;
  LainMetaSource *sources;
  uint32_t source_count;
  uint32_t source_cap;
  char *out;
  uint32_t out_length;
  uint32_t out_cap;
  struct {
    char *out;
    uint32_t out_length;
    uint32_t out_cap;
  } emit_scopes[64];
  uint32_t emit_scope_count;
  uint32_t status;
  uint64_t diagnostic_source, diagnostic_offset;
  LainMetaTypeInfo *types;
  uint32_t type_count;
  uint32_t type_cap;
  /* Meta 的可写暂存：表格建在这里。驱动负责把这块范围登记进 VSpace。 */
  unsigned char *scratch;
  uint32_t scratch_size;
  uint64_t scratch_peak;
  bool scratch_peak_ready;
  /* 这份宿主服务被授权到哪个地址空间（驱动 attach；NULL = 没授权）。
   * 能力表里没有 user_data，所以授权随**宿主对象**走，且由驱动显式给。 */
  LainVmSpace *space;
  /* 这次执行的分配账户（驱动 attach；NULL = 不限额）。暂存区与输出扩容都要
   * 先过账户：宿主也是"实际承诺一块底层存储"的一方。 */
  LainVmQuota *quota;
  uint64_t charged; /* 已经计入账户的字节数（暂存区 + 输出缓冲容量） */
  LainApplyLimits apply_limits;
  uint64_t apply_fuel_remaining;
  uint32_t apply_active_depth;
  LainApplyValue apply_result;
  LainApplyBytes apply_result_bytes;
  uint32_t apply_status;
  L1Diagnostic apply_diagnostic;
  bool apply_ready;
  uint32_t apply_requests;
  /* 段表。第一期的段只有 Source 与 AstIn；`live` 由驱动撤销映射时清掉
   * （`lainmeta_host_revoke_tree`），撤销后引用解析一律拒 9401。 */
  LainMetaSegment segments[LAIN_META_SEGMENT_CAP];
  uint32_t segment_count;
  uint64_t segment_bytes; /* 段表在暂存区里占的字节数（计账用） */
  uint64_t roots_bytes;   /* 每份源码一个 root 槽 */
  /* 诊断用：最近一次解析失败的节点引用与拒码。放在暂存区固定格子之外的宿主
   * 结构里，由 meta_boot 在 trap 后读出来打印。 */
  uint64_t trace_bad_ref;
  uint32_t trace_bad_code;
  uint32_t trace_bad_count;
  uint32_t trace_deny_site;
  uint32_t trace_status5_site;
  uint64_t trace_s5_a, trace_s5_b;
  uint64_t emit_ring_addr[LAINMETA_EMIT_RING];
  uint64_t emit_ring_size[LAINMETA_EMIT_RING];
  uint32_t emit_ring_count;
  uint64_t trace_slots[LAINMETA_TRACE_SLOTS];
  uint64_t trace_deny_a, trace_deny_b;
};

/* --- 暂存区头部的段/根表 ---------------------------------------------------
 * Meta 只拿得到暂存区地址；段号与根引用必须**不需要额外能力调用**就能读到，
 * 否则每次解析引用都要多一次宿主往返。这块元数据不是映射格式的一部分：
 * 它不是任何段，也不参与 LAINAST 布局。
 *
 *   +0   magic / abi_version / segment_count / source_count   （32 字节）
 *   +32  段表 32 字节一项：id、kind、source、root、size       （segment_count 项）
 *   ...  每份源码 8 字节：该份源码 AstIn 的根引用            （source_count 项）
 */
#define LAIN_META_SCRATCH_MAGIC UINT32_C(0x5341544C) /* 小端字节 L T A S */
#define LAIN_META_SCRATCH_HEADER 32u
#define LAIN_META_SEGMENT_ENTRY 32u
#define LAIN_META_SCRATCH_ABI 1u

/* --- 能力名 ---------------------------------------------------------------
 *   名字                          业务参数                  结果
 *   lain_meta_source_count        ()                        count
 *   lain_meta_source_data         (index)                   文本地址
 *   lain_meta_source_length       (index)                   字节数
 *   lain_meta_source_path_data    (index)                   路径地址
 *   lain_meta_emit_reset          ()                        0
 *   lain_meta_emit_scope_begin    ()                        0
 *   lain_meta_emit_scope_end      ()                        0
 *   lain_meta_emit_write          (addr, length)            0
 *   lain_meta_emit_data           ()                        文本地址
 *   lain_meta_emit_length         ()                        字节数
 *   lain_meta_fail                (code)                    0
 *   lain_meta_apply_request        (text, len, entry, len, args, count) 结果位模式
 *   lain_meta_apply_status         ()                        诊断码
 *   lain_meta_apply_kind           ()                        物理类型类别
 *   lain_meta_apply_width          ()                        位宽
 * 宿主指针**不在**业务参数里：它是能力槽上的 context，由 VM 注入。
 * ------------------------------------------------------------------------- */

static void host_set_status(LainMetaHost *host, uint32_t code) {
  if (code == LAINMETA_ERR_DENIED) host->trace_deny_site++;
  if (code == 5u) {
    host->trace_status5_site++;
    host->trace_s5_a = host->trace_bad_count;
    host->trace_s5_b = (uint64_t)(uintptr_t)__builtin_return_address(0);
  }
  host->status = code;
  host->diagnostic_source = UINT64_MAX;
  host->diagnostic_offset = UINT64_MAX;
}

int lainmeta_host_diagnostic(const LainMetaHost *host, LainMetaDiagnostic *out) {
  if (!host || !out || !host->status) return 0;
  out->code = host->status;
  out->source = host->diagnostic_source;
  out->offset = host->diagnostic_offset;
  return 1;
}


void lainmeta_host_attach_space(LainMetaHost *host, LainVmSpace *space) {
  if (!host) return;
  host->space = space;
}

void lainmeta_host_set_apply_limits(LainMetaHost *host,
                                    const LainApplyLimits *limits) {
  if (host && limits) {
    host->apply_limits = *limits;
    host->apply_fuel_remaining = limits->fuel ? limits->fuel : 1000000u;
  }
}

uint32_t lainmeta_host_apply_requests(const LainMetaHost *host) {
  return host ? host->apply_requests : 0;
}

int lainmeta_host_apply_diagnostic(const LainMetaHost *host, L1Diagnostic *out) {
  if (!host || !out || !host->apply_requests || !host->apply_status) return 0;
  *out = host->apply_diagnostic;
  out->code = (int)host->apply_status;
  return 1;
}

/* 宿主边界。这份服务能不能读调用方给的 [addr, addr+length)？
 *
 * 两件事缺一不可：
 *   1) **授权**：驱动得先把地址空间 attach 到这份宿主服务上（没 attach = 身份不明）；
 *   2) **范围**：整段必须落在那个空间授权的区段里。
 * 之前这里是 fail open —— 按裸地址直接 memcpy，于是"从区段末尾多读一字节"
 * （R04）和"换一个没授权的宿主对象"（R05）都能过。先检后写：拒的时候输出缓冲
 * 一个字节都不动。 */
static bool host_range_readable(const LainMetaHost *host, uintptr_t addr,
                                uint64_t length) {
  if (!host->space) return false;
  if (length == 0) return true; /* 不碰内存 */
  if (length > (uint64_t)UINTPTR_MAX - (uint64_t)addr) return false;
  return lainvm_space_check(host->space, addr, length, LAINVM_MEM_READ);
}

static int reserve_output(LainMetaHost *host, uint32_t extra) {
  uint32_t need;
  uint32_t next;
  uint32_t grow;
  char *grown;
  if (extra > UINT32_MAX - 1u - host->out_length) {
    host_set_status(host, LAINMETA_ERR_DENIED);
    return 1;
  }
  need = host->out_length + extra + 1u;
  if (need <= host->out_cap) return 0;
  next = host->out_cap ? host->out_cap : 256u;
  while (next < need) {
    if (next > UINT32_MAX / 2u) {
      next = need;
      break;
    }
    next *= 2u;
  }
  /* 扩容要**先过分配账户**：账户不够就保持原样（旧缓冲、旧容量），
   * 让调用方看到失败，而不是先要了内存再报错。 */
  grow = next - host->out_cap;
  if (lainvm_quota_charge(host->quota, grow) != 0) {
    host_set_status(host, (uint32_t)LAINVM_QUOTA_TRAP);
    return 1;
  }
  grown = (char *)realloc(host->out, next);
  if (!grown) {
    (void)lainvm_quota_release(host->quota, grow); /* 预扣之后失败必须回滚 */
    host_set_status(host, LAINMETA_ERR_OOM);
    return 1;
  }
  host->charged += grow;
  host->out = grown;
  host->out_cap = next;
  return 0;
}

void lainmeta_host_attach_quota(LainMetaHost *host, LainVmQuota *quota) {
  if (host) host->quota = quota;
}

LainMetaHost *lainmeta_host_new(void) {
  LainMetaHost *host = (LainMetaHost *)calloc(1, sizeof(LainMetaHost));
  if (!host) return NULL;
  host->session_id = allocate_session_id();
  if (!host->session_id) {
    free(host);
    return NULL;
  }
  host->apply_limits.max_call_depth = 64;
  host->apply_limits.stack_bytes = 4096;
  host->apply_limits.fuel = 1000000;
  host->apply_fuel_remaining = host->apply_limits.fuel;
  /* 工作区**按需分配**：大小要等源码都登记完才知道（见下）。 */
  host->scratch = NULL;
  host->scratch_size = 0;
  return host;
}

/* 工作区的大小**由编译单元决定**，不是一个常数。
 *
 * 以前这里是写死的 64 KiB，理由是「够建几千条表格项」。加了语法树之后不够了：
 * 树是每个 token 一个节点，节点数跟源码字节数是同一个量级。写死大小的后果不是
 * 「慢」，是编译一份稍大的源码就报「装不下」——而那是宿主的资源配得不对，不是
 * 源码有问题，不该让用户看见。
 *
 * 所以按已登记源码的总字节数给：每字节 128 字节工作区（树 + 类型表 + 作用域表），
 * 再保底 64 KiB。**这只是资源配额，不是语言规则**——Meta 仍然自己算它要多少，
 * 装不下照样报 17，不会踩坏。 */
static uint32_t decide_scratch_size(const LainMetaHost *host) {
  uint64_t total = 0;
  uint32_t i;
  uint64_t size;
  for (i = 0; i < host->source_count; i++)
    total += host->sources[i].length;
  size = 65536u + total * 128u;
  /* 段/根表占头部：每份源码两条段记录加一个 root 槽。 */
  size += LAIN_META_SCRATCH_HEADER +
          (uint64_t)host->source_count * 2u * LAIN_META_SEGMENT_ENTRY +
          (uint64_t)host->source_count * 8u;
  if (size > 0x40000000u) size = 0x40000000u; /* 上限 1 GiB，别拿坏输入去要内存 */
  return (uint32_t)size;
}

/* 段表登记：id 从 (kind, source) 推出来，重复登记是幂等的。 */
static bool segment_open(LainMetaHost *host, uint16_t id, uint16_t kind,
                         uint32_t source) {
  uint32_t i;
  if (!id || id > UINT16_MAX) return false;
  for (i = 0; i < host->segment_count; i++) {
    if (host->segments[i].id == id) {
      host->segments[i].live = true;
      return true;
    }
  }
  if (host->segment_count >= LAIN_META_SEGMENT_CAP) return false;
  host->segments[host->segment_count].id = id;
  host->segments[host->segment_count].kind = kind;
  host->segments[host->segment_count].source = source;
  host->segments[host->segment_count].live = true;
  host->segment_count++;
  return true;
}

static const LainMetaSegment *segment_find(LainMetaHost *host, uint16_t id) {
  uint32_t i;
  if (!id) return NULL;
  for (i = 0; i < host->segment_count; i++)
    if (host->segments[i].id == id) return &host->segments[i];
  return NULL;
}

uint16_t lainmeta_host_source_segment(const LainMetaHost *host,
                                      uint32_t source) {
  if (!host || source >= host->source_count) return 0;
  return host->sources[source].source_segment;
}

uint16_t lainmeta_host_tree_segment(const LainMetaHost *host, uint32_t source) {
  if (!host || source >= host->source_count) return 0;
  return host->sources[source].tree_segment;
}

/* 撤销一个 AstIn 段：驱动 unmap 之后调用，撤销后引用解析一律拒 9401。
 * 树本身仍然活着（宿主还要用它诊断），只是不再接受引用。 */
void lainmeta_host_revoke_tree(LainMetaHost *host, uint32_t source) {
  uint16_t id;
  uint32_t i;
  if (!host || source >= host->source_count) return;
  id = host->sources[source].tree_segment;
  for (i = 0; i < host->segment_count; i++)
    if (host->segments[i].id == id) host->segments[i].live = false;
}

/* 把段/根表写进暂存区头部。Meta 读根引用与段号不需要额外能力调用。 */
/* 段表 / 根槽表放在**高位**，不能紧接 32 字节头部。
 *
 * 原因：Meta 的固定格按 docs/ast-v1.md 的布局表分布在 +56/+64/+72/+80/+88/+96/+104
 * 与 +112/+120/+128/+144 —— 全部落在 32..160 之间。宿主若把段表写在
 * `base + 32 + i*32`（4 段时占 32..160），就会和 Meta 的固定格**互相覆盖**：
 * 实测段表项里读到的是 Meta 的容量值（`raw32[2]=0x87 raw32[5]=0x1508`），
 * 而 Meta 读到的根引用也是垃圾。两边各写各的数，谁都不对。
 *
 * 放到 4096 之后，Meta 的固定格布局完全不用动（规范里那份布局表继续成立），
 * 宿主也只多占 4KB 头部区。两套布局从此不重叠。 */
#define LAIN_META_TABLE_AT 4096u

static void publish_scratch_header(LainMetaHost *host) {
  unsigned char *base = host->scratch;
  uint32_t table_at = LAIN_META_TABLE_AT;
  uint32_t roots_at;
  uint32_t i;
  if (!base) return;
  {
    uint64_t need = (uint64_t)table_at +
                    (uint64_t)host->segment_count * LAIN_META_SEGMENT_ENTRY +
                    (uint64_t)host->source_count * 8u;
    if (need > host->scratch_size) return; /* 头部放不下就不写，绝不越界 */
    memset(base, 0, (size_t)need);
  }
  memcpy(base + 0, &(uint32_t){LAIN_META_SCRATCH_MAGIC}, 4u);
  memcpy(base + 4, &(uint32_t){LAIN_META_SCRATCH_ABI}, 4u);
  memcpy(base + 8, &host->segment_count, 4u);
  memcpy(base + 12, &host->source_count, 4u);
  roots_at = table_at + host->segment_count * LAIN_META_SEGMENT_ENTRY;
  for (i = 0; i < host->segment_count; i++) {
    unsigned char *entry = base + table_at + i * LAIN_META_SEGMENT_ENTRY;
    uint32_t source = host->segments[i].source;
    uint64_t value = 0;
    uint64_t size = 0;
    if (host->segments[i].kind == LAIN_SEGMENT_AST &&
        source < host->source_count) {
      LainAstArenaView view;
      if (lainmeta_host_arena(host, source, &view)) value = view.root;
      if (host->sources[source].current_root)
        value = host->sources[source].current_root;
      size = host->sources[source].tree
                 ? (uint64_t)host->sources[source].length
                 : 0;
      if (host->sources[source].tree) {
        LainAstArenaView arena;
        if (lainmeta_host_arena(host, source, &arena)) size = arena.used;
      }
    } else if (source < host->source_count) {
      size = host->sources[source].length;
    }
    memcpy(entry + 0, &host->segments[i].id, 2u);
    memcpy(entry + 2, &host->segments[i].kind, 2u);
    memcpy(entry + 4, &source, 4u);
    memcpy(entry + 8, &value, 8u);
    memcpy(entry + 16, &size, 8u);
  }
  for (i = 0; i < host->source_count; i++) {
    LainAstArenaView view;
    uint64_t root = 0;
    if (lainmeta_host_arena(host, i, &view)) root = view.root;
    if (host->sources[i].current_root) root = host->sources[i].current_root;
    memcpy(base + roots_at + (uint64_t)i * 8u, &root, 8u);
  }
}

void *lainmeta_host_scratch(LainMetaHost *host, uint32_t *size_out) {
  if (!host) return NULL;
  if (!host->scratch) {
    uint32_t want = decide_scratch_size(host);
    /* 暂存区是这次执行**实际承诺**的存储：一样要先过账户。 */
    if (lainvm_quota_charge(host->quota, want) != 0) {
      host_set_status(host, (uint32_t)LAINVM_QUOTA_TRAP);
      if (size_out) *size_out = 0;
      return NULL;
    }
    host->scratch = (unsigned char *)calloc(1, want);
    if (!host->scratch) {
      (void)lainvm_quota_release(host->quota, want);
      if (size_out) *size_out = 0;
      return NULL;
    }
    host->scratch_size = want;
    host->charged += want;
    /* 段表**不**在这里建：它在 add_source 时就登记好了（引用解析必须先于
     * 暂存区可用，否则宿主在装配阶段就无法用段号解引用）。这里只是把已经定型的
     * 段/根表写进暂存区头部，方便 Meta 直接读。 */
    host->segment_bytes =
        LAIN_META_SCRATCH_HEADER +
        (uint64_t)host->segment_count * LAIN_META_SEGMENT_ENTRY;
    host->roots_bytes = (uint64_t)host->source_count * 8u;
    publish_scratch_header(host);
  }
  if (size_out) *size_out = host->scratch_size;
  return host->scratch;
}

void lainmeta_host_free(LainMetaHost *host) {
  uint32_t i;
  if (!host) return;
  lain_ast_output_free(host->ast_out);
  /* 退出顺序由驱动保证：先销毁引用这些段的 TCB、再撤销映射，最后才销毁宿主。 */
  for (i = 0; i < host->source_count; i++) {
    lainmeta_tree_free(host->sources[i].tree);
    free((void *)host->sources[i].path);
  }
  free(host->sources);
  free(host->out);
  lainapply_bytes_free(&host->apply_result_bytes);
  for (i = 0; i < host->emit_scope_count; i++)
    free(host->emit_scopes[i].out);
  free(host->types);
  free(host->scratch);
  /* 存储真的还回去了，才归还额度。 */
  (void)lainvm_quota_release(host->quota, host->charged);
  host->charged = 0;
  free(host);
}

int lainmeta_host_add_source(LainMetaHost *host, const char *path,
                             const char *text, uint32_t length) {
  LainMetaSource *grown;
  char *copy;
  uint32_t index;
  uint32_t error_offset = 0;
  if (!host || !text) return 1;
  if (host->source_count > LAIN_META_SOURCE_MAX) return 2; /* 段号 16 位，见上 */
  if (host->source_count >= host->source_cap) {
    uint32_t next = host->source_cap ? host->source_cap * 2u : 8u;
    grown = (LainMetaSource *)realloc(host->sources,
                                      sizeof(LainMetaSource) * (size_t)next);
    if (!grown) return 2;
    host->sources = grown;
    host->source_cap = next;
  }
  copy = (char *)malloc(path ? strlen(path) + 1u : 1u);
  if (!copy) return 2;
  if (path)
    strcpy(copy, path);
  else
    copy[0] = '\0';
  index = host->source_count;
  memset(&host->sources[index], 0, sizeof(host->sources[index]));
  host->sources[index].path = copy;
  host->sources[index].text = text;
  host->sources[index].length = length;
  /* 段号先按规则算出来并验范围，再写进源码项。失败一律不截断、不降级。 */
  if (!segment_pair_for(index, &host->sources[index].source_segment,
                        &host->sources[index].tree_segment)) {
    host_set_status(host, LAIN_AST_ERR_SEGMENT);
    free(copy);
    memset(&host->sources[index], 0, sizeof(host->sources[index]));
    return 3;
  }
  /* **登记即建树**：第一期不许靠 Meta 首次访问触发的懒解析——驱动必须在装配
   * TCB 之前就把全部 AstIn 建好、映射完。失败不发布半成品。
   *
   * 失败**类别**由 reader 明确报回来，不再拿账户的累计 `rejected` 去猜：先有过一次
   * 历史配额拒绝、这一次只是语法错的话，猜法会把语法错说成 1044。 */
  {
    LainMetaTreeStatus why = LAINMETA_TREE_OK;
    host->sources[index].tree = lainmeta_tree_parse_classified(
        index, text, length, &error_offset, host->quota, &why);
    if (!host->sources[index].tree) {
      switch (why) {
      case LAINMETA_TREE_ERR_QUOTA:
        /* 配额耗尽是真的没内存：不登记，让驱动停下。 */
        host_set_status(host, (uint32_t)LAINVM_QUOTA_TRAP);
        host->diagnostic_source = UINT64_MAX;
        host->diagnostic_offset = UINT64_MAX;
        free(copy);
        memset(&host->sources[index], 0, sizeof(host->sources[index]));
        return 3;
      case LAINMETA_TREE_ERR_ALLOC:
        host_set_status(host, LAINMETA_ERR_OOM);
        host->diagnostic_source = UINT64_MAX;
        host->diagnostic_offset = UINT64_MAX;
        free(copy);
        memset(&host->sources[index], 0, sizeof(host->sources[index]));
        return 3;
      case LAINMETA_TREE_ERR_INTERNAL:
        /* 两遍扫描对不上/自检没过：**不是**输入的问题，也不冒充语法错。 */
        host_set_status(host, LAINMETA_ERR_TREE_HANDLE);
        host->diagnostic_source = index;
        host->diagnostic_offset = error_offset;
        free(copy);
        memset(&host->sources[index], 0, sizeof(host->sources[index]));
        return 3;
      default:
        break;
      }
      /* reader 拒了这份源码（未闭合的块注释、括号不配对……）：**源码段照常登记**
       * （长度与字节仍然可读，错误位置就是诊断里的那个偏移），AstIn 段登记但**不活**、
       * Arena 一个字节都不发布。Meta 从段表里读到 `size = 0`，于是干净地拒这份源码
       * （状态 3）——旧实现是懒解析在 Meta 第一次访问时才炸，外部看到的正是「Meta
       * 返回 3 + 诊断 byte N code 34」，这里把同一组观测保留下来，只是改在登记时定案。
       * 段号按注册顺序分配，所以**失败的源码也必须占住它的段号对**，否则后面每份
       * 源码的段号都会错位。 */
      host_set_status(host, LAINMETA_ERR_TREE_PARSE);
      host->diagnostic_source = index;
      host->diagnostic_offset = error_offset;
    }
  }
  if (host->sources[index].tree &&
      !lainmeta_tree_set_segment(host->sources[index].tree,
                                 host->sources[index].tree_segment,
                                 host->sources[index].source_segment)) {
    lainmeta_tree_free(host->sources[index].tree);
    host->sources[index].tree = NULL;
    free(copy);
    memset(&host->sources[index], 0, sizeof(host->sources[index]));
    return 3;
  }
  /* 段在这个时刻就**活着**：报文里的引用解析（解析节点地址、字节跨度、孩子引用）
   * 从登记完那一刻起就该能用段号工作，不依赖暂存区是否已经分配。 */
  if (!segment_open(host, host->sources[index].source_segment,
                    LAIN_SEGMENT_SOURCE, index) ||
      !segment_open(host, host->sources[index].tree_segment, LAIN_SEGMENT_AST,
                    index)) {
    if (host->sources[index].tree) lainmeta_tree_free(host->sources[index].tree);
    host->sources[index].tree = NULL;
    free(copy);
    memset(&host->sources[index], 0, sizeof(host->sources[index]));
    host_set_status(host, LAINMETA_ERR_OOM);
    return 3;
  }
  if (!host->sources[index].tree) {
    /* reader 拒了这份源码：AstIn 段登记但**不活**（引用一律拒 9401），段表里它的
     * `size` 是 0 —— Meta 据此干净地拒这份源码。源码段仍然活着，长度与字节可读。 */
    lainmeta_host_revoke_tree(host, index);
  }
  host->source_count++;
  /* 暂存区已经存在时（罕见：先拿暂存区再补源码），头部要跟着刷新。 */
  if (host->scratch) {
    host->segment_bytes =
        LAIN_META_SCRATCH_HEADER +
        (uint64_t)host->segment_count * LAIN_META_SEGMENT_ENTRY;
    host->roots_bytes = (uint64_t)host->source_count * 8u;
    publish_scratch_header(host);
  }
  return 0;
}

uint32_t lainmeta_host_source_count(const LainMetaHost *host) {
  return host ? host->source_count : 0;
}

const char *lainmeta_host_source_path(const LainMetaHost *host,
                                      uint32_t index) {
  if (!host || index >= host->source_count) return "";
  return host->sources[index].path ? host->sources[index].path : "";
}

/* 第 index 份源码的文本地址与字节数。驱动要授权它们，所以需要读得到
 * ——和路径一样，这些地址都在宿主这边，不在 Meta 的映像里。 */
const char *lainmeta_host_source_text(const LainMetaHost *host,
                                      uint32_t index, uint32_t *length_out) {
  if (!host || index >= host->source_count) {
    if (length_out) *length_out = 0;
    return "";
  }
  if (length_out) *length_out = host->sources[index].length;
  return host->sources[index].text;
}

const char *lainmeta_host_output(const LainMetaHost *host) {
  if (!host) return NULL;
  return host->out ? host->out : "";
}

uint32_t lainmeta_host_output_length(const LainMetaHost *host) {
  return host ? host->out_length : 0;
}

uint32_t lainmeta_host_type_count(const LainMetaHost *host) {
  return host ? host->type_count : 0;
}

int lainmeta_host_type_at(const LainMetaHost *host, uint32_t index,
                          LainMetaTypeInfo *out) {
  if (!host || !out || index >= host->type_count) return 0;
  *out = host->types[index];
  return 1;
}

uint32_t lainmeta_host_status(const LainMetaHost *host) {
  return host ? host->status : LAINMETA_ERR_OOM;
}

void lainmeta_host_clear_status(LainMetaHost *host) {
  if (host) host_set_status(host, LAINMETA_OK);
}

/* --- 能力实现 ------------------------------------------------------------- */

/* 业务参数数目按各能力真实签名**严格**检查：多了少了都拒。
 * count>0 时 args 必须非 NULL 才能访问；零业务参数接受 args=NULL/count=0。
 * 空 context 在任何宿主解引用之前直接拒 5，不更新任何宿主状态。 */

static uint32_t cap_source_count(void *context, const uint64_t *args,
                                 uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  (void)args;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 0) return LAINMETA_ERR_DENIED;
  if (out) *out = host->source_count;
  return 0;
}

static uint32_t cap_session_id(void *context, const uint64_t *args,
                              uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  (void)args;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 0) return LAINMETA_ERR_DENIED;
  if (out) *out = host->session_id;
  return 0;
}

static uint32_t cap_source_data(void *context, const uint64_t *args,
                                uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  uint64_t index;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 1 || !args) return LAINMETA_ERR_DENIED;
  index = args[0];
  if (index >= host->source_count) {
    host_set_status(host, LAINMETA_ERR_NO_SOURCE);
    return LAINMETA_ERR_NO_SOURCE;
  }
  if (out) *out = (uint64_t)(uintptr_t)host->sources[index].text;
  return 0;
}

static uint32_t cap_source_length(void *context, const uint64_t *args,
                                  uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  uint64_t index;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 1 || !args) return LAINMETA_ERR_DENIED;
  index = args[0];
  if (index >= host->source_count) {
    host_set_status(host, LAINMETA_ERR_NO_SOURCE);
    return LAINMETA_ERR_NO_SOURCE;
  }
  if (out) *out = host->sources[index].length;
  return 0;
}

static uint32_t cap_source_path_data(void *context, const uint64_t *args,
                                     uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  uint64_t index;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 1 || !args) return LAINMETA_ERR_DENIED;
  index = args[0];
  if (index >= host->source_count) {
    host_set_status(host, LAINMETA_ERR_NO_SOURCE);
    return LAINMETA_ERR_NO_SOURCE;
  }
  if (out) *out = (uint64_t)(uintptr_t)host->sources[index].path;
  return 0;
}

static uint32_t cap_emit_reset(void *context, const uint64_t *args,
                               uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  (void)args;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 0) return LAINMETA_ERR_DENIED;
  host->out_length = 0;
  if (host->out) host->out[0] = '\0';
  if (out) *out = 0;
  return 0;
}

/* 输出作用域把当前缓冲所有权移入栈帧；作用域内 emitter 原样工作，结束时
 * 释放内层缓冲并恢复父缓冲。apply 在作用域内同步消费 emit_data/length，
 * 因而不必把生成模块复制到第二块宿主内存。 */
static uint32_t cap_emit_scope_begin(void *context, const uint64_t *args,
                                     uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  uint32_t depth;
  (void)args;
  if (!host || count != 0 || host->emit_scope_count >= 64u)
    return LAINMETA_ERR_DENIED;
  depth = host->emit_scope_count++;
  host->emit_scopes[depth].out = host->out;
  host->emit_scopes[depth].out_length = host->out_length;
  host->emit_scopes[depth].out_cap = host->out_cap;
  host->out = NULL;
  host->out_length = 0;
  host->out_cap = 0;
  if (out) *out = 0;
  return LAINMETA_OK;
}

static uint32_t cap_emit_scope_end(void *context, const uint64_t *args,
                                   uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  uint32_t depth, inner_cap;
  (void)args;
  if (!host || count != 0 || host->emit_scope_count == 0)
    return LAINMETA_ERR_DENIED;
  depth = --host->emit_scope_count;
  inner_cap = host->out_cap;
  free(host->out);
  (void)lainvm_quota_release(host->quota, inner_cap);
  host->charged -= inner_cap;
  host->out = host->emit_scopes[depth].out;
  host->out_length = host->emit_scopes[depth].out_length;
  host->out_cap = host->emit_scopes[depth].out_cap;
  host->emit_scopes[depth].out = NULL;
  host->emit_scopes[depth].out_length = 0;
  host->emit_scopes[depth].out_cap = 0;
  if (out) *out = 0;
  return LAINMETA_OK;
}

static uint32_t cap_emit_write(void *context, const uint64_t *args,
                               uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  const char *bytes;
  uint64_t length;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 2 || !args) return LAINMETA_ERR_DENIED;
  bytes = (const char *)(uintptr_t)args[0];
  length = args[1];
  /* 环形缓冲：记下本次的 (addr, size)，覆盖的是最旧的那条，不是同一条。 */
  {
    uint32_t slot = host->emit_ring_count % LAINMETA_EMIT_RING;
    host->emit_ring_addr[slot] = args[0];
    host->emit_ring_size[slot] = args[1];
    host->emit_ring_count++;
  }
  if (length > UINT32_MAX - 1u - host->out_length) {
    host_set_status(host, LAINMETA_ERR_DENIED);
    return LAINMETA_ERR_DENIED;
  }
  if (!bytes && length > 0) return LAINMETA_ERR_UNSUPPORTED;
  /* 边界检查在**任何副作用之前**：拒的时候 out_length 与输出缓冲都不动。 */
  if (!host_range_readable(host, (uintptr_t)bytes, length))
    return LAINMETA_ERR_DENIED;
  if (reserve_output(host, (uint32_t)length)) return LAINMETA_ERR_OOM;
  if (length) memcpy(host->out + host->out_length, bytes, (size_t)length);
  host->out_length += (uint32_t)length;
  host->out[host->out_length] = '\0';
  if (out) *out = 0;
  return 0;
}

static uint32_t cap_emit_data(void *context, const uint64_t *args,
                              uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  (void)args;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 0) return LAINMETA_ERR_DENIED;
  if (out) *out = (uint64_t)(uintptr_t)(host->out ? host->out : "");
  return 0;
}

static uint32_t cap_emit_length(void *context, const uint64_t *args,
                                uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  (void)args;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 0) return LAINMETA_ERR_DENIED;
  if (out) *out = host->out_length;
  return 0;
}

static uint32_t cap_fail(void *context, const uint64_t *args, uint32_t count,
                         uint64_t *out) {
  LainMetaHost *host = context;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 1 || !args) return LAINMETA_ERR_DENIED;
  host_set_status(host, (uint32_t)args[0]);
  if (out) *out = 0;
  return 0;
}

/* 未知位置使用全一值，合法的文件零和偏移零仍可表示。 */
static uint32_t cap_diagnostic_field(void *context, const uint64_t *args,
                                     uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  uint64_t value;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 1 || !args) return LAINMETA_ERR_DENIED;
  switch (args[0]) {
    case 1: value = host->status; break;
    case 2: value = host->status ? host->diagnostic_source : UINT64_MAX; break;
    case 3: value = host->status ? host->diagnostic_offset : UINT64_MAX; break;
    default: return LAINMETA_ERR_DENIED;
  }
  if (out) *out = value;
  return 0;
}

static uint32_t cap_fail_at(void *context, const uint64_t *args, uint32_t count,
                            uint64_t *out) {
  LainMetaHost *host = context;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 3 || !args) return LAINMETA_ERR_DENIED;
  if (out) *out = 0;
  if (!args[0] || args[0] > UINT32_MAX || args[1] >= host->source_count ||
      args[2] > host->sources[args[1]].length) {
    host_set_status(host, LAINMETA_ERR_DENIED);
    return 0;
  }
  host_set_status(host, (uint32_t)args[0]);
  host->diagnostic_source = args[1];
  host->diagnostic_offset = args[2];
  if (out) *out = 1;
  return 0;
}

static uint32_t cap_status(void *context, const uint64_t *args, uint32_t count,
                           uint64_t *out) {
  LainMetaHost *host = context;
  (void)args;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 0) return LAINMETA_ERR_DENIED;
  if (out) *out = host->status;
  return 0;
}

/* 描述符是公开的八个 64 位字。先验证、预扣并扩容，最后才提交新条目。 */
static uint32_t cap_type_publish(void *context, const uint64_t *args,
                                 uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  LainMetaTypeInfo info, *grown;
  uint32_t i, next;
  uint64_t bytes;
  if (out) *out = 0;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 1 || !args) return LAINMETA_ERR_DENIED;
  if (!host_range_readable(host, (uintptr_t)args[0], sizeof(info))) {
    host_set_status(host, LAINMETA_ERR_DENIED);
    return 0;
  }
  memcpy(&info, (const void *)(uintptr_t)args[0], sizeof(info));
  if (!info.id) {
    host_set_status(host, LAINMETA_ERR_TYPE_PUBLISH);
    return 0;
  }
  for (i = 0; i < host->type_count; i++) {
    if (host->types[i].id == info.id) {
      host_set_status(host, LAINMETA_ERR_TYPE_PUBLISH);
      return 0;
    }
  }
  if (host->type_count == host->type_cap) {
    if (host->type_cap > UINT32_MAX / 2u) {
      host_set_status(host, LAINMETA_ERR_TYPE_PUBLISH);
      return 0;
    }
    next = host->type_cap ? host->type_cap * 2u : 8u;
    bytes = (uint64_t)(next - host->type_cap) * sizeof(info);
    if ((uint64_t)next * sizeof(info) > SIZE_MAX ||
        lainvm_quota_charge(host->quota, bytes) != 0) {
      host_set_status(host, (uint32_t)LAINVM_QUOTA_TRAP);
      return 0;
    }
    grown = (LainMetaTypeInfo *)realloc(host->types, (size_t)next * sizeof(info));
    if (!grown) {
      (void)lainvm_quota_release(host->quota, bytes);
      host_set_status(host, LAINMETA_ERR_OOM);
      return 0;
    }
    host->types = grown;
    host->type_cap = next;
    host->charged += bytes;
  }
  host->types[host->type_count++] = info;
  if (out) *out = 1;
  return 0;
}

int lainmeta_host_scratch_peak(const LainMetaHost *host, uint64_t *out) {
  if (!host || !out || !host->scratch_peak_ready) return 0;
  *out = host->scratch_peak;
  return 1;
}

static uint32_t cap_scratch_report(void *context, const uint64_t *args,
                                   uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 1 || !args) return LAINMETA_ERR_DENIED;
  if (out) *out = 0;
  /* 峰值是暂存区内的最高使用位置；同一宿主的报告只能增长。 */
  if (args[0] > host->scratch_size ||
      (host->scratch_peak_ready && args[0] < host->scratch_peak)) {
    host_set_status(host, LAINMETA_ERR_RESOURCE_PUBLISH);
    return 0;
  }
  host->scratch_peak = args[0];
  host->scratch_peak_ready = true;
  if (out) *out = 1;
  return 0;
}

static uint32_t cap_scratch_data(void *context, const uint64_t *args,
                                 uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  (void)args;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 0) return LAINMETA_ERR_DENIED;
  if (out) *out = (uint64_t)(uintptr_t)host->scratch;
  return 0;
}

static uint32_t cap_scratch_size(void *context, const uint64_t *args,
                                 uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  (void)args;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 0) return LAINMETA_ERR_DENIED;
  if (out) *out = host->scratch_size;
  return 0;
}

/* apply 请求并入 Expand 统一预算（步骤 4）：先扣 1 次 handler_call，再按
 * ceil((请求文本 + 标量实参记录 + 字节结果长度) / 8) 扣 visit。两笔都成功才启动执行；任一不够就是
 * 9410 的可恢复拒绝（与 cap_ast_charge 同侧），调用方不启动 VM。
 * 未启用 AstOut 时没有统一账户，按规则明确拒绝，不静默放行。 */
static uint32_t apply_budget_charge(LainMetaHost *host, uint64_t text_len) {
  uint32_t rc;
  if (!host->ast_out) return 9347;
  rc = lain_ast_output_charge(host->ast_out, 2, 1);
  if (rc) return LAIN_AST_ERR_BUDGET;
  rc = lain_ast_output_charge(host->ast_out, 1, text_len / 8 + (text_len % 8 != 0));
  return rc ? LAIN_AST_ERR_BUDGET : 0;
}

/* 先核对请求输入，再扣统一预算，最后拷入本次请求私有文本与参数。
 * `text_owned` 只允许宿主 emitter 自己持有的活动输出缓冲通过输入检查。 */
static uint32_t apply_request_core(LainMetaHost *host, const char *text,
                                   uint64_t text_len, const char *entry,
                                   uint64_t entry_len, uint64_t arg_addr,
                                   uint64_t arg_count, uint64_t *out,
                                   bool text_owned, bool want_bytes,
                                   uint64_t result_byte_length,
                                   bool want_byte_arg, uint64_t byte_arg_addr,
                                   uint64_t byte_arg_length,
                                   uint64_t byte_arg_index) {
  char *text_copy = NULL, *entry_copy = NULL;
  LainApplyValue apply_args[8];
  LainApplyResult result;
  L1Diagnostic diag;
  uint64_t arg_bytes;
  LainApplyBytes byte_arg = {0};
  uint64_t request_fuel = 0;
  uint32_t rc;
  LainApplyLimits request_limits;
  if (!host) return LAINMETA_ERR_DENIED;
  lainapply_bytes_free(&host->apply_result_bytes);
  memset(&result, 0, sizeof(result));
  memset(&diag, 0, sizeof(diag));
  memset(&host->apply_result, 0, sizeof(host->apply_result));
  host->apply_ready = false;
  host->apply_status = LAINMETA_ERR_DENIED;
  memset(&host->apply_diagnostic, 0, sizeof(host->apply_diagnostic));
  if (out) *out = 0;
  if (arg_count > 8) {
    host->apply_status = 9341;
    return 0;
  }
  if (want_bytes && (result_byte_length == 0 ||
                     result_byte_length > LAINAPPLY_BYTES_MAX)) {
    host->apply_status = 9345;
    return 0;
  }
  if (want_byte_arg && (byte_arg_length == 0 ||
                        byte_arg_length > LAINAPPLY_BYTES_MAX)) {
    host->apply_status = 9345;
    return 0;
  }
  if (want_byte_arg && (byte_arg_index >= arg_count ||
                        byte_arg_index > UINT32_MAX)) {
    host->apply_status = 9341;
    return 0;
  }
  arg_bytes = arg_count * 24;
  if (!text || !entry || !text_len || !entry_len ||
      text_len > 1024u * 1024u || entry_len > 255u ||
      (!text_owned && !host_range_readable(host, (uintptr_t)text, text_len)) ||
      !host_range_readable(host, (uintptr_t)entry, entry_len) ||
      (arg_bytes && (!arg_addr ||
       !host_range_readable(host, (uintptr_t)arg_addr, arg_bytes))) ||
      (want_byte_arg && (!byte_arg_addr ||
       !host_range_readable(host, (uintptr_t)byte_arg_addr,
                            byte_arg_length)))) {
    host->apply_status = 9330;
    return 0;
  }
  if (memchr(text, 0, (size_t)text_len) ||
      memchr(entry, 0, (size_t)entry_len)) {
    host->apply_status = 9330;
    return 0;
  }
  /* 预算扣账在**参数与地址检查之后、启动执行之前**，也早于计数：
   * 被预算拒绝的请求没有发给 apply 服务，按 apply_requests 的含义不计入。 */
  if (arg_bytes > UINT64_MAX - text_len ||
      (want_byte_arg && byte_arg_length > UINT64_MAX - text_len - arg_bytes) ||
      result_byte_length > UINT64_MAX - text_len - arg_bytes -
                           (want_byte_arg ? byte_arg_length : 0)) {
    host->apply_status = 9345;
    return 0;
  }
  rc = apply_budget_charge(host, text_len + arg_bytes +
      (want_byte_arg ? byte_arg_length : 0) + result_byte_length);
  if (rc) {
    host->apply_status = rc; /* 9410 = 预算可恢复拒绝；9347 = 没有统一预算 */
    return 0;
  }
  host->apply_requests++;
  if (host->apply_fuel_remaining == 0) {
    host->apply_status = 9306;
    goto cleanup_apply;
  }
  text_copy = (char *)malloc((size_t)text_len + 1u);
  entry_copy = (char *)malloc((size_t)entry_len + 1u);
  if (!text_copy || !entry_copy) {
    host->apply_status = LAINMETA_ERR_OOM;
    goto cleanup_apply;
  }
  memcpy(text_copy, text, (size_t)text_len);
  text_copy[text_len] = '\0';
  memcpy(entry_copy, entry, (size_t)entry_len);
  entry_copy[entry_len] = '\0';
  for (uint32_t i = 0; i < (uint32_t)arg_count; i++) {
    uint64_t wire[3];
    memcpy(wire, (const void *)(uintptr_t)(arg_addr + (uint64_t)i * 24),
           sizeof(wire));
    if (wire[0] > TY_ADDR || wire[1] > UINT32_MAX) {
      host->apply_status = 9342;
      goto cleanup_apply;
    }
    apply_args[i].kind = (L1TypeKind)wire[0];
    apply_args[i].width = (uint32_t)wire[1];
    apply_args[i].bits = wire[2];
  }
  memset(&diag, 0, sizeof(diag));
  memset(&result, 0, sizeof(result));
  /* 参数记录是 3 个 64 位单元：kind、width、bits；拷入宿主数组后再交给通用 apply。 */
  if (host->apply_active_depth >= 64u) {
    host->apply_status = 9306;
    goto cleanup_apply;
  }
  request_fuel = host->apply_fuel_remaining;
  /* 具有外部能力的过程可能重入 apply。先从宿主的总余额中划出当前
   * TCB 的最多一半；内层只能再划分剩余部分，所有活动 TCB 的额度总和
   * 因而不超过原额度。无能力的 apply 不可能重入，可以使用全部余额。 */
  if (host->apply_limits.caps && request_fuel > 1u)
    request_fuel /= 2u;
  host->apply_fuel_remaining -= request_fuel;
  request_limits = host->apply_limits;
  request_limits.fuel = request_fuel;
  host->apply_active_depth++;
  if (want_byte_arg) {
    byte_arg.data = (uint8_t *)(uintptr_t)byte_arg_addr;
    byte_arg.length = byte_arg_length;
    rc = (uint32_t)lainapply_proc_with_bytes_arg(
        text_copy, entry_copy, arg_count ? apply_args : NULL,
        (uint32_t)arg_count, (uint32_t)byte_arg_index, &byte_arg,
        &request_limits, &result, &diag);
  } else if (want_bytes)
    rc = (uint32_t)lainapply_proc_bytes(text_copy, entry_copy,
                      arg_count ? apply_args : NULL, (uint32_t)arg_count,
                      result_byte_length, &request_limits, &result, &diag);
  else
    rc = (uint32_t)lainapply_proc(text_copy, entry_copy,
                      arg_count ? apply_args : NULL, (uint32_t)arg_count,
                      &request_limits, &result, &diag);
  host->apply_active_depth--;
  if (result.fuel_used < request_fuel)
    host->apply_fuel_remaining += request_fuel - result.fuel_used;
  if (!rc) {
    /* 内层 capability 可能刚写过共享结果槽；失败的外层请求必须使之失效。 */
    lainapply_bytes_free(&host->apply_result_bytes);
    memset(&host->apply_result, 0, sizeof(host->apply_result));
    host->apply_ready = false;
    host->apply_status = diag.code ? (uint32_t)diag.code : 9330;
    host->apply_diagnostic = diag;
    goto cleanup_apply;
  }
  if (result.kind == LAINAPPLY_RESULT_BYTES) {
    memset(&host->apply_result, 0, sizeof(host->apply_result));
    host->apply_result_bytes = result.bytes;
    result.bytes.data = NULL;
    result.bytes.length = 0;
    host->apply_ready = true;
    host->apply_status = 0;
    if (out) *out = result_byte_length;
    goto cleanup_apply;
  }
  if (result.kind != LAINAPPLY_RESULT_SCALAR) {
    lainapply_bytes_free(&host->apply_result_bytes);
    memset(&host->apply_result, 0, sizeof(host->apply_result));
    host->apply_ready = false;
    host->apply_status = 9330;
    memset(&host->apply_diagnostic, 0, sizeof(host->apply_diagnostic));
    goto cleanup_apply;
  }
  lainapply_bytes_free(&host->apply_result_bytes);
  host->apply_result = result.scalar;
  host->apply_ready = true;
  host->apply_status = 0;
  if (out) *out = result.scalar.bits;
cleanup_apply:
  lainapply_bytes_free(&result.bytes);
  free(entry_copy);
  free(text_copy);
  return 0;
}

/* 外部请求的文本、入口和 wire 都必须在 Meta 地址空间显式授权。 */
static uint32_t cap_apply_request(void *context, const uint64_t *args,
                                  uint32_t count, uint64_t *out) {
  if (count != 6 || !args) return LAINMETA_ERR_DENIED;
  return apply_request_core(
      (LainMetaHost *)context, (const char *)(uintptr_t)args[0], args[1],
      (const char *)(uintptr_t)args[2], args[3], args[4], args[5], out,
      false, false, 0, false, 0, 0, 0);
}

static uint32_t cap_apply_bytes_request(void *context, const uint64_t *args,
                                       uint32_t count, uint64_t *out) {
  if (count != 7 || !args) return LAINMETA_ERR_DENIED;
  return apply_request_core((LainMetaHost *)context,
      (const char *)(uintptr_t)args[0], args[1],
      (const char *)(uintptr_t)args[2], args[3], args[4], args[5], out,
      false, true, args[6], false, 0, 0, 0);
}

static uint32_t cap_apply_bytes_arg_request(void *context,
                                           const uint64_t *args,
                                           uint32_t count, uint64_t *out) {
  if (count != 9 || !args) return LAINMETA_ERR_DENIED;
  return apply_request_core((LainMetaHost *)context,
      (const char *)(uintptr_t)args[0], args[1],
      (const char *)(uintptr_t)args[2], args[3], args[4], args[5], out,
      false, false, 0, true, args[6], args[7], args[8]);
}

/* 对当前 emitter 作用域中的文本 apply。过程名与参数 wire 仍须来自授权的
 * Meta 内存；生成文本的宿主所有权不会暴露成 Meta 可伪造的地址。 */
static uint32_t cap_apply_emitted_request(void *context, const uint64_t *args,
                                          uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  if (!host || count != 4 || !args) return LAINMETA_ERR_DENIED;
  return apply_request_core(host, host->out, host->out_length,
                            (const char *)(uintptr_t)args[0], args[1],
                            args[2], args[3], out, true, false, 0,
                            false, 0, 0, 0);
}

static uint32_t cap_apply_emitted_bytes_request(void *context,
                                               const uint64_t *args,
                                               uint32_t count,
                                               uint64_t *out) {
  LainMetaHost *host = context;
  if (!host || count != 5 || !args) return LAINMETA_ERR_DENIED;
  return apply_request_core(host, host->out, host->out_length,
                            (const char *)(uintptr_t)args[0], args[1],
                            args[2], args[3], out, true, true, args[4],
                            false, 0, 0, 0);
}

static uint32_t cap_apply_emitted_bytes_arg_request(
    void *context, const uint64_t *args, uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  if (!host || count != 7 || !args) return LAINMETA_ERR_DENIED;
  return apply_request_core(host, host->out, host->out_length,
      (const char *)(uintptr_t)args[0], args[1], args[2], args[3], out,
      true, false, 0, true, args[4], args[5], args[6]);
}

static uint32_t cap_apply_status(void *context, const uint64_t *args,
                                uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  (void)args;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 0) return LAINMETA_ERR_DENIED;
  if (out) *out = host->apply_status;
  return 0;
}

static uint32_t cap_apply_kind(void *context, const uint64_t *args,
                              uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  (void)args;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 0) return LAINMETA_ERR_DENIED;
  if (out) *out = host->apply_ready ? host->apply_result.kind : 0;
  return 0;
}

/* 行列属于本次生成的 LAINIR 请求，不代表 Lain 源文件位置。 */
static uint32_t cap_apply_diagnostic_field(void *context, const uint64_t *args,
                                          uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  uint64_t value;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 1 || !args) return LAINMETA_ERR_DENIED;
  switch (args[0]) {
    case 1: value = host->apply_status; break;
    case 2: value = host->apply_diagnostic.line; break;
    case 3: value = host->apply_diagnostic.column; break;
    default: return LAINMETA_ERR_DENIED;
  }
  if (out) *out = value;
  return 0;
}

static uint32_t cap_apply_width(void *context, const uint64_t *args,
                               uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  (void)args;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 0) return LAINMETA_ERR_DENIED;
  if (out) *out = host->apply_ready ? host->apply_result.width : 0;
  return 0;
}

static uint32_t cap_apply_bytes_length(void *context, const uint64_t *args,
                                       uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  (void)args;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 0) return LAINMETA_ERR_DENIED;
  if (out) *out = host->apply_ready ? host->apply_result_bytes.length : 0;
  return 0;
}

static uint32_t cap_apply_bytes_copy(void *context, const uint64_t *args,
                                    uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  uint64_t offset, length, destination;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 3 || !args) return LAINMETA_ERR_DENIED;
  offset = args[0];
  destination = args[1];
  length = args[2];
  if (!host->apply_ready || !host->apply_result_bytes.data ||
      offset > host->apply_result_bytes.length ||
      length > host->apply_result_bytes.length - offset || !host->space ||
      length > (uint64_t)UINTPTR_MAX - (uintptr_t)destination ||
      !lainvm_space_check(host->space, (uintptr_t)destination, length,
                          LAINVM_MEM_WRITE)) {
    if (out) *out = 9344;
    return 0;
  }
  if (length)
    memcpy((void *)(uintptr_t)destination,
           host->apply_result_bytes.data + (size_t)offset, (size_t)length);
  if (out) *out = 0;
  return 0;
}

/* --- AstIn 引用解析 -------------------------------------------------------
 * 段身份在**这里**校验，解读引用绝不越段：先在本段内用减法验证区间，通过之后
 * 才交给 VSpace。固定的四个拒码（9401/9402/9403/9404）就是这一层的返回。
 */

/* 本段的 Arena 视图（只有 AstIn 段有）。段不存在、已撤销或不是 AstIn 都失败，
 * 由调用方记 9401。 */
static bool segment_view(LainMetaHost *host, uint16_t id,
                         LainAstArenaView *out) {
  if (id == LAIN_AST_OUT) return lain_ast_output_view(host->ast_out, out);
  const LainMetaSegment *segment = segment_find(host, id);
  if (!segment || !segment->live || segment->kind != LAIN_SEGMENT_AST)
    return false;
  return lainmeta_host_arena(host, segment->source, out);
}

/* 纯结构校验：只信任调用方已经验过可读性的那块字节。 */
static bool header_ok_trusted(const unsigned char *base, uint64_t window,
                              uint16_t segment_id, LainAstArenaHeader *out);

/* 解引用之前**必须先过这一道**（规范 §2.3）。
 *
 * 规则说得很硬：AST 读取能力要求已绑定合法 VSpace，未绑定就安全拒绝；**每次**
 * 解引用之前验证对应读取范围具有 READ 权限，不能先读 Header/Node 再检查。
 *
 *   * 未绑定 VSpace（`host->space == NULL`）→ 拒（旧实现写成 `host->space && ...`，
 *     于是「没授权」被当成「不用查」，解析照样成功 —— 审计复现过）；
 *   * 零长度不解引用，放行（调用方本来就不许 load）；
 *   * 其余一律 `lainvm_space_check(..., READ)`，失败记 DENIED(5)。
 */
static bool host_read_ok(LainMetaHost *host, uintptr_t address,
                         uint64_t length) {
  if (!host->space) {
    host_set_status(host, LAINMETA_ERR_DENIED);
    return false;
  }
  if (!length) return true;
  if (!lainvm_space_check(host->space, address, length, LAINVM_MEM_READ)) {
    host_set_status(host, LAINMETA_ERR_DENIED);
    return false;
  }
  return true;
}

/* Header 自洽性。**不信任**任何自报字段：窗口长度用驱动登记的那一个。
 *
 * 这一层负责**先验权限再读**：Header 的 64 字节必须在本段已授权的 READ 范围内，
 * 否则连 `memcpy` 都不做。 */
static bool header_read(LainMetaHost *host, const unsigned char *base,
                        uint64_t window, uint16_t segment_id,
                        LainAstArenaHeader *out) {
  if (!base || window < LAIN_AST_HEADER_SIZE) return false;
  if (!host_read_ok(host, (uintptr_t)base, LAIN_AST_HEADER_SIZE)) return false;
  if (segment_id == LAIN_AST_OUT) {
    uint32_t rc = lain_ast_output_check(host->ast_out, 0, 64);
    if (rc) { host_set_status(host, rc); return false; }
  }
  return header_ok_trusted(base, window, segment_id, out);
}

/* 纯结构校验：只信任调用方已经验过可读性的那块字节。 */
static bool header_ok_trusted(const unsigned char *base, uint64_t window,
                              uint16_t segment_id, LainAstArenaHeader *out) {
  LainAstArenaHeader header;
  if (!base || window < LAIN_AST_HEADER_SIZE) return false;
  memcpy(&header, base, sizeof(header));
  if (header.magic != LAIN_AST_MAGIC) return false;
  if (header.abi_version != (uint16_t)LAIN_AST_ABI_VERSION) return false;
  if (header.node_stride != (uint16_t)LAIN_AST_NODE_STRIDE) return false;
  if (header.segment_id != segment_id) return false;
  if (header.flags != 0) return false;
  if (header.nodes_offset < LAIN_AST_HEADER_SIZE) return false;
  if (header.nodes_offset % 8u != 0u) return false;
  if (header.capacity_bytes > window) return false;
  if (header.used_bytes > header.capacity_bytes) return false;
  if (header.node_count > header.node_capacity) return false;
  if (header.node_capacity > (header.capacity_bytes - LAIN_AST_HEADER_SIZE) /
                                 (uint64_t)LAIN_AST_NODE_STRIDE)
    return false;
  if (header.nodes_offset > header.capacity_bytes) return false;
  if (header.node_capacity >
      (header.capacity_bytes - header.nodes_offset) /
          (uint64_t)LAIN_AST_NODE_STRIDE)
    return false;
  /* 预留节点区必须整个落在已分配最高水位之内：已发布的节点槽不能是「还没分配」
   * 的字节。used_bytes 含全部预留槽，所以这里能直接比。 */
  if (header.nodes_offset +
          header.node_capacity * (uint64_t)LAIN_AST_NODE_STRIDE >
      header.used_bytes)
    return false;
  *out = header;
  return true;
}

bool lainmeta_host_arena(const LainMetaHost *host, uint32_t source,
                         LainAstArenaView *out) {
  if (!host || !out || source >= host->source_count) return false;
  return lainmeta_tree_arena(host->sources[source].tree, out);
}

const LainMetaTree *lainmeta_host_tree(LainMetaHost *host, uint32_t source) {
  if (!host || source >= host->source_count) {
    if (host) host_set_status(host, LAINMETA_ERR_NO_SOURCE);
    return NULL;
  }
  return host->sources[source].tree;
}

/* 读节点：引用必须是**本段**已发布的节点槽。
 *
 * 顺序是**先验权限、再解引用**（规范 §2.3）：Header 读之前查 Header 区间，节点
 * memcpy 之前查这 56 字节。任何一步没授权就拒 5，不做「先读后查」。 */
static bool node_read(LainMetaHost *host, LainAstRef ref, LainAstArenaView *view,
                      LainAstArenaHeader *header, LainAstNode *out,
                      uint64_t *offset_out) {
  uint16_t id = lain_ast_segment(ref);
  uint64_t off = lain_ast_offset(ref);
  uint64_t delta;
  const unsigned char *base;
  if (!ref) {
    host_set_status(host, LAIN_AST_ERR_SEGMENT);
    return false;
  }
  if (!segment_view(host, id, view)) {
    host_set_status(host, LAIN_AST_ERR_SEGMENT);
    return false;
  }
  base = (const unsigned char *)view->data;
  /* 未绑定 VSpace、或 Header 那 64 字节不在授权 READ 范围内 → 拒，连字段都不看。 */
  if (!header_read(host, base, view->capacity, id, header)) {
    if (host->status == LAINMETA_ERR_DENIED || id == LAIN_AST_OUT) return false;
    host_set_status(host, LAIN_AST_ERR_HEADER);
    return false;
  }
  if (off < header->nodes_offset) {
    host_set_status(host, LAIN_AST_ERR_SLOT);
    return false;
  }
  delta = off - header->nodes_offset;
  if (delta % LAIN_AST_NODE_STRIDE != 0) {
    host_set_status(host, LAIN_AST_ERR_SLOT);
    return false;
  }
  if (delta / LAIN_AST_NODE_STRIDE >= header->node_count) {
    host_set_status(host, LAIN_AST_ERR_SLOT); /* 未发布的槽 */
    return false;
  }
  if (off > view->used || (uint64_t)LAIN_AST_NODE_STRIDE > view->used - off) {
    host_set_status(host, LAIN_AST_ERR_RANGE);
    return false;
  }
  /* **先验权限，再 memcpy**：这 56 字节必须在授权 READ 范围内。 */
  if (!host_read_ok(host, (uintptr_t)base + (uintptr_t)off,
                    LAIN_AST_NODE_STRIDE))
    return false;
  if (id == LAIN_AST_OUT) {
    uint32_t rc = lain_ast_output_check(host->ast_out, off, 56);
    if (rc) { host_set_status(host, rc); return false; }
  }
  memcpy(out, base + off, sizeof(*out));
  if (offset_out) *offset_out = off;
  return true;
}

bool lainmeta_host_ast_node_addr(LainMetaHost *host, LainAstRef ref,
                                 uintptr_t *address_out) {
  LainAstArenaView view;
  LainAstArenaHeader header;
  LainAstNode node;
  const unsigned char *base;
  uintptr_t address;
  if (!host) return false;
  if (!node_read(host, ref, &view, &header, &node, NULL)) {
    host->trace_bad_ref = ref;
    host->trace_bad_code = host->status;
    host->trace_bad_count++;
    return false;
  }
  if (node.kind != LAIN_AST_TOKEN && node.kind != LAIN_AST_GROUP) {
    host_set_status(host, LAIN_AST_ERR_HEADER);
    return false;
  }
  base = (const unsigned char *)view.data;
  /* 先减后加：区间已经在 node_read 里按本段验证过，这里只做指针加法。**权限检查
   * 已经在 node_read 里做过（未绑定 VSpace 直接拒），这里不重复也不放宽** ——
   * 旧写法是 `host->space && ...`，于是没授权反而放行。 */
  address = (uintptr_t)base + (uintptr_t)lain_ast_offset(ref);
  if (address_out) *address_out = address;
  return true;
}

bool lainmeta_host_ast_span_addr(LainMetaHost *host, LainAstRef ref,
                                 uint64_t length, uintptr_t *address_out) {
  uint16_t id;
  uint64_t off;
  if (!host) return false;
  if (!ref) {
    /* 约定：零长度 + NONE 返回零地址，调用者不得 load。 */
    if (length != 0) {
      host_set_status(host, LAIN_AST_ERR_SEGMENT);
      return false;
    }
    if (address_out) *address_out = 0;
    return true;
  }
  id = lain_ast_segment(ref);
  off = lain_ast_offset(ref);
  if (id == LAIN_AST_OUT) {
    LainAstArenaView v;
    uint32_t rc;
    if (!lain_ast_output_view(host->ast_out, &v)) {
      host_set_status(host, LAIN_AST_ERR_SEGMENT); return false;
    }
    if (off > v.used || length > v.used - off) {
      host_set_status(host, LAIN_AST_ERR_RANGE); return false;
    }
    if (!host_read_ok(host, (uintptr_t)v.data + (uintptr_t)off, length))
      return false;
    rc = lain_ast_output_check(host->ast_out, off, length);
    if (rc) { host_set_status(host, rc); return false; }
    if (address_out) *address_out = (uintptr_t)v.data + (uintptr_t)off;
    return true;
  }
  {
    const LainMetaSegment *segment = segment_find(host, id);
    if (!segment || !segment->live) {
      host_set_status(host, LAIN_AST_ERR_SEGMENT);
      return false;
    }
    if (segment->kind == LAIN_SEGMENT_SOURCE) {
      uint32_t source = segment->source;
      uintptr_t address;
      if (source >= host->source_count) {
        host_set_status(host, LAIN_AST_ERR_SEGMENT);
        return false;
      }
      if (off > host->sources[source].length ||
          length > (uint64_t)host->sources[source].length - off) {
        host_set_status(host, LAIN_AST_ERR_RANGE);
        return false;
      }
      address = (uintptr_t)host->sources[source].text + (uintptr_t)off;
      /* Source 段的地址由宿主自己算出来；**可读性必须在这里验**（未绑定 VSpace
       * 一律拒）——这一路径正是把 Meta 的字节引用解析成宿主地址的地方，放行等于
       * 把「没授权」当成「不用查」。零长度不解引用，只要求空间已绑定。 */
      if (!host->space) {
        host_set_status(host, LAINMETA_ERR_DENIED);
        return false;
      }
      if (!host_read_ok(host, address, length)) return false;
      if (address_out) *address_out = address;
      return true;
    }
    if (segment->kind == LAIN_SEGMENT_AST) {
      LainAstArenaView view;
      LainAstArenaHeader header;
      uintptr_t address;
      const unsigned char *base;
      if (!segment_view(host, id, &view)) {
        host_set_status(host, LAIN_AST_ERR_SEGMENT);
        return false;
      }
      base = (const unsigned char *)view.data;
      /* Header 的 64 字节：先验权限再读（未绑定 VSpace 直接拒）。 */
      if (!header_read(host, base, view.capacity, id, &header)) {
        if (host->status == LAINMETA_ERR_DENIED) return false;
        host_set_status(host, LAIN_AST_ERR_HEADER);
        return false;
      }
      /* 可寻址范围是**已发布**字节：不能靠 Header 自报去读未写的容量。 */
      if (off > view.used || length > view.used - off) {
        host_set_status(host, LAIN_AST_ERR_RANGE);
        return false;
      }
      address = (uintptr_t)base + (uintptr_t)off;
      if (!host_read_ok(host, address, length)) return false;
      if (address_out) *address_out = address;
      return true;
    }
  }
  host_set_status(host, LAIN_AST_ERR_SEGMENT);
  return false;
}

bool lainmeta_host_ast_child_ref(LainMetaHost *host, LainAstRef ref,
                                 uint64_t index, LainAstRef *out) {
  LainAstArenaView view;
  LainAstArenaHeader header;
  LainAstNode node;
  uint64_t offset = 0;
  uint64_t at;
  const unsigned char *base;
  if (!host) return false;
  if (!node_read(host, ref, &view, &header, &node, &offset)) return false;
  if (node.kind != LAIN_AST_GROUP) {
    host_set_status(host, LAIN_AST_ERR_SLOT); /* 词没有孩子 */
    return false;
  }
  if (index >= node.child_count) {
    host_set_status(host, LAIN_AST_ERR_RANGE);
    return false;
  }
  if (!node.children_offset || node.children_offset % 8u ||
      node.children_offset < header.nodes_offset +
          header.node_capacity * LAIN_AST_NODE_STRIDE ||
      node.children_offset > view.used ||
      (uint64_t)node.child_count * 8u > view.used - node.children_offset) {
    host_set_status(host, LAIN_AST_ERR_RANGE);
    return false;
  }
  if ((uint64_t)node.child_count > UINT64_MAX / 8u) {
    host_set_status(host, LAIN_AST_ERR_RANGE);
    return false;
  }
  if (index > (UINT64_MAX - node.children_offset) / 8u) {
    host_set_status(host, LAIN_AST_ERR_RANGE);
    return false;
  }
  at = node.children_offset + index * 8u;
  if (at > view.used || 8u > view.used - at) {
    host_set_status(host, LAIN_AST_ERR_RANGE);
    return false;
  }
  base = (const unsigned char *)view.data;
  /* 孩子列表那一项：**先验权限再 memcpy**（撤销映射后这里必须拒，不能照读）。 */
  if (!host_read_ok(host, (uintptr_t)base + (uintptr_t)at, 8u)) return false;
  if (view.segment_id == LAIN_AST_OUT) {
    uint32_t rc = lain_ast_output_check(host->ast_out, at, 8);
    if (rc) { host_set_status(host, rc); return false; }
  }
  memcpy(out, base + at, 8u);
  {
    /* 孩子引用必须落在同一个 Arena 的已发布节点槽上，并且那个目标节点也要在
     * 授权 READ 范围内（node_read 自己会验）。 */
    LainAstArenaHeader child_header;
    LainAstNode child;
    if (!node_read(host, *out, &view, &child_header, &child, NULL)) return false;
  }
  return true;
}
/* 四个 AstIn 原语。业务参数严格检查：多了少了都拒。
 * 它们一律通过 host 的属性函数解析引用：meta 从不自己解码引用、也不做指针算术。 */
static uint32_t cap_ast_root(void *context, const uint64_t *args, uint32_t count,
                             uint64_t *out) {
  LainMetaHost *host = context;
  LainAstArenaView view;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 1 || !args) return LAINMETA_ERR_DENIED;
  /* AST 读取能力都要求已绑定合法 VSpace（规范 §2.3）：没授权就没有「读 AST」这回事。 */
  if (!host->space) {
    host_set_status(host, LAINMETA_ERR_DENIED);
    if (out) *out = LAIN_AST_REF_NONE;
    return LAINMETA_ERR_DENIED;
  }
  if (args[0] >= host->source_count) {
    host_set_status(host, LAIN_AST_ERR_SEGMENT);
    if (out) *out = LAIN_AST_REF_NONE;
    return host->status;
  }
  if (!lainmeta_host_arena(host, (uint32_t)args[0], &view)) {
    host_set_status(host, LAIN_AST_ERR_SEGMENT);
    if (out) *out = LAIN_AST_REF_NONE;
    return host->status;
  }
  if (host->sources[args[0]].current_root)
    view.root = host->sources[args[0]].current_root;
  {
    uintptr_t address;
    if (!lainmeta_host_ast_node_addr(host, view.root, &address)) {
      if (out) *out = LAIN_AST_REF_NONE;
      return host->status;
    }
  }
  if (out) *out = view.root;
  return 0;
}

static uint32_t cap_ast_node_addr(void *context, const uint64_t *args,
                                  uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  uintptr_t address = 0;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 1 || !args) return LAINMETA_ERR_DENIED;
  if (!lainmeta_host_ast_node_addr(host, (LainAstRef)args[0], &address)) {
    if (out) *out = 0;
    return host->status;
  }
  if (out) *out = (uint64_t)address;
  return 0;
}

static uint32_t cap_ast_span_addr(void *context, const uint64_t *args,
                                  uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  uintptr_t address = 0;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 2 || !args) return LAINMETA_ERR_DENIED;
  if (!lainmeta_host_ast_span_addr(host, (LainAstRef)args[0], args[1],
                                   &address)) {
    if (out) *out = 0;
    return host->status;
  }
  if (out) *out = (uint64_t)address;
  return 0;
}

static uint32_t cap_trace(void *context, const uint64_t *args, uint32_t count,
                          uint64_t *out) {
  LainMetaHost *host = context;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 2 || !args) return LAINMETA_ERR_DENIED;
  if (args[0] < LAINMETA_TRACE_SLOTS) host->trace_slots[args[0]] = args[1];
  if (out) *out = 0;
  return 0;
}

static uint32_t cap_ast_child_ref(void *context, const uint64_t *args,
                                  uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  LainAstRef child = LAIN_AST_REF_NONE;
  if (!host) return LAINMETA_ERR_DENIED;
  if (count != 2 || !args) return LAINMETA_ERR_DENIED;
  if (!lainmeta_host_ast_child_ref(host, (LainAstRef)args[0], args[1], &child)) {
    if (out) *out = LAIN_AST_REF_NONE;
    return host->status;
  }
  if (out) *out = child;
  return 0;
}

typedef struct {
  const char *name;
  LainVmHostFn fn;
} MetaCapability;

/* AstOut 的所有引用仍由同一 Host/CSpace 解析，构造层没有第二套上下文表。 */
static uint32_t output_resolve(void *context, LainAstRef ref, LainAstNode *n) {
  LainMetaHost *host = context;
  uintptr_t at;
  if (!lainmeta_host_ast_node_addr(host, ref, &at)) return host->status;
  memcpy(n, (const void *)at, sizeof(*n));
  return 0;
}
uint32_t lainmeta_host_enable_ast_out(LainMetaHost *host,
                                     const LainExpandLimits *limits) {
  uint32_t rc;
  if (!host) return LAINMETA_ERR_DENIED;
  if (host->ast_out || host->space) return LAIN_AST_ERR_TRANSACTION;
  host->ast_out = lain_ast_output_new(limits, host->quota, host,
                                    output_resolve, &rc);
  if (rc) host_set_status(host, rc);
  return rc;
}
bool lainmeta_host_ast_out(const LainMetaHost *host, LainAstArenaView *out) {
  return host && lain_ast_output_view(host->ast_out, out);
}
/* 只读计数：apply 请求已并入 Expand 统一预算，驱动要能核对实际扣账。
 * 未启用 AstOut 时没有账户，一律返回 0。 */
uint64_t lainmeta_host_ast_handler_calls(const LainMetaHost *host) {
  return host && host->ast_out
             ? lain_ast_output_handler_calls(host->ast_out) : 0;
}
uint64_t lainmeta_host_ast_visits(const LainMetaHost *host) {
  return host && host->ast_out ? lain_ast_output_visits(host->ast_out) : 0;
}
static uint32_t output_entry(LainMetaHost *host, const uint64_t *args,
                             uint32_t count, uint32_t wanted, uint64_t *out) {
  LainAstArenaView view;
  if (out) *out = 0;
  if (!host || count != wanted || (wanted && !args)) return LAINMETA_ERR_DENIED;
  if (!lain_ast_output_view(host->ast_out, &view)) return LAIN_AST_ERR_SEGMENT;
  if (!host->space || !lainvm_space_check(host->space, (uintptr_t)view.data,
        view.capacity, LAINVM_MEM_READ | LAINVM_MEM_WRITE))
    return LAINMETA_ERR_DENIED;
  return 0;
}
static uint32_t output_result(LainMetaHost *host, uint32_t rc,
                              uint64_t value, uint64_t *out) {
  if (rc && host) host_set_status(host, rc);
  if (out) *out = rc ? 0 : value;
  return rc;
}
static bool output_recoverable(uint32_t rc) {
  return rc == LAIN_AST_ERR_CAPACITY || rc == LAIN_AST_ERR_BUDGET;
}
/* 容量与预算耗尽是事务内的正常拒绝：回调本身成功，让 Meta 有机会 rollback。
 * 句柄/引用型能力以 0 表示失败；状态型能力把拒码作为返回值。其它协议错误仍 trap。 */
static uint32_t output_ref_result(LainMetaHost *host, uint32_t rc,
                                  uint64_t value, uint64_t *out) {
  if (!output_recoverable(rc)) return output_result(host, rc, value, out);
  host_set_status(host, rc);
  if (out) *out = 0;
  return 0;
}
static uint32_t output_status_result(LainMetaHost *host, uint32_t rc,
                                     uint64_t *out) {
  if (!output_recoverable(rc)) return output_result(host, rc, 0, out);
  host_set_status(host, rc);
  if (out) *out = rc;
  return 0;
}
static uint32_t cap_ast_tx_begin(void *context, const uint64_t *args,
                                uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  uint64_t id = 0;
  uint32_t rc = output_entry(host, args, count, 0, out);
  if (!rc) rc = lain_ast_output_begin(host->ast_out, &id);
  return output_ref_result(host, rc, id, out);
}
static uint32_t cap_ast_charge(void *context, const uint64_t *args,
                              uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  uint32_t rc = output_entry(host, args, count, 2, out);
  if (!rc) rc = lain_ast_output_charge(host->ast_out, args[0], args[1]);
  return output_status_result(host, rc, out);
}
static uint32_t cap_ast_tx_release(void *context, const uint64_t *args,
                                  uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  uint32_t rc = output_entry(host, args, count, 1, out);
  if (!rc) rc = lain_ast_output_release(host->ast_out, args[0]);
  return output_status_result(host, rc, out);
}
static uint32_t cap_ast_tx_rollback(void *context, const uint64_t *args,
                                   uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  uint32_t rc = output_entry(host, args, count, 1, out);
  if (!rc) rc = lain_ast_output_rollback(host->ast_out, args[0]);
  return output_status_result(host, rc, out);
}
static uint32_t cap_ast_append_token(void *context, const uint64_t *args,
                                    uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  LainAstRef ref = 0;
  uint32_t rc = output_entry(host, args, count, 4, out);
  if (!rc && !host_range_readable(host, (uintptr_t)args[0], args[1]))
    rc = LAINMETA_ERR_DENIED;
  if (!rc) rc = lain_ast_output_token(host->ast_out, (const void *)(uintptr_t)args[0],
                                    args[1], args[2], args[3], &ref);
  return output_ref_result(host, rc, ref, out);
}
static uint32_t cap_ast_append_refs(void *context, const uint64_t *args,
                                   uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  LainAstRef ref = 0;
  uint32_t rc = output_entry(host, args, count, 2, out);
  if (!rc && args[1] > UINT64_MAX / 8) rc = LAIN_AST_ERR_RANGE;
  if (!rc && !host_range_readable(host, (uintptr_t)args[0], args[1] * 8))
    rc = LAINMETA_ERR_DENIED;
  if (!rc) rc = lain_ast_output_refs(host->ast_out,
           (const LainAstRef *)(uintptr_t)args[0], args[1], &ref);
  return output_ref_result(host, rc, ref, out);
}
static uint32_t cap_ast_append_group(void *context, const uint64_t *args,
                                    uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  LainAstRef ref = 0;
  uint32_t rc = output_entry(host, args, count, 5, out);
  if (!rc) rc = lain_ast_output_group(host->ast_out, args[0], args[1],
                                    args[2], args[3], args[4], &ref);
  return output_ref_result(host, rc, ref, out);
}
static uint32_t cap_ast_expansion_id(void *context, const uint64_t *args,
                                    uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  uint64_t value = 0;
  uint32_t rc = output_entry(host, args, count, 1, out);
  if (!rc) {
    if (lain_ast_segment(args[0]) == LAIN_AST_OUT)
      rc = lain_ast_output_expansion(host->ast_out, args[0], &value);
    else {
      LainAstNode node;
      LainAstArenaView view;
      LainAstArenaHeader header;
      if (!node_read(host, args[0], &view, &header, &node, NULL))
        rc = host->status;
    }
  }
  return output_result(host, rc, value, out);
}
static uint32_t cap_ast_commit(void *context, const uint64_t *args,
                              uint32_t count, uint64_t *out) {
  LainMetaHost *host = context;
  LainAstArenaView before;
  uint32_t rc = output_entry(host, args, count, 4, out);
  if (!rc && (args[0] >= host->source_count ||
      !lainmeta_host_arena(host, (uint32_t)args[0], &before)))
    rc = LAIN_AST_ERR_SEGMENT;
  if (!rc) {
    LainAstRef old = host->sources[args[0]].current_root;
    if (!old) old = before.root;
    if (old != args[1]) rc = LAIN_AST_ERR_TRANSACTION;
  }
  if (!rc) rc = lain_ast_output_commit(host->ast_out, args[3], args[2]);
  if (!rc) {
    uint64_t offset = 4096 + (args[0] * 2 + 1) * 32 + 8;
    host->sources[args[0]].current_root = args[2];
    /* 只更新 root 镜像，不重写 Meta 的控制格或语义旁表。 */
    if (host->scratch && offset <= host->scratch_size &&
        8 <= host->scratch_size - offset)
      memcpy(host->scratch + offset, &args[2], 8);
  }
  return output_status_result(host, rc, out);
}

static const MetaCapability k_capabilities[] = {
    {"lain_meta_session_id", cap_session_id},
    {"lain_meta_source_count", cap_source_count},
    {"lain_meta_source_data", cap_source_data},
    {"lain_meta_source_length", cap_source_length},
    {"lain_meta_source_path_data", cap_source_path_data},
    {"lain_meta_emit_reset", cap_emit_reset},
    {"lain_meta_emit_scope_begin", cap_emit_scope_begin},
    {"lain_meta_emit_scope_end", cap_emit_scope_end},
    {"lain_meta_emit_write", cap_emit_write},
    {"lain_meta_emit_data", cap_emit_data},
    {"lain_meta_emit_length", cap_emit_length},
    {"lain_meta_fail", cap_fail},
    {"lain_meta_status", cap_status},
    {"lain_meta_diagnostic_field", cap_diagnostic_field},
    {"lain_meta_fail_at", cap_fail_at},
    {"lain_meta_type_publish", cap_type_publish},
    {"lain_meta_scratch_data", cap_scratch_data},
    {"lain_meta_scratch_size", cap_scratch_size},
    {"lain_meta_scratch_report", cap_scratch_report},
    {"lain_meta_apply_request", cap_apply_request},
    {"lain_meta_apply_bytes_request", cap_apply_bytes_request},
    {"lain_meta_apply_bytes_arg_request", cap_apply_bytes_arg_request},
    {"lain_meta_apply_emitted_request", cap_apply_emitted_request},
    {"lain_meta_apply_emitted_bytes_request", cap_apply_emitted_bytes_request},
    {"lain_meta_apply_emitted_bytes_arg_request", cap_apply_emitted_bytes_arg_request},
    {"lain_meta_apply_status", cap_apply_status},
    {"lain_meta_apply_diagnostic_field", cap_apply_diagnostic_field},
    {"lain_meta_apply_kind", cap_apply_kind},
    {"lain_meta_apply_width", cap_apply_width},
    {"lain_meta_apply_bytes_length", cap_apply_bytes_length},
    {"lain_meta_apply_bytes_copy", cap_apply_bytes_copy},
    {"lain_meta_ast_root", cap_ast_root},
    {"lain_meta_ast_node_addr", cap_ast_node_addr},
    {"lain_meta_ast_span_addr", cap_ast_span_addr},
    {"lain_meta_ast_child_ref", cap_ast_child_ref},
    {"lain_meta_trace", cap_trace},
};

/* 输出能力只授予显式配置 AstOut 的任务，第一期只读消费者不获得写树能力。 */
static const MetaCapability k_output_capabilities[] = {
    {"lain_meta_ast_charge", cap_ast_charge},
    {"lain_meta_ast_tx_begin", cap_ast_tx_begin},
    {"lain_meta_ast_tx_release", cap_ast_tx_release},
    {"lain_meta_ast_tx_rollback", cap_ast_tx_rollback},
    {"lain_meta_ast_append_token", cap_ast_append_token},
    {"lain_meta_ast_append_refs", cap_ast_append_refs},
    {"lain_meta_ast_append_group", cap_ast_append_group},
    {"lain_meta_ast_expansion_id", cap_ast_expansion_id},
    {"lain_meta_ast_commit", cap_ast_commit},
};


/* 登记底座服务：每一项都绑定这份 host 作为 context。
 * host 为 NULL 是参数错误（这层服务不接受无宿主绑定）；表已冻结或重名时
 * lainvm_caps_add 会失败，调用方不能使用半注册的任务。 */
int lainmeta_host_register(LainMetaHost *host, LainVmCaps *caps) {
  size_t i;
  if (!host || !caps) return 1;
  for (i = 0; i < sizeof(k_capabilities) / sizeof(k_capabilities[0]); i++) {
    if (lainvm_caps_add(caps, k_capabilities[i].name, LAINVM_CAP_FUNCTION,
                        k_capabilities[i].fn, host) != 0)
      return 2;
  }
  if (host->ast_out) {
    for (i = 0; i < sizeof(k_output_capabilities) / sizeof(k_output_capabilities[0]); i++) {
      if (lainvm_caps_add(caps, k_output_capabilities[i].name, LAINVM_CAP_FUNCTION,
                         k_output_capabilities[i].fn, host) != 0)
        return 2;
    }
  }
  return 0;
}

/* 诊断：最近一次节点引用解析失败。 */
void lainmeta_host_trace_bad(const LainMetaHost *host, uint64_t *ref_out,
                             uint32_t *code_out, uint32_t *count_out) {
  if (ref_out) *ref_out = host ? host->trace_bad_ref : 0;
  if (code_out) *code_out = host ? host->trace_bad_code : 0;
  if (count_out) *count_out = host ? host->trace_bad_count : 0;
  (void)lainmeta_host_diagnostic;
}

uint32_t lainmeta_host_deny_count(const LainMetaHost *host) {
  return host ? host->trace_deny_site : 0;
}

uint64_t lainmeta_host_trace_slot(const LainMetaHost *host, uint32_t slot) {
  if (!host || slot >= LAINMETA_TRACE_SLOTS) return 0;
  return host->trace_slots[slot];
}

uint32_t lainmeta_host_emit_ring(const LainMetaHost *host, uint32_t index,
                                 uint64_t *addr_out, uint64_t *size_out) {
  uint32_t slot;
  if (!host || index >= LAINMETA_EMIT_RING) return 0;
  slot = (host->emit_ring_count + index) % LAINMETA_EMIT_RING;
  if (addr_out) *addr_out = host->emit_ring_addr[slot];
  if (size_out) *size_out = host->emit_ring_size[slot];
  return host->emit_ring_count;
}

uint32_t lainmeta_host_status5_count(const LainMetaHost *host) {
  return host ? host->trace_status5_site : 0;
}

void lainmeta_host_status5_site(const LainMetaHost *host, uint64_t *a, uint64_t *b) {
  if (a) *a = host ? host->trace_s5_a : 0;
  if (b) *b = host ? host->trace_s5_b : 0;
}
