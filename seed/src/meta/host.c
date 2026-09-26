/* lainmeta/host.h 的实现。
 *
 * 每个宿主函数都是 LainVmHostFn：拿原始 64 位值，返回 0 = 成功。
 * args[0] 永远是 host 自己的地址——Meta 从 initialize 收到它，之后每次调用
 * 都原样传回来。没有全局，也没有 user_data。
 */
#include "lainmeta/host.h"
#include "lainmeta/tree.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *path;
  const char *text;
  uint32_t length;
  LainMetaTree *tree;
} LainMetaSource;

_Static_assert(sizeof(LainMetaTypeInfo) == 64,
               "Meta 类型摘要必须占八个 64 位字");

struct LainMetaHost {
  LainMetaSource *sources;
  uint32_t source_count;
  uint32_t source_cap;
  char *out;
  uint32_t out_length;
  uint32_t out_cap;
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
  LainEvalLimits eval_limits;
  LainEvalValue eval_result;
  uint32_t eval_status;
  L1Diagnostic eval_diagnostic;
  bool eval_ready;
  uint32_t eval_requests;
};

/* --- 能力名 ---------------------------------------------------------------
 *   名字                          参数                      结果
 *   lain_meta_source_count        ()                        count
 *   lain_meta_source_data         (index)                   文本地址
 *   lain_meta_source_length       (index)                   字节数
 *   lain_meta_source_path_data    (index)                   路径地址
 *   lain_meta_emit_reset          ()                        0
 *   lain_meta_emit_write          (addr, length)            0
 *   lain_meta_emit_data           ()                        文本地址
 *   lain_meta_emit_length         ()                        字节数
 *   lain_meta_fail                (code)                    0
 *   lain_meta_eval_request        (text, len, entry, len)  结果位模式
 *   lain_meta_eval_status         ()                        诊断码
 *   lain_meta_eval_kind           ()                        物理类型类别
 *   lain_meta_eval_width          ()                        位宽
 * 每个的第一个参数都是 host 地址。
 * ------------------------------------------------------------------------- */

/* 先验证首参数存在；这里只保护参数缺失，不能证明非零地址的宿主身份。 */
#define HOST_OF(args) ((args) && count > 0 \
    ? (LainMetaHost *)(uintptr_t)(args)[0] : NULL)

static void host_set_status(LainMetaHost *host, uint32_t code) {
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

void lainmeta_host_set_eval_limits(LainMetaHost *host,
                                   const LainEvalLimits *limits) {
  if (host && limits) host->eval_limits = *limits;
}

uint32_t lainmeta_host_eval_requests(const LainMetaHost *host) {
  return host ? host->eval_requests : 0;
}

int lainmeta_host_eval_diagnostic(const LainMetaHost *host, L1Diagnostic *out) {
  if (!host || !out || !host->eval_requests || !host->eval_status) return 0;
  *out = host->eval_diagnostic;
  out->code = (int)host->eval_status;
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
  host->eval_limits.max_call_depth = 64;
  host->eval_limits.stack_bytes = 4096;
  host->eval_limits.fuel = 1000000;
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
  if (size > 0x40000000u) size = 0x40000000u; /* 上限 1 GiB，别拿坏输入去要内存 */
  return (uint32_t)size;
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
  }
  if (size_out) *size_out = host->scratch_size;
  return host->scratch;
}

