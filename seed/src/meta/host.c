/* lainmeta/host.h 的实现。
 *
 * 每个宿主函数都是 LainVmHostFn：拿原始 64 位值，返回 0 = 成功。
 * args[0] 永远是 host 自己的地址——Meta 从 initialize 收到它，之后每次调用
 * 都原样传回来。没有全局，也没有 user_data。
 */
#include "lainmeta/host.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *path;
  const char *text;
  uint32_t length;
} LainMetaSource;

struct LainMetaHost {
  LainMetaSource *sources;
  uint32_t source_count;
  uint32_t source_cap;
  char *out;
  uint32_t out_length;
  uint32_t out_cap;
  uint32_t status;
  /* Meta 的可写暂存：表格建在这里。驱动负责把这块范围登记进 VSpace。 */
  unsigned char *scratch;
  uint32_t scratch_size;
  /* 这份宿主服务被授权到哪个地址空间（驱动 attach；NULL = 没授权）。
   * 能力表里没有 user_data，所以授权随**宿主对象**走，且由驱动显式给。 */
  LainVmSpace *space;
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
 * 每个的第一个参数都是 host 地址。
 * ------------------------------------------------------------------------- */

#define HOST_OF(args) ((LainMetaHost *)(uintptr_t)(args)[0])

void lainmeta_host_attach_space(LainMetaHost *host, LainVmSpace *space) {
  if (!host) return;
  host->space = space;
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
  uint32_t need = host->out_length + extra + 1u;
  uint32_t next;
  char *grown;
  if (need <= host->out_cap) return 0;
  next = host->out_cap ? host->out_cap : 256u;
  while (next < need) next *= 2u;
  grown = (char *)realloc(host->out, next);
  if (!grown) {
    host->status = LAINMETA_ERR_OOM;
    return 1;
  }
  host->out = grown;
  host->out_cap = next;
  return 0;
}

LainMetaHost *lainmeta_host_new(void) {
  LainMetaHost *host = (LainMetaHost *)calloc(1, sizeof(LainMetaHost));
  if (!host) return NULL;
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
    host->scratch_size = decide_scratch_size(host);
    host->scratch = (unsigned char *)calloc(1, host->scratch_size);
    if (!host->scratch) {
      host->scratch_size = 0;
      if (size_out) *size_out = 0;
      return NULL;
    }
  }
  if (size_out) *size_out = host->scratch_size;
  return host->scratch;
}

void lainmeta_host_free(LainMetaHost *host) {
  uint32_t i;
  if (!host) return;
  for (i = 0; i < host->source_count; i++) free((void *)host->sources[i].path);
  free(host->sources);
  free(host->out);
  free(host->scratch);
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

uint32_t lainmeta_host_status(const LainMetaHost *host) {
  return host ? host->status : LAINMETA_ERR_OOM;
}

void lainmeta_host_clear_status(LainMetaHost *host) {
  if (host) host->status = LAINMETA_OK;
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
    host->status = LAINMETA_ERR_NO_SOURCE;
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
    host->status = LAINMETA_ERR_NO_SOURCE;
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
    host->status = LAINMETA_ERR_NO_SOURCE;
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
  host->status = (uint32_t)args[1];
  if (out) *out = 0;
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
    {"lain_meta_scratch_data", cap_scratch_data},
    {"lain_meta_scratch_size", cap_scratch_size},
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