void lainmeta_host_free(LainMetaHost *host) {
  uint32_t i;
  if (!host) return;
  for (i = 0; i < host->source_count; i++) {
    lainmeta_tree_free(host->sources[i].tree);
    free((void *)host->sources[i].path);
  }
  free(host->sources);
  free(host->out);
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
  if (!host || !text) return 1;
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
  host->sources[host->source_count].path = copy;
  host->sources[host->source_count].text = text;
  host->sources[host->source_count].length = length;
  host->sources[host->source_count].tree = NULL;
  host->source_count++;
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

static uint32_t cap_source_count(const uint64_t *args, uint32_t count,
                                 uint64_t *out) {
  LainMetaHost *host = HOST_OF(args);
  if (!host || count < 1) return LAINMETA_ERR_OOM;
  if (out) *out = host->source_count;
  return 0;
}

static uint32_t cap_source_data(const uint64_t *args, uint32_t count,
                                uint64_t *out) {
  LainMetaHost *host = HOST_OF(args);
  uint64_t index;
  if (!host || count < 2) return LAINMETA_ERR_OOM;
  index = args[1];
  if (index >= host->source_count) {
    host_set_status(host, LAINMETA_ERR_NO_SOURCE);
    return LAINMETA_ERR_NO_SOURCE;
  }
  if (out) *out = (uint64_t)(uintptr_t)host->sources[index].text;
  return 0;
}

static uint32_t cap_source_length(const uint64_t *args, uint32_t count,
                                  uint64_t *out) {
  LainMetaHost *host = HOST_OF(args);
  uint64_t index;
  if (!host || count < 2) return LAINMETA_ERR_OOM;
  index = args[1];
  if (index >= host->source_count) {
    host_set_status(host, LAINMETA_ERR_NO_SOURCE);
    return LAINMETA_ERR_NO_SOURCE;
  }
  if (out) *out = host->sources[index].length;
  return 0;
}

static uint32_t cap_source_path_data(const uint64_t *args, uint32_t count,
                                     uint64_t *out) {
  LainMetaHost *host = HOST_OF(args);
  uint64_t index;
  if (!host || count < 2) return LAINMETA_ERR_OOM;
  index = args[1];
  if (index >= host->source_count) {
    host_set_status(host, LAINMETA_ERR_NO_SOURCE);
    return LAINMETA_ERR_NO_SOURCE;
  }
  if (out) *out = (uint64_t)(uintptr_t)host->sources[index].path;
  return 0;
}

static uint32_t cap_emit_reset(const uint64_t *args, uint32_t count,
                               uint64_t *out) {
  LainMetaHost *host = HOST_OF(args);
  if (!host || count < 1) return LAINMETA_ERR_OOM;
  host->out_length = 0;
  if (host->out) host->out[0] = '\0';
  if (out) *out = 0;
  return 0;
}

static uint32_t cap_emit_write(const uint64_t *args, uint32_t count,
                               uint64_t *out) {
  LainMetaHost *host = HOST_OF(args);
  const char *bytes;
  uint64_t length;
  if (!host || count < 3) return LAINMETA_ERR_OOM;
  bytes = (const char *)(uintptr_t)args[1];
  length = args[2];
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

static uint32_t cap_emit_data(const uint64_t *args, uint32_t count,
                              uint64_t *out) {
  LainMetaHost *host = HOST_OF(args);
  if (!host || count < 1) return LAINMETA_ERR_OOM;
  if (out) *out = (uint64_t)(uintptr_t)(host->out ? host->out : "");
  return 0;
}

static uint32_t cap_emit_length(const uint64_t *args, uint32_t count,
                                uint64_t *out) {
  LainMetaHost *host = HOST_OF(args);
  if (!host || count < 1) return LAINMETA_ERR_OOM;
  if (out) *out = host->out_length;
  return 0;
}

static uint32_t cap_fail(const uint64_t *args, uint32_t count, uint64_t *out) {
  LainMetaHost *host = HOST_OF(args);
  if (!host || count < 2) return 1;
  host_set_status(host, (uint32_t)args[1]);
  if (out) *out = 0;
  return 0;
}

/* 未知位置使用全一值，合法的文件零和偏移零仍可表示。 */
static uint32_t cap_diagnostic_field(const uint64_t *args, uint32_t count,
                                      uint64_t *out) {
  LainMetaHost *host;
  uint64_t value;
  if (!args || count < 2 || !HOST_OF(args)) return LAINMETA_ERR_DENIED;
  host = HOST_OF(args);
  switch (args[1]) {
    case 1: value = host->status; break;
    case 2: value = host->status ? host->diagnostic_source : UINT64_MAX; break;
    case 3: value = host->status ? host->diagnostic_offset : UINT64_MAX; break;
    default: return LAINMETA_ERR_DENIED;
  }
  if (out) *out = value;
  return 0;
}

static uint32_t cap_fail_at(const uint64_t *args, uint32_t count, uint64_t *out) {
  LainMetaHost *host;
  if (!args || count < 4 || !HOST_OF(args)) return LAINMETA_ERR_DENIED;
  host = HOST_OF(args);
  if (out) *out = 0;
  if (!args[1] || args[1] > UINT32_MAX || args[2] >= host->source_count ||
      args[3] > host->sources[args[2]].length) {
    host_set_status(host, LAINMETA_ERR_DENIED);
    return 0;
  }
  host_set_status(host, (uint32_t)args[1]);
  host->diagnostic_source = args[2];
  host->diagnostic_offset = args[3];
  if (out) *out = 1;
  return 0;
}

static uint32_t cap_status(const uint64_t *args, uint32_t count, uint64_t *out) {
  if (!args || count < 1 || !HOST_OF(args)) return LAINMETA_ERR_DENIED;
  if (out) *out = HOST_OF(args)->status;
  return 0;
}

/* 描述符是公开的八个 64 位字。先验证、预扣并扩容，最后才提交新条目。 */
static uint32_t cap_type_publish(const uint64_t *args, uint32_t count,
                                 uint64_t *out) {
  LainMetaHost *host;
  LainMetaTypeInfo info, *grown;
  uint32_t i, next;
  uint64_t bytes;
  if (out) *out = 0;
  if (count < 2 || !args) return LAINMETA_ERR_DENIED;
  host = HOST_OF(args);
  if (!host) return LAINMETA_ERR_DENIED;
  if (!host_range_readable(host, (uintptr_t)args[1], sizeof(info))) {
    host_set_status(host, LAINMETA_ERR_DENIED);
    return 0;
  }
  memcpy(&info, (const void *)(uintptr_t)args[1], sizeof(info));
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

static uint32_t cap_scratch_report(const uint64_t *args, uint32_t count,
                                   uint64_t *out) {
  LainMetaHost *host;
  if (!args || count < 2 || !HOST_OF(args)) return LAINMETA_ERR_DENIED;
  host = HOST_OF(args);
  if (out) *out = 0;
  /* 峰值是暂存区内的最高使用位置；同一宿主的报告只能增长。 */
  if (args[1] > host->scratch_size ||
      (host->scratch_peak_ready && args[1] < host->scratch_peak)) {
    host_set_status(host, LAINMETA_ERR_RESOURCE_PUBLISH);
    return 0;
  }
  host->scratch_peak = args[1];
  host->scratch_peak_ready = true;
  if (out) *out = 1;
  return 0;
}

static uint32_t cap_scratch_data(const uint64_t *args, uint32_t count,
                                 uint64_t *out) {
  LainMetaHost *host = HOST_OF(args);
  if (!host || count < 1) return LAINMETA_ERR_OOM;
  if (out) *out = (uint64_t)(uintptr_t)host->scratch;
  return 0;
}

static uint32_t cap_scratch_size(const uint64_t *args, uint32_t count,
                                 uint64_t *out) {
  LainMetaHost *host = HOST_OF(args);
  if (!host || count < 1) return LAINMETA_ERR_OOM;
  if (out) *out = host->scratch_size;
  return 0;
}

/* 先核对 Meta 地址空间的两段输入，再拷入本次请求私有文本。
 * Eval 在独立 VSpace 中运行，只拿驱动显式给的能力与预算。 */
static uint32_t cap_eval_request(const uint64_t *args, uint32_t count,
                                 uint64_t *out) {
  LainMetaHost *host;
  const char *text, *entry;
  char *text_copy = NULL, *entry_copy = NULL;
  LainEvalValue value;
  L1Diagnostic diag;
  uint64_t text_len, entry_len;
  if (count < 5) return LAINMETA_ERR_DENIED;
  host = HOST_OF(args);
  if (!host) return LAINMETA_ERR_DENIED;
  host->eval_requests++;
  host->eval_ready = false;
  host->eval_status = LAINMETA_ERR_DENIED;
  memset(&host->eval_diagnostic, 0, sizeof(host->eval_diagnostic));
  if (out) *out = 0;
  text = (const char *)(uintptr_t)args[1];
  text_len = args[2];
  entry = (const char *)(uintptr_t)args[3];
  entry_len = args[4];
  if (!text || !entry || !text_len || !entry_len ||
      text_len > 1024u * 1024u || entry_len > 255u ||
      !host_range_readable(host, (uintptr_t)text, text_len) ||
      !host_range_readable(host, (uintptr_t)entry, entry_len))
    return 0;
  if (memchr(text, 0, (size_t)text_len) ||
      memchr(entry, 0, (size_t)entry_len)) {
    host->eval_status = 9330;
    return 0;
  }
  text_copy = (char *)malloc((size_t)text_len + 1u);
  entry_copy = (char *)malloc((size_t)entry_len + 1u);
  if (!text_copy || !entry_copy) {
    host->eval_status = LAINMETA_ERR_OOM;
    goto cleanup_eval;
  }
  memcpy(text_copy, text, (size_t)text_len);
  text_copy[text_len] = '\0';
  memcpy(entry_copy, entry, (size_t)entry_len);
  entry_copy[entry_len] = '\0';
  memset(&diag, 0, sizeof(diag));
  if (!laineval_text(text_copy, entry_copy, &host->eval_limits, &value, &diag)) {
    host->eval_status = diag.code ? (uint32_t)diag.code : 9330;
    host->eval_diagnostic = diag;
    goto cleanup_eval;
  }
  host->eval_result = value;
  host->eval_ready = true;
  host->eval_status = 0;
  if (out) *out = value.bits;
cleanup_eval:
  free(entry_copy);
  free(text_copy);
  return 0;
}

static uint32_t cap_eval_status(const uint64_t *args, uint32_t count,
                                uint64_t *out) {
  if (count < 1 || !HOST_OF(args)) return LAINMETA_ERR_DENIED;
  if (out) *out = HOST_OF(args)->eval_status;
  return 0;
}

static uint32_t cap_eval_kind(const uint64_t *args, uint32_t count,
                              uint64_t *out) {
  if (count < 1 || !HOST_OF(args)) return LAINMETA_ERR_DENIED;
  if (out) *out = HOST_OF(args)->eval_ready ? HOST_OF(args)->eval_result.kind : 0;
  return 0;
}

/* 行列属于本次生成的 LAINIR 请求，不代表 Lain 源文件位置。 */
static uint32_t cap_eval_diagnostic_field(const uint64_t *args, uint32_t count,
                                          uint64_t *out) {
  LainMetaHost *host;
  uint64_t value;
  if (!args || count < 2 || !HOST_OF(args)) return LAINMETA_ERR_DENIED;
  host = HOST_OF(args);
  switch (args[1]) {
    case 1: value = host->eval_status; break;
    case 2: value = host->eval_diagnostic.line; break;
    case 3: value = host->eval_diagnostic.column; break;
    default: return LAINMETA_ERR_DENIED;
  }
  if (out) *out = value;
  return 0;
}

static uint32_t cap_eval_width(const uint64_t *args, uint32_t count,
                               uint64_t *out) {
  if (count < 1 || !HOST_OF(args)) return LAINMETA_ERR_DENIED;
  if (out) *out = HOST_OF(args)->eval_ready ? HOST_OF(args)->eval_result.width : 0;
  return 0;
}

/* 宿主句柄的高 32 位是源文件编号 + 1，低 32 位是该树内部句柄。
 * 所有访问先核对归属，不把句柄当指针解引用。 */
static uint64_t tree_encode(uint32_t source, uint64_t local) {
  return ((uint64_t)source + 1u) << 32 | local;
}

static LainMetaTree *tree_source(LainMetaHost *host, uint32_t source) {
  uint32_t offset = 0;
  uint64_t rejected_before;
  LainMetaSource *item;
  if (!host || source >= host->source_count) {
    if (host) host_set_status(host, LAINMETA_ERR_NO_SOURCE);
    return NULL;
  }
  item = &host->sources[source];
  if (!item->tree) {
    rejected_before = host->quota ? host->quota->rejected : 0;
    item->tree = lainmeta_tree_parse_with_quota(
        source, item->text, item->length, &offset, host->quota);
    if (!item->tree) {
      host_set_status(host, host->quota && host->quota->rejected > rejected_before
                         ? (uint32_t)LAINVM_QUOTA_TRAP
                         : LAINMETA_ERR_TREE_PARSE);
      if (host->status == LAINMETA_ERR_TREE_PARSE) {
        host->diagnostic_source = source;
        host->diagnostic_offset = offset;
      }
    }
  }
  return item->tree;
}

const LainMetaTree *lainmeta_host_tree(LainMetaHost *host, uint32_t source) {
  return tree_source(host, source);
}

static LainMetaTree *tree_decode(LainMetaHost *host, uint64_t handle,
                                 uint64_t *local_out) {
  uint64_t source = handle >> 32;
  LainMetaTree *tree;
  LainMetaTreeNode node;
  if (!source || source - 1u >= host->source_count || !(uint32_t)handle) {
    host_set_status(host, LAINMETA_ERR_TREE_HANDLE);
    return NULL;
  }
  tree = tree_source(host, (uint32_t)(source - 1u));
  if (!tree) return NULL;
  if (!lainmeta_tree_node(tree, (uint32_t)handle, &node)) {
    host_set_status(host, LAINMETA_ERR_TREE_HANDLE);
    return NULL;
  }
  if (local_out) *local_out = (uint32_t)handle;
  return tree;
}

static uint64_t tree_quota_rejected(const LainMetaHost *host) {
  return host->quota ? host->quota->rejected : 0;
}

/* 来源链只指向更早创建的节点；生成节点的 start 不代表原始源码位置。 */
static void tree_diagnostic_origin(LainMetaHost *host, LainMetaTree *tree,
                                    uint64_t local) {
  LainMetaTreeNode node;
  while (local && lainmeta_tree_node(tree, local, &node)) {
    if (node.origin) {
      if (node.origin >= local) return;
      local = node.origin;
      continue;
    }
    if (node.source_index < host->source_count &&
        node.start <= host->sources[node.source_index].length) {
      host->diagnostic_source = node.source_index;
      host->diagnostic_offset = node.start;
    }
    return;
  }
}

static void tree_edit_failed(LainMetaHost *host, uint64_t rejected_before,
                             LainMetaTree *tree, uint64_t origin) {
  host_set_status(host, tree_quota_rejected(host) > rejected_before
                     ? (uint32_t)LAINVM_QUOTA_TRAP
                     : LAINMETA_ERR_TREE_EDIT);
  tree_diagnostic_origin(host, tree, origin);
}

static uint32_t cap_tree_root(const uint64_t *args, uint32_t count,
                              uint64_t *out) {
  LainMetaHost *host;
  LainMetaTree *tree;
  if (count < 2 || !HOST_OF(args)) return LAINMETA_ERR_DENIED;
  host = HOST_OF(args);
  if (out) *out = 0;
  if (args[1] > UINT32_MAX) {
    host_set_status(host, LAINMETA_ERR_NO_SOURCE);
    return 0;
  }
  tree = tree_source(host, (uint32_t)args[1]);
  if (tree && out) *out = tree_encode((uint32_t)args[1], lainmeta_tree_root(tree));
  return 0;
}

/* 字段编号固定为公开接口：1 kind, 2 start, 3 length, 4 source,
 * 5 origin, 6 delimiter, 7 child_count。 */
static uint32_t cap_tree_field(const uint64_t *args, uint32_t count,
                               uint64_t *out) {
  LainMetaHost *host;
  LainMetaTree *tree;
  LainMetaTreeNode node;
  uint64_t local;
  if (count < 3 || !HOST_OF(args)) return LAINMETA_ERR_DENIED;
  host = HOST_OF(args);
  if (out) *out = 0;
  tree = tree_decode(host, args[1], &local);
  if (!tree || !lainmeta_tree_node(tree, local, &node)) return 0;
  if (!out) return 0;
  switch (args[2]) {
    case 1: *out = node.kind; break;
    case 2: *out = node.start; break;
    case 3: *out = node.length; break;
    case 4: *out = node.source_index; break;
    case 5: *out = node.origin ? tree_encode((uint32_t)(args[1] >> 32) - 1u,
                                               node.origin) : 0; break;
    case 6: *out = node.delimiter; break;
    case 7: *out = node.child_count; break;
    default: host_set_status(host, LAINMETA_ERR_TREE_HANDLE); break;
  }
  return 0;
}

static uint32_t cap_tree_child(const uint64_t *args, uint32_t count,
                               uint64_t *out) {
  LainMetaHost *host;
  LainMetaTree *tree;
  uint64_t local, child;
  if (count < 3 || !HOST_OF(args)) return LAINMETA_ERR_DENIED;
  host = HOST_OF(args);
  if (out) *out = 0;
  tree = tree_decode(host, args[1], &local);
  if (!tree || args[2] > UINT32_MAX) return 0;
  child = lainmeta_tree_child(tree, local, (uint32_t)args[2]);
  if (!child) host_set_status(host, LAINMETA_ERR_TREE_HANDLE);
  else if (out) *out = tree_encode((uint32_t)(args[1] >> 32) - 1u, child);
  return 0;
}

static uint32_t cap_tree_text_byte(const uint64_t *args, uint32_t count,
                                   uint64_t *out) {
  LainMetaHost *host;
  LainMetaTree *tree;
  const char *text;
  uint64_t local;
  uint32_t length;
  if (count < 3 || !HOST_OF(args)) return LAINMETA_ERR_DENIED;
  host = HOST_OF(args);
  if (out) *out = 0;
  tree = tree_decode(host, args[1], &local);
  if (!tree) return 0;
  text = lainmeta_tree_text(tree, local, &length);
  if (!text || args[2] >= length) {
    host_set_status(host, LAINMETA_ERR_TREE_HANDLE);
    return 0;
  }
  if (out) *out = (unsigned char)text[args[2]];
  return 0;
}

static uint32_t cap_tree_make_token(const uint64_t *args, uint32_t count,
                                    uint64_t *out) {
  LainMetaHost *host;
  LainMetaTree *tree;
  uint64_t origin = 0, local, rejected_before;
  if (count < 5 || !HOST_OF(args)) return LAINMETA_ERR_DENIED;
  host = HOST_OF(args);
  if (out) *out = 0;
  if (args[1] > UINT32_MAX || args[3] > UINT32_MAX ||
      !host_range_readable(host, (uintptr_t)args[2], args[3])) {
    host_set_status(host, LAINMETA_ERR_DENIED);
    return 0;
  }
  tree = tree_source(host, (uint32_t)args[1]);
  if (!tree) return 0;
  if (args[4]) {
    if ((args[4] >> 32) != args[1] + 1u ||
        tree_decode(host, args[4], &origin) != tree) {
      host_set_status(host, LAINMETA_ERR_TREE_EDIT);
      return 0;
    }
  }
  rejected_before = tree_quota_rejected(host);
  local = lainmeta_tree_make_token(tree, (const char *)(uintptr_t)args[2],
                                    (uint32_t)args[3], origin);
  if (!local) tree_edit_failed(host, rejected_before, tree, origin);
  else if (out) *out = tree_encode((uint32_t)args[1], local);
  return 0;
}

static uint32_t cap_tree_replace(const uint64_t *args, uint32_t count,
                                 uint64_t *out) {
  LainMetaHost *host;
  LainMetaTree *tree;
  uint64_t group, replacement, local, rejected_before;
  if (count < 4 || !HOST_OF(args)) return LAINMETA_ERR_DENIED;
  host = HOST_OF(args);
  if (out) *out = 0;
  tree = tree_decode(host, args[1], &group);
  if (!tree) return 0;
  if (args[2] > UINT32_MAX ||
      (args[1] >> 32) != (args[3] >> 32) ||
      tree_decode(host, args[3], &replacement) != tree) {
    host_set_status(host, LAINMETA_ERR_TREE_EDIT);
    tree_diagnostic_origin(host, tree, group);
    return 0;
  }
  rejected_before = tree_quota_rejected(host);
  local = lainmeta_tree_replace_child(tree, group, (uint32_t)args[2], replacement);
  if (!local) tree_edit_failed(host, rejected_before, tree, group);
  else if (out) *out = tree_encode((uint32_t)(args[1] >> 32) - 1u, local);
  return 0;
}

static uint32_t cap_tree_make_group(const uint64_t *args, uint32_t count,
                                    uint64_t *out) {
  LainMetaHost *host;
  LainMetaTree *tree;
  const uint64_t *encoded;
  uint64_t *children = NULL, origin = 0, local, child_bytes, rejected_before;
  uint32_t i, child_count;
  if (count < 6 || !HOST_OF(args)) return LAINMETA_ERR_DENIED;
  host = HOST_OF(args);
  if (out) *out = 0;
  if (args[1] > UINT32_MAX || args[2] > 255 || args[4] > 65536 ||
      !host_range_readable(host, (uintptr_t)args[3], args[4] * 8u)) {
    host_set_status(host, LAINMETA_ERR_DENIED);
    return 0;
  }
  tree = tree_source(host, (uint32_t)args[1]);
  if (!tree) return 0;
  if (args[5] &&
      ((args[5] >> 32) != args[1] + 1u ||
       tree_decode(host, args[5], &origin) != tree)) {
    host_set_status(host, LAINMETA_ERR_TREE_EDIT);
    return 0;
  }
  child_count = (uint32_t)args[4];
  child_bytes = (uint64_t)sizeof(*children) * child_count;
  encoded = (const uint64_t *)(uintptr_t)args[3];
  if (child_count) {
    if (lainvm_quota_charge(host->quota, child_bytes) != 0) {
      host_set_status(host, (uint32_t)LAINVM_QUOTA_TRAP);
      tree_diagnostic_origin(host, tree, origin);
      return 0;
    }
    children = (uint64_t *)malloc((size_t)child_bytes);
    if (!children) {
      (void)lainvm_quota_release(host->quota, child_bytes);
      host_set_status(host, LAINMETA_ERR_OOM);
      tree_diagnostic_origin(host, tree, origin);
      return 0;
    }
  }
  for (i = 0; i < child_count; i++) {
    uint64_t handle;
    memcpy(&handle, (const unsigned char *)encoded + (size_t)i * 8u, 8u);
    if ((handle >> 32) != args[1] + 1u ||
        tree_decode(host, handle, &children[i]) != tree) {
      host_set_status(host, LAINMETA_ERR_TREE_EDIT);
      tree_diagnostic_origin(host, tree, origin);
      free(children);
      (void)lainvm_quota_release(host->quota, child_bytes);
      return 0;
    }
  }
  rejected_before = tree_quota_rejected(host);
  local = lainmeta_tree_make_group(tree, (unsigned char)args[2], children,
                                    child_count, origin);
  free(children);
  if (child_count) (void)lainvm_quota_release(host->quota, child_bytes);
  if (!local) tree_edit_failed(host, rejected_before, tree, origin);
  else if (out) *out = tree_encode((uint32_t)args[1], local);
  return 0;
}

typedef struct {
  const char *name;
  LainVmHostFn fn;
} MetaCapability;

static const MetaCapability k_capabilities[] = {
    {"lain_meta_source_count", cap_source_count},
    {"lain_meta_source_data", cap_source_data},
    {"lain_meta_source_length", cap_source_length},
    {"lain_meta_source_path_data", cap_source_path_data},
    {"lain_meta_emit_reset", cap_emit_reset},
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
    {"lain_meta_eval_request", cap_eval_request},
    {"lain_meta_eval_status", cap_eval_status},
    {"lain_meta_eval_diagnostic_field", cap_eval_diagnostic_field},
    {"lain_meta_eval_kind", cap_eval_kind},
    {"lain_meta_eval_width", cap_eval_width},
    {"lain_meta_tree_root", cap_tree_root},
    {"lain_meta_tree_field", cap_tree_field},
    {"lain_meta_tree_child", cap_tree_child},
    {"lain_meta_tree_text_byte", cap_tree_text_byte},
    {"lain_meta_tree_make_token", cap_tree_make_token},
    {"lain_meta_tree_make_group", cap_tree_make_group},
    {"lain_meta_tree_replace_child", cap_tree_replace},
};

int lainmeta_host_register(LainMetaHost *host, LainVmCaps *caps) {
  size_t i;
  if (!host || !caps) return 1;
  for (i = 0; i < sizeof(k_capabilities) / sizeof(k_capabilities[0]); i++) {
    if (lainvm_caps_add(caps, k_capabilities[i].name, LAINVM_CAP_FUNCTION,
                        k_capabilities[i].fn) != 0)
      return 2;
  }
  return 0;
}
