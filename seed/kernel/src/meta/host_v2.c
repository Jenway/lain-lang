/* Lain-Meta v2 宿主的实现。
 *
 * 只有一种交付方式：guest 拿到 u32 句柄，字段值由宿主读出来再返回。Arena 字节、
 * C 结构体镜像、暂存区偏移都不进 ABI。四条通道各自的存储归属：
 *   源码文本    驱动持有，宿主按 [text, text+length) 只读映射给 guest；
 *   符号池      符号表持有，一次分配不再扩容，整池只读映射；
 *   AstOut 区   ast_out 持有，只读映射（节点只能经原语追加，guest 从不直接写）；
 *   工作区      宿主向 VSpace 要的一块 OWNED 区，meta_alloc 线性切分。
 */
#include "lain/meta/host_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast_out.h"
#include "lain/ir/build.h"
#include "lain/ir/verify.h"
#include "lain/meta/ast.h"
#include "lain/meta/tree.h"
#include "lain/text/print.h"

/* 段号：源码字节与 AstIn 节点分居两段，二者都按源码序号编址。 */
#define V2_SEG_TEXT_BASE 16u
#define V2_SEG_AST_BASE 64u

#define V2_CLASS_SHIFT 28u
#define V2_INDEX_MASK 0x0FFFFFFFu
#define V2_AST_IN 1u
#define V2_AST_OUT 2u
#define V2_SYM 3u
#define V2_MODULE 4u
#define V2_REGION 5u

#define V2_REGION_MAX 1024u
#define V2_INST_MAX 4096u
#define V2_PROC_PARAM_MAX 16u
#define V2_PROC_MAX 256u
#define V2_DATA_MAX 256u
#define V2_FRAME_MAX 16u
#define V2_CASE_MAX 64u
#define V2_NAME_MAX 512u
#define V2_PATH_MAX 1024u

typedef struct {
  L1RegionParam params[V2_PROC_PARAM_MAX];
  uint32_t param_count;
  const L1Type *results[L1_MAX_RESULTS];
  uint32_t result_count;
  const L1Inst *insts[V2_INST_MAX];
  uint32_t inst_count;
} V2Frame;

typedef struct {
  bool open;
  const char *name;
  L1Param params[V2_PROC_PARAM_MAX];
  uint32_t param_count;
  const L1Type *results[L1_MAX_RESULTS];
  uint32_t result_count;
  bool is_extern;
  const char *link_name;
  V2Frame frames[V2_FRAME_MAX];
  uint32_t depth;
  const L1Inst *last;
  L1SwitchCase cases[V2_CASE_MAX];
  uint32_t case_count;
} V2Proc;

typedef struct {
  bool in_use;
  L1Builder *builder;
  const char *name;
  V2Proc proc;
  const L1Data *data[V2_DATA_MAX];
  uint32_t data_count;
  const L1Subroutine *subs[V2_PROC_MAX];
  uint32_t sub_count;
  const L1Region *regions[V2_REGION_MAX];
  uint32_t region_count;
  const L1Module *module;
  /* 操作数邮箱：下一条 #inst* 的实参。邮箱跟着模块走，同时有两个模块在构造时
   * 也不会互相踩。 */
  L1Operand args[L1_MAX_OPERANDS];
  uint32_t arg_count;
} V2Module;

typedef struct {
  char *path;
  const char *text;
  char *owned_text;
  uint32_t length;
  LainMetaTree *tree;
} V2Source;

struct LainMetaHostV2 {
  LainVmSpace *space;
  LainVmQuota *quota;
  LainMetaSymTab *syms;
  bool symbols_mapped;

  V2Source sources[LAINMETA_V2_MAX_SOURCES];
  uint32_t source_count;

  LainAstOutput *out;
  bool out_mapped;
  LainMetaSymId *out_syms;
  uint32_t out_index_cap;
  bool tx_open;
  uint64_t tx_id;
  uint32_t tx_mark;

  char *text;
  uint64_t text_length;
  uint64_t text_capacity;

  LainMetaV2Diag diags[LAINMETA_V2_MAX_DIAGS];
  uint32_t diag_count;

  V2Module mods[LAINMETA_V2_MAX_MODULES];

  uintptr_t workspace;
  uint64_t workspace_bytes;
  uint64_t workspace_used;

  LainExpandLimits expand_limits;
  LainMetaApplyLimits apply_limits;
};

/* --- 句柄 ---------------------------------------------------------------- */

static uint32_t handle_ast_in(uint32_t source, uint32_t slot) {
  return (V2_AST_IN << V2_CLASS_SHIFT) | ((source & 0xFFu) << 20) |
         (slot & 0xFFFFFu);
}
static uint32_t handle_ast_out(uint32_t index) {
  return (V2_AST_OUT << V2_CLASS_SHIFT) | (index & V2_INDEX_MASK);
}
static uint32_t handle_sym(LainMetaSymId sym) {
  return (V2_SYM << V2_CLASS_SHIFT) | (sym & V2_INDEX_MASK);
}
static uint32_t handle_module(uint32_t index) {
  return (V2_MODULE << V2_CLASS_SHIFT) | (index & V2_INDEX_MASK);
}
static uint32_t handle_region(uint32_t module, uint32_t index) {
  return (V2_REGION << V2_CLASS_SHIFT) | ((module & 0xFFu) << 20) |
         ((index + 1u) & 0xFFFFFu);
}

/* --- 诊断 ---------------------------------------------------------------- */

static void v2_diag(LainMetaHostV2 *h, uint32_t level, LainAstRef span,
                    const char *message) {
  LainMetaV2Diag *d;
  uint16_t segment = lain_ast_segment(span);
  if (h->diag_count == LAINMETA_V2_MAX_DIAGS) return;
  d = &h->diags[h->diag_count++];
  memset(d, 0, sizeof(*d));
  d->level = level;
  d->offset = lain_ast_offset(span);
  if (segment >= V2_SEG_TEXT_BASE && segment < V2_SEG_TEXT_BASE + 48u) {
    d->source = (uint32_t)(segment - V2_SEG_TEXT_BASE);
  } else {
    d->source = 0xFFFFFFFFu;
  }
  d->length = 0;
  if (message) {
    size_t n = strlen(message);
    if (n >= LAINMETA_V2_MAX_MESSAGE) n = LAINMETA_V2_MAX_MESSAGE - 1;
    memcpy(d->message, message, n);
  }
}

/* --- 源码 ---------------------------------------------------------------- */

static const LainAstArenaHeader *v2_arena_header(const LainMetaTree *tree) {
  LainAstArenaView view;
  if (!lainmeta_tree_arena(tree, &view)) return NULL;
  return (const LainAstArenaHeader *)view.data;
}

static bool v2_normalize(const char *in, char *out, size_t cap) {
  char buf[V2_PATH_MAX];
  size_t len = strlen(in);
  size_t i = 0, o = 0;
  uint32_t depth = 0;
  if (len + 1 > sizeof(buf) || cap < 4) return false;
  for (i = 0; i < len; i++) buf[i] = in[i] == '\\' ? '/' : in[i];
  buf[len] = 0;
  out[0] = 0;
  i = 0;
  if (len >= 2 && buf[1] == ':') {
    out[o++] = buf[0];
    out[o++] = ':';
    i = 2;
    if (i < len && buf[i] == '/') { out[o++] = '/'; i++; }
  } else if (len && buf[0] == '/') {
    out[o++] = '/';
    i = 1;
  }
  while (i < len) {
    size_t start, n;
    while (i < len && buf[i] == '/') i++;
    start = i;
    while (i < len && buf[i] != '/') i++;
    n = i - start;
    if (!n) break;
    if (n == 1 && buf[start] == '.') continue;
    if (n == 2 && buf[start] == '.' && buf[start + 1] == '.') {
      if (depth) {
        while (o && out[o - 1] != '/') o--;
        if (o) o--;
        depth--;
      }
      continue;
    }
    if (o && out[o - 1] != '/') {
      if (o + 1 >= cap) return false;
      out[o++] = '/';
    }
    if (o + n >= cap) return false;
    memcpy(out + o, buf + start, n);
    o += n;
    depth++;
  }
  out[o] = 0;
  return o != 0 || depth == 0;
}

static void v2_dirname(const char *path, char *out, size_t cap) {
  size_t n = strlen(path);
  while (n && path[n - 1] != '/') n--;
  if (!n) {
    out[0] = '.';
    out[1] = '/';
    out[2] = 0;
    return;
  }
  if (n >= cap) n = cap - 1;
  memcpy(out, path, n);
  out[n] = 0;
}

static char *v2_read_file(const char *path, uint32_t *length) {
  FILE *file = fopen(path, "rb");
  long size;
  char *buffer;
  if (!file) return NULL;
  if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
  size = ftell(file);
  if (size < 0 || size > 0x7FFFFFFF) { fclose(file); return NULL; }
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
  *length = (uint32_t)size;
  return buffer;
}

static bool v2_map_symbols(LainMetaHostV2 *h) {
  const unsigned char *pool;
  uint32_t capacity = 0, used = 0;
  if (h->symbols_mapped || !h->space || !h->syms) return h->symbols_mapped;
  pool = lainmeta_sym_pool(h->syms, &capacity, &used);
  if (!pool || !capacity) return false;
  (void)lainvm_space_map_external(h->space, (uintptr_t)pool, capacity, capacity,
                                  LAINVM_MEM_READ, 0);
  h->symbols_mapped = true;
  return true;
}

static bool v2_map_out(LainMetaHostV2 *h) {
  LainAstArenaView view;
  if (h->out_mapped || !h->space || !h->out) return h->out_mapped;
  if (!lain_ast_output_view(h->out, &view)) return false;
  (void)lainvm_space_map_external(h->space, (uintptr_t)view.data,
                                  view.capacity, view.capacity,
                                  LAINVM_MEM_READ, 0);
  h->out_mapped = true;
  return true;
}

static bool v2_map_text(LainMetaHostV2 *h, const char *text, uint32_t length) {
  if (!h->space || !text || !length) return false;
  (void)lainvm_space_map_external(h->space, (uintptr_t)text, length, length,
                                  LAINVM_MEM_READ, 0);
  return true;
}

static bool v2_add_source(LainMetaHostV2 *h, const char *path, const char *text,
                          char *owned, uint32_t length, uint32_t *index_out,
                          LainMetaV2Diag *error) {
  V2Source *source;
  LainMetaTreeStatus status = LAINMETA_TREE_OK;
  uint32_t offset = 0;
  uint32_t index;
  if (h->source_count == LAINMETA_V2_MAX_SOURCES) return false;
  index = h->source_count++;
  source = &h->sources[index];
  memset(source, 0, sizeof(*source));
  if (path) {
    size_t n = strlen(path);
    source->path = (char *)malloc(n + 1);
    if (!source->path) { h->source_count--; return false; }
    memcpy(source->path, path, n + 1);
  }
  source->text = text;
  source->owned_text = owned;
  source->length = length;
  source->tree = lainmeta_tree_parse_symbols(index, text, length, h->syms,
                                            &offset, h->quota, &status);
  if (!source->tree) {
    if (error) {
      memset(error, 0, sizeof(*error));
      error->source = index;
      error->offset = offset;
      error->level = (uint32_t)status;
      snprintf(error->message, LAINMETA_V2_MAX_MESSAGE,
               status == LAINMETA_TREE_ERR_SYNTAX ? "syntax error at offset %u"
               : status == LAINMETA_TREE_ERR_QUOTA ? "quota exhausted"
               : status == LAINMETA_TREE_ERR_ALLOC ? "allocation failed"
                                                   : "internal parse failure",
               offset);
    }
    free(source->path);
    source->path = NULL;
    h->source_count--;
    free(owned);
    return false;
  }
  (void)lainmeta_tree_set_segment(source->tree, (uint16_t)(V2_SEG_AST_BASE + index),
                                  (uint16_t)(V2_SEG_TEXT_BASE + index));
  (void)v2_map_text(h, text, length);
  (void)v2_map_symbols(h);
  if (index_out) *index_out = index;
  return true;
}

/* --- 节点寻址 ------------------------------------------------------------ */

static uint32_t v2_node(LainMetaHostV2 *h, uint32_t handle, LainAstNode *out,
                        uint32_t *source_out, uint32_t *index_out) {
  uint32_t cls = handle >> V2_CLASS_SHIFT;
  if (cls == V2_AST_IN) {
    uint32_t source = (handle >> 20) & 0xFFu;
    uint32_t slot = handle & 0xFFFFFu;
    if (source >= h->source_count) return LAINMETA_V2_ERR_HANDLE;
    if (!lainmeta_tree_node(h->sources[source].tree, slot, out))
      return LAINMETA_V2_ERR_HANDLE;
    if (source_out) *source_out = source;
    if (index_out) *index_out = slot;
    return 0;
  }
  if (cls == V2_AST_OUT) {
    uint32_t index = handle & V2_INDEX_MASK;
    LainAstRef ref;
    if (!h->out) return LAINMETA_V2_ERR_STATE;
    if (index >= lain_ast_output_node_count(h->out))
      return LAINMETA_V2_ERR_HANDLE;
    ref = lain_ast_ref(LAIN_AST_OUT,
                       LAIN_AST_HEADER_SIZE +
                           (uint64_t)index * LAIN_AST_NODE_STRIDE);
    if (lain_ast_output_node(h->out, ref, out) != 0)
      return LAINMETA_V2_ERR_HANDLE;
    if (source_out) *source_out = 0;
    if (index_out) *index_out = index;
    return 0;
  }
  return LAINMETA_V2_ERR_HANDLE;
}

static LainAstRef v2_ref_of(LainMetaHostV2 *h, uint32_t handle, bool *ok) {
  uint32_t cls = handle >> V2_CLASS_SHIFT;
  *ok = true;
  if (cls == V2_AST_IN) {
    uint32_t source = (handle >> 20) & 0xFFu;
    uint32_t slot = handle & 0xFFFFFu;
    const LainAstArenaHeader *header;
    if (source >= h->source_count) { *ok = false; return 0; }
    header = v2_arena_header(h->sources[source].tree);
    if (!header) { *ok = false; return 0; }
    return lain_ast_ref((uint16_t)(V2_SEG_AST_BASE + source),
                        lain_ast_node_offset(header, slot));
  }
  if (cls == V2_AST_OUT) {
    return lain_ast_ref(LAIN_AST_OUT,
                        LAIN_AST_HEADER_SIZE +
                            (uint64_t)(handle & V2_INDEX_MASK) *
                                LAIN_AST_NODE_STRIDE);
  }
  *ok = false;
  return 0;
}

static uint32_t v2_handle_of_ref(LainMetaHostV2 *h, LainAstRef ref) {
  uint16_t segment = lain_ast_segment(ref);
  uint64_t offset = lain_ast_offset(ref);
  if (segment == LAIN_AST_OUT) {
    if (offset < LAIN_AST_HEADER_SIZE) return 0;
    return handle_ast_out((uint32_t)((offset - LAIN_AST_HEADER_SIZE) /
                                     LAIN_AST_NODE_STRIDE));
  }
  if (segment >= V2_SEG_AST_BASE &&
      segment < V2_SEG_AST_BASE + LAINMETA_V2_MAX_SOURCES) {
    uint32_t source = (uint32_t)(segment - V2_SEG_AST_BASE);
    const LainAstArenaHeader *header;
    if (source >= h->source_count) return 0;
    header = v2_arena_header(h->sources[source].tree);
    if (!header || offset < header->nodes_offset) return 0;
    (void)h;
    return handle_ast_in(
        source, (uint32_t)((offset - header->nodes_offset) /
                           LAIN_AST_NODE_STRIDE));
  }
  return 0;
}

/* ast_out 的解析回调：把它构造期间遇到的一切引用翻译成节点。 */
static uint32_t v2_resolve(void *context, LainAstRef ref, LainAstNode *out) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)context;
  uint16_t segment = lain_ast_segment(ref);
  uint64_t offset = lain_ast_offset(ref);
  if (segment == LAIN_AST_OUT)
    return lain_ast_output_node(h->out, ref, out);
  if (segment >= V2_SEG_AST_BASE &&
      segment < V2_SEG_AST_BASE + LAINMETA_V2_MAX_SOURCES) {
    uint32_t source = (uint32_t)(segment - V2_SEG_AST_BASE);
    const LainAstArenaHeader *header;
    uint32_t slot;
    if (source >= h->source_count) return LAIN_AST_ERR_SEGMENT;
    header = v2_arena_header(h->sources[source].tree);
    if (!header || offset < header->nodes_offset) return LAIN_AST_ERR_RANGE;
    slot = (uint32_t)((offset - header->nodes_offset) / LAIN_AST_NODE_STRIDE);
    if (!lainmeta_tree_node(h->sources[source].tree, slot, out))
      return LAIN_AST_ERR_SLOT;
    return 0;
  }
  return LAIN_AST_ERR_SEGMENT;
}

static uint32_t v2_node_sym(LainMetaHostV2 *h, uint32_t source, uint32_t index,
                            bool is_out) {
  if (is_out) {
    if (!h->out_syms || index >= h->out_index_cap) return 0;
    return h->out_syms[index];
  }
  {
    const uint32_t *ids = NULL;
    uint32_t count = 0;
    if (!lainmeta_tree_symbols(h->sources[source].tree, &ids, &count)) return 0;
    if (index >= count) return 0;
    return ids[index];
  }
}

/* --- 名字与类型 ---------------------------------------------------------- */

static bool v2_sym_view(LainMetaHostV2 *h, uint32_t handle,
                        const unsigned char **bytes, uint32_t *length) {
  if (!h->syms || (handle >> V2_CLASS_SHIFT) != V2_SYM) return false;
  return lainmeta_sym_bytes(h->syms, handle & V2_INDEX_MASK, bytes, length);
}

static const char *v2_name(LainMetaHostV2 *h, L1Builder *builder, uint32_t handle,
                           char *scratch, size_t capacity) {
  const unsigned char *bytes = NULL;
  uint32_t length = 0;
  if (!v2_sym_view(h, handle, &bytes, &length)) return NULL;
  if ((size_t)length + 1 > capacity) return NULL;
  if (length) memcpy(scratch, bytes, length);
  scratch[length] = 0;
  if (!builder) return scratch;
  return lainir_builder_string(builder, scratch);
}

static const L1Type *v2_type(L1Builder *builder, uint64_t code, uint64_t width) {
  if (code == 0) return NULL;
  if (code > 4) return NULL;
  return lainir_type(builder, (L1TypeKind)(code - 1), (uint32_t)width);
}

/* --- 模块 ---------------------------------------------------------------- */

static V2Module *v2_module(LainMetaHostV2 *h, uint32_t handle) {
  uint32_t index;
  if ((handle >> V2_CLASS_SHIFT) != V2_MODULE) return NULL;
  index = handle & V2_INDEX_MASK;
  if (index >= LAINMETA_V2_MAX_MODULES) return NULL;
  if (!h->mods[index].in_use) return NULL;
  return &h->mods[index];
}

static const L1Region *v2_region(LainMetaHostV2 *h, uint32_t handle,
                                 uint32_t module_index) {
  uint32_t index;
  if ((handle >> V2_CLASS_SHIFT) != V2_REGION) return NULL;
  if (((handle >> 20) & 0xFFu) != module_index) return NULL;
  index = handle & 0xFFFFFu;
  if (!index) return NULL;
  index -= 1;
  if (index >= h->mods[module_index].region_count) return NULL;
  return h->mods[module_index].regions[index];
}

static uint32_t v2_append(LainMetaHostV2 *h, V2Module *m, const L1Inst *inst) {
  V2Frame *frame;
  if (!inst) return LAINMETA_V2_ERR_IR;
  if (!m->proc.open || !m->proc.depth) return LAINMETA_V2_ERR_STATE;
  frame = &m->proc.frames[m->proc.depth - 1];
  if (frame->inst_count == V2_INST_MAX) return LAINMETA_V2_ERR_CAPACITY;
  frame->insts[frame->inst_count++] = inst;
  m->proc.last = inst;
  /* 模块实体是缓存的：追加指令后必须作废，否则下一次验证看的是旧快照。 */
  m->module = NULL;
  (void)h;
  return 0;
}

static uint32_t v2_take_operands(V2Module *m, L1Operand *out,
                                uint32_t *count) {
  uint32_t i;
  *count = m->arg_count;
  for (i = 0; i < m->arg_count; i++) out[i] = m->args[i];
  m->arg_count = 0;
  return 0;
}

static uint32_t v2_build_module(V2Module *m) {
  L1Data data[V2_DATA_MAX];
  L1Subroutine subs[V2_PROC_MAX];
  uint32_t i;
  if (m->module) return 0;
  if (m->proc.open) return LAINMETA_V2_ERR_STATE;
  for (i = 0; i < m->data_count; i++) data[i] = *m->data[i];
  for (i = 0; i < m->sub_count; i++) subs[i] = *m->subs[i];
  m->module = lainir_module(m->builder, m->name, data, m->data_count, subs,
                            m->sub_count);
  if (!m->module) return LAINMETA_V2_ERR_IR;
  return 0;
}

static uint32_t v2_verify_module(LainMetaHostV2 *h, V2Module *m) {
  L1Diagnostic diag;
  uint32_t rc = v2_build_module(m);
  if (rc) return rc;
  memset(&diag, 0, sizeof(diag));
  if (lainir_verify(m->module, &diag) != 0) {
    v2_diag(h, 1, 0, diag.message);
    return (uint32_t)diag.code;
  }
  return 0;
}

static const L1Subroutine *v2_find_subroutine(const L1Module *module,
                                              const char *name) {
  uint32_t i;
  for (i = 0; i < module->subroutine_count; i++) {
    const char *sub_name = module->subroutines[i].name;
    if (sub_name && strcmp(sub_name, name) == 0) return &module->subroutines[i];
  }
  return NULL;
}

/* --- 原语：源码与符号 ---------------------------------------------------- */

static uint32_t v2_source_count(void *ctx, const uint64_t *args, uint32_t argc,
                                uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  (void)args;
  if (argc) return LAINMETA_V2_ERR_ARG;
  *res = h->source_count;
  return 0;
}

static uint32_t v2_source_root(void *ctx, const uint64_t *args, uint32_t argc,
                               uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  uint32_t source, offset = 0;
  const LainAstArenaHeader *header;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  source = (uint32_t)args[0];
  if (source >= h->source_count) return LAINMETA_V2_ERR_ARG;
  header = v2_arena_header(h->sources[source].tree);
  if (!header) return LAINMETA_V2_ERR_SOURCE;
  offset = (uint32_t)((lain_ast_offset(header->root) - header->nodes_offset) /
                      LAIN_AST_NODE_STRIDE);
  *res = handle_ast_in(source, offset);
  return 0;
}

/* 源码的路径符号：模块名就用它，过程归属由此确定。 */
static uint32_t v2_source_name(void *ctx, const uint64_t *args, uint32_t argc,
                               uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  const V2Source *source;
  LainMetaSymId sym;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  if ((uint32_t)args[0] >= h->source_count) return LAINMETA_V2_ERR_SOURCE;
  source = &h->sources[(uint32_t)args[0]];
  if (!source->path) return LAINMETA_V2_ERR_SOURCE;
  sym = lainmeta_sym_intern(h->syms, source->path,
                            (uint32_t)strlen(source->path));
  if (!sym) return LAINMETA_V2_ERR_CAPACITY;
  *res = handle_sym(sym);
  return 0;
}

static uint32_t v2_source_import(void *ctx, const uint64_t *args, uint32_t argc,
                                 uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  const unsigned char *bytes = NULL;
  uint32_t length = 0;
  char rel[V2_PATH_MAX];
  char joined[V2_PATH_MAX];
  char canonical[V2_PATH_MAX];
  char *text;
  uint32_t size = 0, index = 0, i;
  *res = 0xFFFFFFFFu;
  if (argc != 2) return LAINMETA_V2_ERR_ARG;
  if ((uint32_t)args[0] >= h->source_count) return LAINMETA_V2_ERR_ARG;
  if (!v2_sym_view(h, (uint32_t)args[1], &bytes, &length))
    return LAINMETA_V2_ERR_SYM;
  if (length + 1 > sizeof(rel)) return LAINMETA_V2_ERR_ARG;
  memcpy(rel, bytes, length);
  rel[length] = 0;
  if (snprintf(joined, sizeof(joined), "%s", rel) >= (int)sizeof(joined))
    return LAINMETA_V2_ERR_ARG;
  if (!v2_normalize(joined, canonical, sizeof(canonical)))
    return LAINMETA_V2_ERR_ARG;
  for (i = 0; i < h->source_count; i++) {
    if (h->sources[i].path && strcmp(h->sources[i].path, canonical) == 0) {
      *res = i;
      return 0;
    }
  }
  text = v2_read_file(canonical, &size);
  if (!text) return LAINMETA_V2_ERR_SOURCE;
  if (!v2_add_source(h, canonical, text, text, size, &index, NULL)) {
    free(text);
    return LAINMETA_V2_ERR_SOURCE;
  }
  *res = index;
  return 0;
}

static uint32_t v2_sym_intern(void *ctx, const uint64_t *args, uint32_t argc,
                              uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  uintptr_t address = (uintptr_t)args[0];
  uint32_t length = (uint32_t)args[1];
  LainMetaSymId sym;
  if (argc != 2) return LAINMETA_V2_ERR_ARG;
  if (!h->space || !lainvm_space_check(h->space, address, length,
                                      LAINVM_MEM_READ) )
    return LAINMETA_V2_ERR_RANGE;
  sym = lainmeta_sym_intern(h->syms, (const void *)address, length);
  if (!sym) return LAINMETA_V2_ERR_SYM;
  *res = handle_sym(sym);
  return 0;
}

static uint32_t v2_sym_find(void *ctx, const uint64_t *args, uint32_t argc,
                            uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  LainMetaSymId sym;
  if (argc != 2) return LAINMETA_V2_ERR_ARG;
  if (!h->space || !lainvm_space_check(h->space, (uintptr_t)args[0],
                                      (uint32_t)args[1], LAINVM_MEM_READ))
    return LAINMETA_V2_ERR_RANGE;
  sym = lainmeta_sym_find(h->syms, (const void *)(uintptr_t)args[0],
                          (uint32_t)args[1]);
  *res = sym ? handle_sym(sym) : 0;
  return 0;
}

/* 两个符号句柄的原文是否相同（AST 侧句柄按出现位置分配，只能按内容比）。 */
static uint32_t v2_sym_same(void *ctx, const uint64_t *args, uint32_t argc,
                            uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  const unsigned char *a = NULL;
  const unsigned char *b = NULL;
  uint32_t la = 0;
  uint32_t lb = 0;
  if (argc != 2) return LAINMETA_V2_ERR_ARG;
  if (!v2_sym_view(h, (uint32_t)args[0], &a, &la)) return LAINMETA_V2_ERR_SYM;
  if (!v2_sym_view(h, (uint32_t)args[1], &b, &lb)) return LAINMETA_V2_ERR_SYM;
  *res = (la == lb && (la == 0 || memcmp(a, b, la) == 0)) ? 1u : 0u;
  return 0;
}

static uint32_t v2_sym_addr(void *ctx, const uint64_t *args, uint32_t argc,
                            uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  const unsigned char *bytes = NULL;
  uint32_t length = 0;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  (void)v2_map_symbols(h);
  if (!v2_sym_view(h, (uint32_t)args[0], &bytes, &length))
    return LAINMETA_V2_ERR_SYM;
  *res = (uint64_t)(uintptr_t)bytes;
  return 0;
}

static uint32_t v2_sym_len(void *ctx, const uint64_t *args, uint32_t argc,
                           uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  const unsigned char *bytes = NULL;
  uint32_t length = 0;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  if (!v2_sym_view(h, (uint32_t)args[0], &bytes, &length))
    return LAINMETA_V2_ERR_SYM;
  *res = length;
  return 0;
}

/* --- 原语：AST ----------------------------------------------------------- */

static uint32_t v2_ast_kind(void *ctx, const uint64_t *args, uint32_t argc,
                            uint64_t *res) {
  LainAstNode node;
  uint32_t rc;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  rc = v2_node((LainMetaHostV2 *)ctx, (uint32_t)args[0], &node, NULL, NULL);
  if (rc) return rc;
  *res = node.kind;
  return 0;
}

static uint32_t v2_ast_delim(void *ctx, const uint64_t *args, uint32_t argc,
                             uint64_t *res) {
  LainAstNode node;
  uint32_t rc;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  rc = v2_node((LainMetaHostV2 *)ctx, (uint32_t)args[0], &node, NULL, NULL);
  if (rc) return rc;
  *res = node.delimiter;
  return 0;
}

static uint32_t v2_ast_sym(void *ctx, const uint64_t *args, uint32_t argc,
                           uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  LainAstNode node;
  uint32_t source = 0, index = 0, rc;
  uint32_t cls;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  rc = v2_node(h, (uint32_t)args[0], &node, &source, &index);
  if (rc) return rc;
  cls = (uint32_t)args[0] >> V2_CLASS_SHIFT;
  index = v2_node_sym(h, source, index, cls == V2_AST_OUT);
  /* 组节点没有符号：0 是「无」，不与 Sym 句柄混用。 */
  *res = index ? handle_sym((LainMetaSymId)index) : 0;
  return 0;
}

static uint32_t v2_ast_span(void *ctx, const uint64_t *args, uint32_t argc,
                            uint64_t *res) {
  LainAstNode node;
  uint32_t rc;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  rc = v2_node((LainMetaHostV2 *)ctx, (uint32_t)args[0], &node, NULL, NULL);
  if (rc) return rc;
  *res = node.source_span;
  return 0;
}

static uint32_t v2_ast_span_length(void *ctx, const uint64_t *args,
                                   uint32_t argc, uint64_t *res) {
  LainAstNode node;
  uint32_t rc;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  rc = v2_node((LainMetaHostV2 *)ctx, (uint32_t)args[0], &node, NULL, NULL);
  if (rc) return rc;
  *res = node.source_length;
  return 0;
}

static uint32_t v2_ast_child_count(void *ctx, const uint64_t *args,
                                   uint32_t argc, uint64_t *res) {
  LainAstNode node;
  uint32_t rc;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  rc = v2_node((LainMetaHostV2 *)ctx, (uint32_t)args[0], &node, NULL, NULL);
  if (rc) return rc;
  /* 词没有孩子：返回 0 而不是拒绝，调用方不必先判种类。 */
  *res = node.kind == LAIN_AST_GROUP ? node.child_count : 0;
  return 0;
}

static uint32_t v2_ast_child(void *ctx, const uint64_t *args, uint32_t argc,
                             uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  LainAstNode node;
  uint32_t source = 0, index = 0, rc;
  uint32_t cls;
  LainAstRef child = 0;
  uint32_t at = (uint32_t)args[1];
  if (argc != 2) return LAINMETA_V2_ERR_ARG;
  rc = v2_node(h, (uint32_t)args[0], &node, &source, &index);
  if (rc) return rc;
  /* 越界与「不是组」都回 0 句柄：探路式遍历不该把边界当失败。 */
  if (node.kind != LAIN_AST_GROUP || at >= node.child_count) {
    *res = 0;
    return 0;
  }
  cls = (uint32_t)args[0] >> V2_CLASS_SHIFT;
  if (cls == V2_AST_OUT) {
    if (lain_ast_output_child(h->out,
                              lain_ast_ref(LAIN_AST_OUT,
                                           LAIN_AST_HEADER_SIZE +
                                               (uint64_t)index *
                                                   LAIN_AST_NODE_STRIDE),
                              at, &child) != 0)
      return LAINMETA_V2_ERR_HANDLE;
  } else {
    LainAstArenaView view;
    if (!lainmeta_tree_arena(h->sources[source].tree, &view))
      return LAINMETA_V2_ERR_SOURCE;
    if (node.children_offset + (uint64_t)at * 8u + 8u > view.used)
      return LAINMETA_V2_ERR_RANGE;
    memcpy(&child,
           (const unsigned char *)view.data + node.children_offset +
               (uint64_t)at * 8u,
           8);
  }
  *res = v2_handle_of_ref(h, child);
  return 0;
}

static uint32_t v2_ast_literal_addr(void *ctx, const uint64_t *args,
                                    uint32_t argc, uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  LainAstNode node;
  uint32_t source = 0, index = 0, rc;
  const char *base = NULL;
  uint32_t cls;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  rc = v2_node(h, (uint32_t)args[0], &node, &source, &index);
  if (rc) return rc;
  if (node.kind != LAIN_AST_TOKEN) {
    *res = 0;
    return 0;
  }
  cls = (uint32_t)args[0] >> V2_CLASS_SHIFT;
  if (cls == V2_AST_OUT) {
    LainAstArenaView view;
    if (!h->out || !lain_ast_output_view(h->out, &view))
      return LAINMETA_V2_ERR_STATE;
    base = (const char *)view.data;
  } else {
    base = lainmeta_tree_text(h->sources[source].tree, NULL);
  }
  if (!base) return LAINMETA_V2_ERR_SOURCE;
  *res = (uint64_t)(uintptr_t)(base + lain_ast_offset(node.text));
  return 0;
}

static uint32_t v2_ast_literal_len(void *ctx, const uint64_t *args,
                                   uint32_t argc, uint64_t *res) {
  LainAstNode node;
  uint32_t rc;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  rc = v2_node((LainMetaHostV2 *)ctx, (uint32_t)args[0], &node, NULL, NULL);
  if (rc) return rc;
  *res = node.kind == LAIN_AST_TOKEN ? node.text_length : 0;
  return 0;
}

static void v2_reset_out_index(LainMetaHostV2 *h) {
  uint64_t published = lain_ast_output_node_count(h->out);
  if (published > h->out_index_cap) published = h->out_index_cap;
  if (published < h->tx_mark) published = h->tx_mark;
  memset(h->out_syms + h->tx_mark, 0,
         (size_t)(published - h->tx_mark) * sizeof(*h->out_syms));
}

static uint32_t v2_ast_tx_begin(void *ctx, const uint64_t *args, uint32_t argc,
                                uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  uint32_t rc;
  (void)args;
  if (argc) return LAINMETA_V2_ERR_ARG;
  if (!h->out) return LAINMETA_V2_ERR_STATE;
  if (h->tx_open) return LAINMETA_V2_ERR_TX;
  (void)v2_map_out(h);
  rc = lain_ast_output_begin(h->out, &h->tx_id);
  if (rc) return rc;
  h->tx_open = true;
  h->tx_mark = (uint32_t)lain_ast_output_node_count(h->out);
  *res = h->tx_id;
  return 0;
}

static uint32_t v2_ast_tx_commit(void *ctx, const uint64_t *args, uint32_t argc,
                                 uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  uint32_t rc;
  bool ok = true;
  LainAstRef root;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  if (!h->out || !h->tx_open) return LAINMETA_V2_ERR_TX;
  root = v2_ref_of(h, (uint32_t)args[0], &ok);
  if (!ok) return LAINMETA_V2_ERR_HANDLE;
  rc = lain_ast_output_commit(h->out, h->tx_id, root);
  if (rc) return rc;
  h->tx_open = false;
  *res = 0;
  return 0;
}

static uint32_t v2_ast_tx_rollback(void *ctx, const uint64_t *args,
                                   uint32_t argc, uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  uint32_t rc;
  (void)args;
  if (argc) return LAINMETA_V2_ERR_ARG;
  if (!h->out || !h->tx_open) return LAINMETA_V2_ERR_TX;
  rc = lain_ast_output_rollback(h->out, h->tx_id);
  if (rc) return rc;
  h->tx_open = false;
  v2_reset_out_index(h);
  *res = 0;
  return 0;
}

static uint32_t v2_ast_mk_token(void *ctx, const uint64_t *args, uint32_t argc,
                                uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  uintptr_t text = (uintptr_t)args[2];
  uint32_t length = (uint32_t)args[3];
  LainAstRef origin = 0, ref = 0;
  bool ok = true;
  uint32_t rc, index, published;
  if (argc != 4) return LAINMETA_V2_ERR_ARG;
  if (!h->out || !h->tx_open) return LAINMETA_V2_ERR_TX;
  if (length && (!h->space ||
                 !lainvm_space_check(h->space, text, length, LAINVM_MEM_READ)))
    return LAINMETA_V2_ERR_RANGE;
  if (args[1]) {
    origin = v2_ref_of(h, (uint32_t)args[1], &ok);
    if (!ok) return LAINMETA_V2_ERR_HANDLE;
  }
  rc = lain_ast_output_token(h->out, (const void *)text, length, origin, 0,
                            &ref);
  if (rc) return rc;
  published = (uint32_t)lain_ast_output_node_count(h->out);
  index = published - 1;
  if (index < h->out_index_cap) {
    h->out_syms[index] =
        (args[0] >> V2_CLASS_SHIFT) == V2_SYM ? (args[0] & V2_INDEX_MASK) : 0;
  }
  *res = handle_ast_out(index);
  return 0;
}

static uint32_t v2_ast_mk_group(void *ctx, const uint64_t *args, uint32_t argc,
                                uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  LainAstRef refs[256];
  LainAstRef slice = 0, ref = 0;
  uint32_t count = (uint32_t)args[1];
  uintptr_t children = (uintptr_t)args[2];
  uint32_t i, rc, index, published;
  if (argc != 3) return LAINMETA_V2_ERR_ARG;
  if (!h->out || !h->tx_open) return LAINMETA_V2_ERR_TX;
  if (count > sizeof(refs) / sizeof(refs[0])) return LAINMETA_V2_ERR_ARG;
  if (count && (!h->space || !lainvm_space_check(h->space, children,
                                                 (uint64_t)count * 4u,
                                                 LAINVM_MEM_READ)))
    return LAINMETA_V2_ERR_RANGE;
  for (i = 0; i < count; i++) {
    uint32_t handle;
    bool ok = true;
    memcpy(&handle, (const unsigned char *)children + (uint64_t)i * 4u, 4);
    refs[i] = v2_ref_of(h, handle, &ok);
    if (!ok) return LAINMETA_V2_ERR_HANDLE;
  }
  rc = lain_ast_output_refs(h->out, refs, count, &slice);
  if (rc) return rc;
  rc = lain_ast_output_group(h->out, args[0], slice, count, 0, 0, &ref);
  if (rc) return rc;
  published = (uint32_t)lain_ast_output_node_count(h->out);
  index = published - 1;
  if (index < h->out_index_cap) h->out_syms[index] = 0;
  *res = handle_ast_out(index);
  return 0;
}

static uint32_t v2_diag_emit(void *ctx, const uint64_t *args, uint32_t argc,
                             uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  char message[LAINMETA_V2_MAX_MESSAGE];
  uintptr_t text = (uintptr_t)args[2];
  uint32_t length = (uint32_t)args[3];
  if (argc != 4) return LAINMETA_V2_ERR_ARG;
  if (length >= sizeof(message)) return LAINMETA_V2_ERR_ARG;
  if (length && (!h->space ||
                 !lainvm_space_check(h->space, text, length, LAINVM_MEM_READ)))
    return LAINMETA_V2_ERR_RANGE;
  if (length) memcpy(message, (const void *)text, length);
  message[length] = 0;
  v2_diag(h, (uint32_t)args[1], args[0], message);
  *res = 0;
  return 0;
}

/* --- 原语：结构化 IR ----------------------------------------------------- */

static uint32_t v2_ir_mod_new(void *ctx, const uint64_t *args, uint32_t argc,
                              uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  char scratch[V2_NAME_MAX];
  const char *name;
  uint32_t i;
  V2Module *m;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  for (i = 0; i < LAINMETA_V2_MAX_MODULES; i++) {
    if (!h->mods[i].in_use) break;
  }
  if (i == LAINMETA_V2_MAX_MODULES) return LAINMETA_V2_ERR_CAPACITY;
  m = &h->mods[i];
  memset(m, 0, sizeof(*m));
  m->builder = lainir_builder_new();
  if (!m->builder) return LAINMETA_V2_ERR_IR;
  name = v2_name(h, m->builder, (uint32_t)args[0], scratch, sizeof(scratch));
  if (!name) {
    lainir_builder_free(m->builder);
    memset(m, 0, sizeof(*m));
    return LAINMETA_V2_ERR_SYM;
  }
  m->name = name;
  m->in_use = true;
  *res = handle_module(i);
  return 0;
}

static uint32_t v2_ir_data_add(void *ctx, const uint64_t *args, uint32_t argc,
                               uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  char scratch[V2_NAME_MAX];
  const char *symbol;
  uintptr_t bytes = (uintptr_t)args[2];
  uint32_t size = (uint32_t)args[3];
  const L1Data *data;
  if (argc != 5) return LAINMETA_V2_ERR_ARG;
  if (!m) return LAINMETA_V2_ERR_STATE;
  if (m->data_count == V2_DATA_MAX) return LAINMETA_V2_ERR_CAPACITY;
  if (size && (!h->space ||
               !lainvm_space_check(h->space, bytes, size, LAINVM_MEM_READ)))
    return LAINMETA_V2_ERR_RANGE;
  symbol = v2_name(h, m->builder, (uint32_t)args[1], scratch, sizeof(scratch));
  if (!symbol) return LAINMETA_V2_ERR_SYM;
  data = lainir_data(m->builder, symbol, (const uint8_t *)bytes, size,
                     args[4] != 0);
  if (!data) return LAINMETA_V2_ERR_IR;
  m->data[m->data_count++] = data;
  m->module = NULL;
  *res = m->data_count;
  return 0;
}

/* 查静态数据是否已经登记过（1 起的序号，0 表示没有）。克隆出来的模块带着
   主模块的数据段，反射表这类全局数据靠它判重。 */
static uint32_t v2_ir_data_find(void *ctx, const uint64_t *args, uint32_t argc,
                                uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  char scratch[V2_NAME_MAX];
  const char *symbol;
  uint32_t i;
  if (argc != 2) return LAINMETA_V2_ERR_ARG;
  if (!m) return LAINMETA_V2_ERR_HANDLE;
  symbol = v2_name(h, m->builder, (uint32_t)args[1], scratch, sizeof(scratch));
  if (!symbol) return LAINMETA_V2_ERR_SYM;
  *res = 0;
  for (i = 0; i < m->data_count; i++) {
    const char *name = m->data[i]->symbol;
    if (name && strcmp(name, symbol) == 0) {
      *res = (uint64_t)i + 1;
      break;
    }
  }
  return 0;
}

static uint32_t v2_ir_proc_begin(void *ctx, const uint64_t *args, uint32_t argc,
                                 uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  char scratch[V2_NAME_MAX];
  const char *name;
  if (argc != 2) return LAINMETA_V2_ERR_ARG;
  if (!m) return LAINMETA_V2_ERR_HANDLE;
  if (m->proc.open) return LAINMETA_V2_ERR_STATE;
  if (m->sub_count == V2_PROC_MAX) return LAINMETA_V2_ERR_CAPACITY;
  name = v2_name(h, m->builder, (uint32_t)args[1], scratch, sizeof(scratch));
  if (!name) return LAINMETA_V2_ERR_SYM;
  memset(&m->proc, 0, sizeof(m->proc));
  m->proc.name = name;
  m->proc.open = true;
  *res = 0;
  return 0;
}

static uint32_t v2_ir_proc_extern(void *ctx, const uint64_t *args,
                                  uint32_t argc, uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  char scratch[V2_NAME_MAX];
  char scratch2[V2_NAME_MAX];
  const char *name, *link_name;
  if (argc != 3) return LAINMETA_V2_ERR_ARG;
  if (!m) return LAINMETA_V2_ERR_HANDLE;
  if (m->proc.open) return LAINMETA_V2_ERR_STATE;
  if (m->sub_count == V2_PROC_MAX) return LAINMETA_V2_ERR_CAPACITY;
  name = v2_name(h, m->builder, (uint32_t)args[1], scratch, sizeof(scratch));
  link_name = v2_name(h, m->builder, (uint32_t)args[2], scratch2,
                      sizeof(scratch2));
  if (!name || !link_name) return LAINMETA_V2_ERR_SYM;
  memset(&m->proc, 0, sizeof(m->proc));
  m->proc.name = name;
  m->proc.link_name = link_name;
  m->proc.is_extern = true;
  m->proc.open = true;
  *res = 0;
  return 0;
}

static uint32_t v2_ir_proc_param(void *ctx, const uint64_t *args, uint32_t argc,
                                 uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  char scratch[V2_NAME_MAX];
  const char *name;
  const L1Type *ty;
  if (argc != 4) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open) return LAINMETA_V2_ERR_STATE;
  if (m->proc.param_count == V2_PROC_PARAM_MAX)
    return LAINMETA_V2_ERR_CAPACITY;
  name = v2_name(h, m->builder, (uint32_t)args[1], scratch, sizeof(scratch));
  ty = v2_type(m->builder, args[2], args[3]);
  if (!name || !ty) return LAINMETA_V2_ERR_ARG;
  m->proc.params[m->proc.param_count++] = lainir_proc_param(name, ty);
  *res = m->proc.param_count;
  return 0;
}

static uint32_t v2_ir_proc_result(void *ctx, const uint64_t *args,
                                  uint32_t argc, uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  const L1Type *ty;
  if (argc != 3) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open) return LAINMETA_V2_ERR_STATE;
  if (m->proc.result_count == L1_MAX_RESULTS) return LAINMETA_V2_ERR_CAPACITY;
  ty = v2_type(m->builder, args[1], args[2]);
  if (!ty) return LAINMETA_V2_ERR_ARG;
  m->proc.results[m->proc.result_count++] = ty;
  *res = m->proc.result_count;
  return 0;
}

static uint32_t v2_ir_proc_end(void *ctx, const uint64_t *args, uint32_t argc,
                               uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  const L1Subroutine *sub;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  if (!m) return LAINMETA_V2_ERR_HANDLE;
  if (m->proc.is_extern && m->proc.open) {
    if (m->proc.depth) return LAINMETA_V2_ERR_STATE;
    sub = lainir_subroutine_extern(m->builder, m->proc.name, m->proc.link_name,
                                   m->proc.params, m->proc.param_count,
                                   m->proc.results, m->proc.result_count);
    if (!sub) return LAINMETA_V2_ERR_IR;
    m->subs[m->sub_count++] = sub;
    m->module = NULL;
    m->proc.open = false;
    *res = m->sub_count;
    return 0;
  }
  /* 普通过程在顶层区域结束时已经收尾，这里只报告已有的子过程数。 */
  if (m->proc.open) return LAINMETA_V2_ERR_STATE;
  *res = m->sub_count;
  return 0;
}

static uint32_t v2_ir_region_begin(void *ctx, const uint64_t *args,
                                   uint32_t argc, uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open) return LAINMETA_V2_ERR_STATE;
  if (m->proc.depth == V2_FRAME_MAX) return LAINMETA_V2_ERR_CAPACITY;
  memset(&m->proc.frames[m->proc.depth], 0, sizeof(V2Frame));
  m->proc.depth++;
  *res = m->proc.depth;
  return 0;
}

static uint32_t v2_ir_region_param(void *ctx, const uint64_t *args,
                                   uint32_t argc, uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  V2Frame *frame;
  char scratch[V2_NAME_MAX];
  const char *name;
  const L1Type *ty;
  L1Operand init;
  if (argc != 6) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open || !m->proc.depth) return LAINMETA_V2_ERR_STATE;
  frame = &m->proc.frames[m->proc.depth - 1];
  if (frame->param_count == V2_PROC_PARAM_MAX)
    return LAINMETA_V2_ERR_CAPACITY;
  name = v2_name(h, m->builder, (uint32_t)args[1], scratch, sizeof(scratch));
  ty = v2_type(m->builder, args[2], args[3]);
  if (!name || !ty) return LAINMETA_V2_ERR_ARG;
  if (args[4] == 0) {
    init = lainir_int(args[5]);
  } else if (args[4] == 1) {
    init = lainir_float_bits(args[5]);
  } else if (args[4] == 2) {
    char value[V2_NAME_MAX];
    const char *value_name;
    if (!(args[5] >> V2_CLASS_SHIFT) ||
        (args[5] >> V2_CLASS_SHIFT) != V2_SYM)
      return LAINMETA_V2_ERR_ARG;
    value_name = v2_name(h, m->builder, (uint32_t)args[5], value,
                         sizeof(value));
    if (!value_name) return LAINMETA_V2_ERR_SYM;
    init = lainir_ref(value_name);
  } else {
    return LAINMETA_V2_ERR_ARG;
  }
  frame->params[frame->param_count++] = lainir_param(name, ty, init);
  *res = frame->param_count;
  return 0;
}

static uint32_t v2_ir_region_result(void *ctx, const uint64_t *args,
                                    uint32_t argc, uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  V2Frame *frame;
  const L1Type *ty;
  if (argc != 3) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open || !m->proc.depth) return LAINMETA_V2_ERR_STATE;
  frame = &m->proc.frames[m->proc.depth - 1];
  if (frame->result_count == L1_MAX_RESULTS) return LAINMETA_V2_ERR_CAPACITY;
  ty = v2_type(m->builder, args[1], args[2]);
  if (!ty) return LAINMETA_V2_ERR_ARG;
  frame->results[frame->result_count++] = ty;
  *res = frame->result_count;
  return 0;
}

static uint32_t v2_ir_region_end(void *ctx, const uint64_t *args, uint32_t argc,
                                 uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  V2Frame *frame;
  const L1Region *region;
  uint32_t index;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open || !m->proc.depth) return LAINMETA_V2_ERR_STATE;
  if (m->region_count == V2_REGION_MAX) return LAINMETA_V2_ERR_CAPACITY;
  frame = &m->proc.frames[m->proc.depth - 1];
  region = lainir_region(m->builder, frame->params, frame->param_count,
                         frame->results, frame->result_count, frame->insts,
                         frame->inst_count);
  if (!region) return LAINMETA_V2_ERR_IR;
  index = m->region_count++;
  m->regions[index] = region;
  m->proc.depth--;
  m->proc.last = NULL;
  if (!m->proc.depth) {
    /* 过程体已完整：收尾成一个子过程。 */
    const L1Subroutine *sub = lainir_subroutine(
        m->builder, m->proc.name, m->proc.params, m->proc.param_count,
        m->proc.results, m->proc.result_count, region);
    if (!sub) return LAINMETA_V2_ERR_IR;
    m->subs[m->sub_count++] = sub;
    m->module = NULL;
    m->proc.open = false;
  }
  *res = handle_region((uint32_t)(m - h->mods), index);
  return 0;
}

static uint32_t v2_ir_args_reset(void *ctx, const uint64_t *args, uint32_t argc,
                                 uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  m = v2_module(h, (uint32_t)args[0]);
  if (!m) return LAINMETA_V2_ERR_HANDLE;
  m->arg_count = 0;
  *res = 0;
  return 0;
}

static uint32_t v2_ir_arg_ref(void *ctx, const uint64_t *args, uint32_t argc,
                              uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m;
  char scratch[V2_NAME_MAX];
  const char *name;
  if (argc != 2) return LAINMETA_V2_ERR_ARG;
  m = v2_module(h, (uint32_t)args[0]);
  if (!m) return LAINMETA_V2_ERR_HANDLE;
  if (m->arg_count == L1_MAX_OPERANDS) return LAINMETA_V2_ERR_CAPACITY;
  name = v2_name(h, m->builder, (uint32_t)args[1], scratch, sizeof(scratch));
  if (!name) return LAINMETA_V2_ERR_SYM;
  m->args[m->arg_count++] = lainir_ref(name);
  *res = m->arg_count;
  return 0;
}

static uint32_t v2_ir_arg_int(void *ctx, const uint64_t *args, uint32_t argc,
                              uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m;
  if (argc != 2) return LAINMETA_V2_ERR_ARG;
  m = v2_module(h, (uint32_t)args[0]);
  if (!m) return LAINMETA_V2_ERR_HANDLE;
  if (m->arg_count == L1_MAX_OPERANDS) return LAINMETA_V2_ERR_CAPACITY;
  m->args[m->arg_count++] = lainir_int(args[1]);
  *res = m->arg_count;
  return 0;
}

static uint32_t v2_ir_arg_float(void *ctx, const uint64_t *args, uint32_t argc,
                                uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m;
  if (argc != 2) return LAINMETA_V2_ERR_ARG;
  m = v2_module(h, (uint32_t)args[0]);
  if (!m) return LAINMETA_V2_ERR_HANDLE;
  if (m->arg_count == L1_MAX_OPERANDS) return LAINMETA_V2_ERR_CAPACITY;
  m->args[m->arg_count++] = lainir_float_bits(args[1]);
  *res = m->arg_count;
  return 0;
}

static uint32_t v2_ir_inst(void *ctx, const uint64_t *args, uint32_t argc,
                           uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  L1Operand operands[L1_MAX_OPERANDS];
  char scratch[V2_NAME_MAX];
  const char *result_name = NULL;
  const L1Type *ty;
  const L1Inst *inst;
  uint32_t count = 0, kind = (uint32_t)args[1], rc;
  if (argc != 5) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open || !m->proc.depth) return LAINMETA_V2_ERR_STATE;
  if (kind >= INST_COUNT) return LAINMETA_V2_ERR_ARG;
  if (args[2]) {
    result_name = v2_name(h, m->builder, (uint32_t)args[2], scratch,
                          sizeof(scratch));
    if (!result_name) return LAINMETA_V2_ERR_SYM;
  }
  ty = v2_type(m->builder, args[3], args[4]);
  v2_take_operands(m, operands, &count);
  inst = lainir_inst(m->builder, (L1InstKind)kind, result_name, ty, operands,
                     count);
  rc = v2_append(h, m, inst);
  if (rc) return rc;
  *res = 1;
  return 0;
}

static uint32_t v2_ir_inst_if(void *ctx, const uint64_t *args, uint32_t argc,
                              uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  char scratch[V2_NAME_MAX], cond[V2_NAME_MAX];
  const char *result_name = NULL, *cond_name;
  const L1Region *then_body, *else_body = NULL;
  const L1Inst *inst;
  uint32_t rc;
  if (argc != 5) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open || !m->proc.depth) return LAINMETA_V2_ERR_STATE;
  if (args[1]) {
    result_name = v2_name(h, m->builder, (uint32_t)args[1], scratch,
                          sizeof(scratch));
    if (!result_name) return LAINMETA_V2_ERR_SYM;
  }
  cond_name = v2_name(h, m->builder, (uint32_t)args[2], cond, sizeof(cond));
  if (!cond_name) return LAINMETA_V2_ERR_SYM;
  then_body = v2_region(h, (uint32_t)args[3], (uint32_t)(m - h->mods));
  if (!then_body) return LAINMETA_V2_ERR_HANDLE;
  if (args[4]) {
    else_body = v2_region(h, (uint32_t)args[4], (uint32_t)(m - h->mods));
    if (!else_body) return LAINMETA_V2_ERR_HANDLE;
  }
  inst = lainir_inst_if(m->builder, result_name, lainir_ref(cond_name),
                        then_body, else_body);
  rc = v2_append(h, m, inst);
  if (rc) return rc;
  *res = 1;
  return 0;
}

static uint32_t v2_ir_inst_loop(void *ctx, const uint64_t *args, uint32_t argc,
                                uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  char scratch[V2_NAME_MAX], label[V2_NAME_MAX];
  const char *result_name = NULL, *label_name;
  const L1Region *body;
  const L1Inst *inst;
  uint32_t rc;
  if (argc != 4) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open || !m->proc.depth) return LAINMETA_V2_ERR_STATE;
  if (args[1]) {
    result_name = v2_name(h, m->builder, (uint32_t)args[1], scratch,
                          sizeof(scratch));
    if (!result_name) return LAINMETA_V2_ERR_SYM;
  }
  label_name = v2_name(h, m->builder, (uint32_t)args[2], label, sizeof(label));
  if (!label_name) return LAINMETA_V2_ERR_SYM;
  body = v2_region(h, (uint32_t)args[3], (uint32_t)(m - h->mods));
  if (!body) return LAINMETA_V2_ERR_HANDLE;
  inst = lainir_inst_loop(m->builder, result_name, label_name, body);
  rc = v2_append(h, m, inst);
  if (rc) return rc;
  *res = 1;
  return 0;
}

static uint32_t v2_ir_case_add(void *ctx, const uint64_t *args, uint32_t argc,
                               uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  const L1Region *body;
  if (argc != 3) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open) return LAINMETA_V2_ERR_STATE;
  if (m->proc.case_count == V2_CASE_MAX) return LAINMETA_V2_ERR_CAPACITY;
  body = v2_region(h, (uint32_t)args[2], (uint32_t)(m - h->mods));
  if (!body) return LAINMETA_V2_ERR_HANDLE;
  m->proc.cases[m->proc.case_count].value = args[1];
  m->proc.cases[m->proc.case_count].body = body;
  m->proc.case_count++;
  *res = m->proc.case_count;
  return 0;
}

static uint32_t v2_ir_inst_switch(void *ctx, const uint64_t *args,
                                  uint32_t argc, uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  char scratch[V2_NAME_MAX], selector[V2_NAME_MAX];
  const char *result_name = NULL, *selector_name;
  const L1Type *ty;
  const L1Region *default_case = NULL;
  const L1Inst *inst;
  uint32_t rc;
  if (argc != 6) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open || !m->proc.depth) return LAINMETA_V2_ERR_STATE;
  if (args[1]) {
    result_name = v2_name(h, m->builder, (uint32_t)args[1], scratch,
                          sizeof(scratch));
    if (!result_name) return LAINMETA_V2_ERR_SYM;
  }
  selector_name =
      v2_name(h, m->builder, (uint32_t)args[2], selector, sizeof(selector));
  if (!selector_name) return LAINMETA_V2_ERR_SYM;
  ty = v2_type(m->builder, args[3], args[4]);
  if (!ty) return LAINMETA_V2_ERR_ARG;
  if (args[5]) {
    default_case = v2_region(h, (uint32_t)args[5], (uint32_t)(m - h->mods));
    if (!default_case) return LAINMETA_V2_ERR_HANDLE;
  }
  inst = lainir_inst_switch(m->builder, result_name, lainir_ref(selector_name),
                            ty, m->proc.cases, m->proc.case_count,
                            default_case);
  m->proc.case_count = 0;
  rc = v2_append(h, m, inst);
  if (rc) return rc;
  *res = 1;
  return 0;
}

static uint32_t v2_ir_inst_jump(void *ctx, const uint64_t *args, uint32_t argc,
                                uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  L1Operand operands[L1_MAX_OPERANDS];
  char label[V2_NAME_MAX];
  const char *label_name;
  const L1Inst *inst;
  uint32_t count = 0, kind = (uint32_t)args[1], rc;
  if (argc != 3) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open || !m->proc.depth) return LAINMETA_V2_ERR_STATE;
  if (kind != INST_BREAK && kind != INST_CONTINUE && kind != INST_RETURN &&
      kind != INST_YIELD && kind != INST_UNREACHABLE)
    return LAINMETA_V2_ERR_ARG;
  label_name = args[2] ? v2_name(h, m->builder, (uint32_t)args[2], label,
                                 sizeof(label))
                       : NULL;
  if (args[2] && !label_name) return LAINMETA_V2_ERR_SYM;
  v2_take_operands(m, operands, &count);
  inst = lainir_inst_jump(m->builder, (L1InstKind)kind, label_name, operands,
                          count);
  rc = v2_append(h, m, inst);
  if (rc) return rc;
  *res = 1;
  return 0;
}

static uint32_t v2_ir_inst_call(void *ctx, const uint64_t *args, uint32_t argc,
                                uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  L1Operand operands[L1_MAX_OPERANDS];
  char scratch[V2_NAME_MAX], callee[V2_NAME_MAX];
  const char *result_name = NULL, *callee_name;
  const L1Inst *inst;
  uint32_t count = 0, rc;
  if (argc != 4) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open || !m->proc.depth) return LAINMETA_V2_ERR_STATE;
  if (args[1]) {
    result_name = v2_name(h, m->builder, (uint32_t)args[1], scratch,
                          sizeof(scratch));
    if (!result_name) return LAINMETA_V2_ERR_SYM;
  }
  callee_name = v2_name(h, m->builder, (uint32_t)args[2], callee,
                        sizeof(callee));
  if (!callee_name) return LAINMETA_V2_ERR_SYM;
  v2_take_operands(m, operands, &count);
  inst = lainir_inst_call_ex(m->builder, result_name, callee_name, operands,
                             count, args[3] != 0);
  rc = v2_append(h, m, inst);
  if (rc) return rc;
  *res = 1;
  return 0;
}

// 间接调用：被调值作第 0 个操作数。参数类型每项压成 16 位，低 8 位是类型码
// （0 表示列表结束），高 8 位是宽度；每字四项，八项占两个字。
// 返回类型码为 0 表示没有返回值，此时结果名必须为空。
static uint32_t v2_ir_inst_call_indirect(void *ctx, const uint64_t *args,
                                         uint32_t argc, uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  L1Operand operands[L1_MAX_OPERANDS];
  const L1Type *params[L1_MAX_OPERANDS];
  char scratch[V2_NAME_MAX];
  const char *result_name = NULL;
  const L1Type *ret_ty;
  const L1Inst *inst;
  uint32_t count = 0, np = 0, i, rc;
  if (argc != 6) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open || !m->proc.depth) return LAINMETA_V2_ERR_STATE;
  if (args[1]) {
    result_name = v2_name(h, m->builder, (uint32_t)args[1], scratch,
                          sizeof(scratch));
    if (!result_name) return LAINMETA_V2_ERR_SYM;
  }
  for (i = 0; i < L1_MAX_OPERANDS; i++) {
    uint64_t word = i < 4 ? args[4] : args[5];
    uint64_t item = (word >> (16u * (i % 4u))) & 0xFFFFu;
    uint32_t code = (uint32_t)(item & 0xFFu);
    uint32_t width = (uint32_t)((item >> 8) & 0xFFu);
    if (!code) break;
    params[i] = v2_type(m->builder, code, width);
    if (!params[i]) return LAINMETA_V2_ERR_ARG;
    np++;
  }
  ret_ty = v2_type(m->builder, args[2], args[3]);
  if (args[2] && !ret_ty) return LAINMETA_V2_ERR_ARG;
  if (!args[2] && args[1]) return LAINMETA_V2_ERR_ARG;
  v2_take_operands(m, operands, &count);
  if (!count) return LAINMETA_V2_ERR_ARG;
  inst = lainir_inst_call_indirect(m->builder, result_name, ret_ty, params, np,
                                   operands, count);
  rc = v2_append(h, m, inst);
  if (rc) return rc;
  *res = 1;
  return 0;
}

static uint32_t v2_ir_inst_mem(void *ctx, const uint64_t *args, uint32_t argc,
                               uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  L1Operand operands[L1_MAX_OPERANDS];
  char scratch[V2_NAME_MAX];
  const char *result_name = NULL;
  const L1Type *ty;
  const L1Inst *inst;
  uint32_t count = 0, kind = (uint32_t)args[1], order = (uint32_t)args[5], rc;
  if (argc != 8) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open || !m->proc.depth) return LAINMETA_V2_ERR_STATE;
  if (kind != INST_LOAD && kind != INST_STORE && kind != INST_ALLOCA)
    return LAINMETA_V2_ERR_ARG;
  if (order > ORDER_SEQ_CST) return LAINMETA_V2_ERR_ARG;
  if (args[2]) {
    result_name = v2_name(h, m->builder, (uint32_t)args[2], scratch,
                          sizeof(scratch));
    if (!result_name) return LAINMETA_V2_ERR_SYM;
  }
  ty = v2_type(m->builder, args[3], args[4]);
  v2_take_operands(m, operands, &count);
  inst = lainir_inst_mem_aligned(m->builder, (L1InstKind)kind, result_name, ty,
                                 (L1MemOrder)order, args[6] != 0,
                                 (uint32_t)args[7], operands, count);
  rc = v2_append(h, m, inst);
  if (rc) return rc;
  *res = 1;
  return 0;
}

static uint32_t v2_ir_inst_symbol(void *ctx, const uint64_t *args,
                                  uint32_t argc, uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  char scratch[V2_NAME_MAX], symbol[V2_NAME_MAX];
  const char *result_name = NULL, *symbol_name;
  const L1Inst *inst;
  uint32_t kind = (uint32_t)args[1], rc;
  if (argc != 4) return LAINMETA_V2_ERR_ARG;
  if (!m || !m->proc.open || !m->proc.depth) return LAINMETA_V2_ERR_STATE;
  if (args[2]) {
    result_name = v2_name(h, m->builder, (uint32_t)args[2], scratch,
                          sizeof(scratch));
    if (!result_name) return LAINMETA_V2_ERR_SYM;
  }
  symbol_name =
      v2_name(h, m->builder, (uint32_t)args[3], symbol, sizeof(symbol));
  if (!symbol_name) return LAINMETA_V2_ERR_SYM;
  inst = lainir_inst_symbol(m->builder, (L1InstKind)kind, result_name,
                            symbol_name);
  rc = v2_append(h, m, inst);
  if (rc) return rc;
  *res = 1;
  return 0;
}

static uint32_t v2_ir_mod_verify(void *ctx, const uint64_t *args, uint32_t argc,
                                 uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  if (!m) return LAINMETA_V2_ERR_HANDLE;
  /* 校验失败是编译器的正常结果，不是宿主故障：把诊断码交回 guest，由它决定
   * 怎么报给用户。只有非法模块句柄才走 Trap。 */
  *res = v2_verify_module(h, m);
  return 0;
}

static uint32_t v2_ir_mod_emit_output(void *ctx, const uint64_t *args,
                                      uint32_t argc, uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  char *text;
  size_t length;
  uint32_t rc;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  if (!m) return LAINMETA_V2_ERR_HANDLE;
  rc = v2_verify_module(h, m);
  if (rc) return LAINMETA_V2_ERR_IR;
  text = lainir_print_to_string(m->module);
  if (!text) return LAINMETA_V2_ERR_IR;
  length = strlen(text);
  if (h->text_length + length + 1 > h->text_capacity) {
    uint64_t want = h->text_capacity ? h->text_capacity : 4096;
    char *grown;
    while (want < h->text_length + length + 1) want *= 2;
    grown = (char *)realloc(h->text, (size_t)want);
    if (!grown) {
      free(text);
      return LAINMETA_V2_ERR_CAPACITY;
    }
    h->text = grown;
    h->text_capacity = want;
  }
  memcpy(h->text + h->text_length, text, length);
  h->text_length += length;
  h->text[h->text_length] = 0;
  free(text);
  *res = length;
  return 0;
}

/* --- 原语：隔离求值的模块生命周期 ---------------------------------------- */

/* 把一个正在编译的模块复制成独立临时模块：已登记的 data 与已闭合的过程按
 * 指针搬运，源模块半成品过程（proc.open）不在其中；两边 builder 互不共享，
 * 临时模块释放时不动源模块的 arena。 */
static uint32_t v2_ir_mod_clone_completed(void *ctx, const uint64_t *args,
                                          uint32_t argc, uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *src = v2_module(h, (uint32_t)args[0]);
  V2Module *m;
  const char *name;
  uint32_t i;
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  if (!src) return LAINMETA_V2_ERR_HANDLE;
  for (i = 0; i < LAINMETA_V2_MAX_MODULES; i++) {
    if (!h->mods[i].in_use) break;
  }
  if (i == LAINMETA_V2_MAX_MODULES) return LAINMETA_V2_ERR_CAPACITY;
  m = &h->mods[i];
  memset(m, 0, sizeof(*m));
  m->builder = lainir_builder_new();
  if (!m->builder) return LAINMETA_V2_ERR_IR;
  name = lainir_builder_string(m->builder, src->name);
  if (!name) {
    lainir_builder_free(m->builder);
    memset(m, 0, sizeof(*m));
    return LAINMETA_V2_ERR_IR;
  }
  m->name = name;
  for (i = 0; i < src->data_count; i++) m->data[m->data_count++] = src->data[i];
  for (i = 0; i < src->sub_count; i++) m->subs[m->sub_count++] = src->subs[i];
  m->in_use = true;
  *res = handle_module((uint32_t)(m - h->mods));
  return 0;
}

/* 释放临时模块。句柄随即失效；写过的状态整块清零，槽位可以再次分配。 */
static uint32_t v2_ir_mod_free(void *ctx, const uint64_t *args, uint32_t argc,
                               uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  if (argc != 1) return LAINMETA_V2_ERR_ARG;
  if (!m) return LAINMETA_V2_ERR_HANDLE;
  lainir_builder_free(m->builder);
  memset(m, 0, sizeof(*m));
  *res = 0;
  return 0;
}

/* --- 原语：内存与 apply -------------------------------------------------- */

static uint32_t v2_mem_alloc(void *ctx, const uint64_t *args, uint32_t argc,
                             uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  uint64_t bytes = args[0];
  uint64_t align = argc > 1 ? args[1] : 0;
  uint64_t at;
  if (argc != 2) return LAINMETA_V2_ERR_ARG;
  if (!align) align = 16;
  if (align & (align - 1)) return LAINMETA_V2_ERR_ARG;
  if (align > 4096) return LAINMETA_V2_ERR_ARG;
  if (!h->workspace) return LAINMETA_V2_ERR_STATE;
  at = (h->workspace_used + align - 1) & ~(align - 1);
  if (at > h->workspace_bytes || bytes > h->workspace_bytes - at)
    return LAINMETA_V2_ERR_MEM;
  h->workspace_used = at + bytes;
  *res = (uint64_t)(h->workspace + at);
  return 0;
}

static uint32_t v2_apply_module(void *ctx, const uint64_t *args, uint32_t argc,
                                uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  char scratch[V2_NAME_MAX], entry[V2_NAME_MAX];
  char *text;
  LainMetaApplyValue values[8];
  LainMetaApplyResult result;
  L1Diagnostic diag;
  const L1Subroutine *sub;
  uintptr_t args_ptr = (uintptr_t)args[2];
  uintptr_t out_ptr = (uintptr_t)args[4];
  uint32_t arg_n = (uint32_t)args[3];
  uint32_t out_cap = (uint32_t)args[5];
  uint32_t i, rc;
  if (argc != 6) return LAINMETA_V2_ERR_ARG;
  if (!m) return LAINMETA_V2_ERR_HANDLE;
  if (arg_n > 8) return LAINMETA_V2_ERR_ARG;
  /* out 是一条 16 字节记录：[状态 0, 结果 8]。求值失败属于用户代码的语义结果，
     不能回错误码——宿主回调一旦回非 0，外层 VM 会直接以该码 Trap，
     编译器自己就被打死了。只有参数或句柄用错才回错误码。 */
  if (out_cap < 16) return LAINMETA_V2_ERR_ARG;
  if (!h->space) return LAINMETA_V2_ERR_STATE;
  if (arg_n && !lainvm_space_check(h->space, args_ptr, (uint64_t)arg_n * 8u,
                                   LAINVM_MEM_READ))
    return LAINMETA_V2_ERR_RANGE;
  if (!lainvm_space_check(h->space, out_ptr, 16, LAINVM_MEM_WRITE))
    return LAINMETA_V2_ERR_RANGE;
  rc = v2_verify_module(h, m);
  if (rc) return LAINMETA_V2_ERR_IR;
  {
    const char *name = v2_name(h, NULL, (uint32_t)args[1], entry,
                               sizeof(entry));
    if (!name) return LAINMETA_V2_ERR_SYM;
    sub = v2_find_subroutine(m->module, name);
    if (!sub) return LAINMETA_APPLY_ERR_NO_ENTRY;
  }
  if (arg_n != sub->param_count) return LAINMETA_APPLY_ERR_ARG_COUNT;
  for (i = 0; i < arg_n; i++) {
    uint64_t bits = 0;
    memcpy(&bits, (const unsigned char *)args_ptr + (uint64_t)i * 8u, 8);
    values[i].kind = sub->params[i].ty ? sub->params[i].ty->kind : TY_BITS;
    values[i].width = sub->params[i].ty ? sub->params[i].ty->width : 64;
    values[i].bits = bits;
  }
  text = lainir_print_to_string(m->module);
  if (!text) return LAINMETA_V2_ERR_IR;
  memset(&result, 0, sizeof(result));
  memset(&diag, 0, sizeof(diag));
  if (!lainmeta_apply_proc(text, entry, values, arg_n, &h->apply_limits,
                           &result, &diag)) {
    uint64_t fail[2];
    free(text);
    v2_diag(h, 1, 0, diag.message);
    fail[0] = diag.code ? diag.code : LAINMETA_V2_ERR_APPLY;
    fail[1] = 0;
    memcpy((void *)out_ptr, fail, sizeof(fail));
    *res = 0;
    return 0;
  }
  free(text);
  if (result.kind != LAINMETA_APPLY_RESULT_SCALAR) {
    uint64_t fail[2];
    fail[0] = LAINMETA_V2_ERR_APPLY;
    fail[1] = 0;
    memcpy((void *)out_ptr, fail, sizeof(fail));
    *res = 0;
    return 0;
  }
  {
    uint64_t record[2];
    record[0] = 0;
    record[1] = result.scalar.bits;
    memcpy((void *)out_ptr, record, sizeof(record));
  }
  *res = 0;
  return 0;
}

/* 隔离执行一个返回 #addr 的零参入口，并把它指向的字节流整块抄进 guest 缓冲。
   标量通道交付不了文本，编译期注入必须走这条。 */
static uint32_t v2_apply_module_bytes(void *ctx, const uint64_t *args,
                                     uint32_t argc, uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  V2Module *m = v2_module(h, (uint32_t)args[0]);
  char entry[V2_NAME_MAX];
  char *text;
  LainMetaApplyResult result;
  L1Diagnostic diag;
  const L1Subroutine *sub;
  uintptr_t out_ptr = (uintptr_t)args[2];
  uint64_t cap = args[3];
  size_t length;
  uint32_t rc;
  if (argc != 4) return LAINMETA_V2_ERR_ARG;
  if (!m) return LAINMETA_V2_ERR_HANDLE;
  if (!cap || cap > LAINMETA_APPLY_BYTES_MAX) return LAINMETA_V2_ERR_ARG;
  if (!h->space) return LAINMETA_V2_ERR_STATE;
  if (!lainvm_space_check(h->space, out_ptr, cap, LAINVM_MEM_WRITE))
    return LAINMETA_V2_ERR_RANGE;
  rc = v2_verify_module(h, m);
  if (rc) return LAINMETA_V2_ERR_IR;
  {
    const char *name = v2_name(h, NULL, (uint32_t)args[1], entry,
                               sizeof(entry));
    if (!name) return LAINMETA_V2_ERR_SYM;
    sub = v2_find_subroutine(m->module, name);
    if (!sub) return LAINMETA_APPLY_ERR_NO_ENTRY;
  }
  if (sub->param_count) return LAINMETA_APPLY_ERR_ARG_COUNT;
  text = lainir_print_to_string(m->module);
  if (!text) return LAINMETA_V2_ERR_IR;
  memset(&result, 0, sizeof(result));
  memset(&diag, 0, sizeof(diag));
  if (!lainmeta_apply_proc_bytes(text, entry, NULL, 0, cap, &h->apply_limits,
                                 &result, &diag)) {
    free(text);
    v2_diag(h, 1, 0, diag.message);
    return diag.code ? (uint32_t)diag.code : LAINMETA_V2_ERR_APPLY;
  }
  free(text);
  if (result.kind != LAINMETA_APPLY_RESULT_BYTES) {
    lainmeta_apply_bytes_free(&result.bytes);
    return LAINMETA_V2_ERR_APPLY;
  }
  length = (size_t)result.bytes.length;
  if (length > cap) length = (size_t)cap;
  memcpy((void *)out_ptr, result.bytes.data, length);
  lainmeta_apply_bytes_free(&result.bytes);
  *res = (uint64_t)length;
  return 0;
}

/* 把 guest 里一段文本登记成新的源码单元。词法错误不是宿主故障：
   只写诊断并把 0 交回 guest，绝不能返回非 0（那会让整台虚拟机 Trap）。 */
static uint32_t v2_ast_parse_snippet(void *ctx, const uint64_t *args,
                                     uint32_t argc, uint64_t *res) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)ctx;
  LainMetaV2Diag error;
  char *copy;
  uintptr_t ptr = (uintptr_t)args[0];
  uint64_t length = args[1];
  uint32_t index = 0;
  if (argc != 2) return LAINMETA_V2_ERR_ARG;
  *res = 0;
  if (!length || length > 0xFFFFFFFFull) {
    v2_diag(h, 1, 0, "snippet is empty");
    return 0;
  }
  if (!h->space) return LAINMETA_V2_ERR_STATE;
  if (!lainvm_space_check(h->space, ptr, length, LAINVM_MEM_READ))
    return LAINMETA_V2_ERR_RANGE;
  if (h->source_count == LAINMETA_V2_MAX_SOURCES) {
    v2_diag(h, 1, 0, "too many sources");
    return 0;
  }
  copy = (char *)malloc((size_t)length);
  if (!copy) return LAINMETA_V2_ERR_MEM;
  memcpy(copy, (const void *)ptr, (size_t)length);
  memset(&error, 0, sizeof(error));
  if (!v2_add_source(h, NULL, copy, copy, (uint32_t)length, &index, &error)) {
    v2_diag(h, 1, 0, error.message[0] ? error.message : "snippet rejected");
    return 0;
  }
  *res = index;
  return 0;
}

/* --- 登记表 -------------------------------------------------------------- */

typedef struct {
  const char *name;
  LainVmHostFn fn;
} V2Capability;

static const V2Capability k_v2_capabilities[] = {
    {"lain2_source_count", v2_source_count},
    {"lain2_source_root", v2_source_root},
    {"lain2_source_name", v2_source_name},
    {"lain2_source_import", v2_source_import},
    {"lain2_sym_intern", v2_sym_intern},
    {"lain2_sym_find", v2_sym_find},
    {"lain2_sym_same", v2_sym_same},
    {"lain2_sym_addr", v2_sym_addr},
    {"lain2_sym_len", v2_sym_len},
    {"lain2_diag_emit", v2_diag_emit},
    {"lain2_ast_kind", v2_ast_kind},
    {"lain2_ast_delim", v2_ast_delim},
    {"lain2_ast_sym", v2_ast_sym},
    {"lain2_ast_span", v2_ast_span},
    {"lain2_ast_span_length", v2_ast_span_length},
    {"lain2_ast_child_count", v2_ast_child_count},
    {"lain2_ast_child", v2_ast_child},
    {"lain2_ast_literal_addr", v2_ast_literal_addr},
    {"lain2_ast_literal_len", v2_ast_literal_len},
    {"lain2_ast_tx_begin", v2_ast_tx_begin},
    {"lain2_ast_tx_commit", v2_ast_tx_commit},
    {"lain2_ast_tx_rollback", v2_ast_tx_rollback},
    {"lain2_ast_mk_token", v2_ast_mk_token},
    {"lain2_ast_mk_group", v2_ast_mk_group},
    {"lain2_ir_mod_new", v2_ir_mod_new},
    {"lain2_ir_data_add", v2_ir_data_add},
    {"lain2_ir_data_find", v2_ir_data_find},
    {"lain2_ir_proc_begin", v2_ir_proc_begin},
    {"lain2_ir_proc_extern", v2_ir_proc_extern},
    {"lain2_ir_proc_param", v2_ir_proc_param},
    {"lain2_ir_proc_result", v2_ir_proc_result},
    {"lain2_ir_proc_end", v2_ir_proc_end},
    {"lain2_ir_region_begin", v2_ir_region_begin},
    {"lain2_ir_region_param", v2_ir_region_param},
    {"lain2_ir_region_result", v2_ir_region_result},
    {"lain2_ir_region_end", v2_ir_region_end},
    {"lain2_ir_args_reset", v2_ir_args_reset},
    {"lain2_ir_arg_ref", v2_ir_arg_ref},
    {"lain2_ir_arg_int", v2_ir_arg_int},
    {"lain2_ir_arg_float", v2_ir_arg_float},
    {"lain2_ir_inst", v2_ir_inst},
    {"lain2_ir_inst_if", v2_ir_inst_if},
    {"lain2_ir_inst_loop", v2_ir_inst_loop},
    {"lain2_ir_case_add", v2_ir_case_add},
    {"lain2_ir_inst_switch", v2_ir_inst_switch},
    {"lain2_ir_inst_jump", v2_ir_inst_jump},
    {"lain2_ir_inst_call", v2_ir_inst_call},
    {"lain2_ir_inst_call_indirect", v2_ir_inst_call_indirect},
    {"lain2_ir_inst_mem", v2_ir_inst_mem},
    {"lain2_ir_inst_symbol", v2_ir_inst_symbol},
    {"lain2_ir_mod_verify", v2_ir_mod_verify},
    {"lain2_ir_mod_emit_output", v2_ir_mod_emit_output},
    {"lain2_ir_mod_clone_completed", v2_ir_mod_clone_completed},
    {"lain2_ir_mod_free", v2_ir_mod_free},
    {"lain2_mem_alloc", v2_mem_alloc},
    {"lain2_apply_module", v2_apply_module},
    {"lain2_apply_module_bytes", v2_apply_module_bytes},
    {"lain2_ast_parse_snippet", v2_ast_parse_snippet},
};

int lainv2_host_register(LainMetaHostV2 *host, LainVmCaps *caps) {
  uint32_t i;
  if (!host || !caps) return LAINMETA_V2_ERR_ARG;
  (void)v2_map_symbols(host);
  (void)v2_map_out(host);
  for (i = 0; i < sizeof(k_v2_capabilities) / sizeof(k_v2_capabilities[0]); i++) {
    if (lainvm_caps_add(caps, k_v2_capabilities[i].name, LAINVM_CAP_FUNCTION,
                        k_v2_capabilities[i].fn, host) != LAINVM_CAPS_OK)
      return LAINMETA_V2_ERR_CAPACITY;
  }
  return 0;
}

/* --- 宿主生命周期 -------------------------------------------------------- */

LainMetaHostV2 *lainv2_host_new(void) {
  LainMetaHostV2 *h = (LainMetaHostV2 *)calloc(1, sizeof(*h));
  if (!h) return NULL;
  h->apply_limits.max_call_depth = 32;
  h->apply_limits.stack_bytes = 1u << 20;
  h->apply_limits.fuel = 1000000;
  h->apply_limits.quota_bytes = 16u << 20;
  return h;
}

void lainv2_host_free(LainMetaHostV2 *host) {
  uint32_t i;
  if (!host) return;
  for (i = 0; i < host->source_count; i++) {
    lainmeta_tree_free(host->sources[i].tree);
    free(host->sources[i].path);
    free(host->sources[i].owned_text);
  }
  for (i = 0; i < LAINMETA_V2_MAX_MODULES; i++) {
    if (host->mods[i].in_use) lainir_builder_free(host->mods[i].builder);
  }
  if (host->out) lain_ast_output_free(host->out);
  free(host->out_syms);
  if (host->workspace) {
    lainvm_quota_release(host->quota, host->workspace_bytes);
  }
  lainmeta_sym_free(host->syms);
  free(host->text);
  free(host);
}

void lainv2_host_attach_space(LainMetaHostV2 *host, LainVmSpace *space) {
  if (host) host->space = space;
}

void lainv2_host_attach_quota(LainMetaHostV2 *host, LainVmQuota *quota) {
  if (host) host->quota = quota;
}

bool lainv2_host_init_symbols(LainMetaHostV2 *host, uint32_t symbols,
                             uint32_t bytes) {
  if (!host || host->source_count || host->syms) return false;
  host->syms = lainmeta_sym_new(symbols, bytes, host->quota);
  return host->syms != NULL;
}

bool lainv2_host_init_ast_out(LainMetaHostV2 *host,
                             const LainExpandLimits *limits) {
  uint32_t error = 0;
  if (!host || host->out || !limits) return false;
  host->out = lain_ast_output_new(limits, host->quota, host, v2_resolve,
                                  &error);
  if (!host->out) return false;
  host->expand_limits = *limits;
  host->out_index_cap = (uint32_t)limits->ast_out_node_capacity;
  host->out_syms = (LainMetaSymId *)calloc(host->out_index_cap,
                                          sizeof(*host->out_syms));
  if (!host->out_syms) {
    lain_ast_output_free(host->out);
    host->out = NULL;
    return false;
  }
  (void)v2_map_out(host);
  return true;
}

void lainv2_host_set_apply_limits(LainMetaHostV2 *host,
                                 const LainMetaApplyLimits *limits) {
  if (host && limits) host->apply_limits = *limits;
}

bool lainv2_host_alloc_workspace(LainMetaHostV2 *host, uint64_t bytes,
                                uint64_t alignment) {
  LainVmRegionHandle handle;
  const LainVmRegion *region;
  if (!host || !host->space || !bytes || host->workspace) return false;
  handle = lainvm_space_alloc(host->space, bytes, alignment, bytes,
                              LAINVM_MEM_READ | LAINVM_MEM_WRITE, 0,
                              host->quota);
  if (lainvm_space_handle_none(handle)) return false;
  region = lainvm_space_slot(host->space, handle);
  if (!region) return false;
  host->workspace = region->base;
  host->workspace_bytes = region->capacity;
  host->workspace_used = 0;
  return true;
}

bool lainv2_host_add_source(LainMetaHostV2 *host, const char *path,
                           const char *text, uint32_t length,
                           uint32_t *index_out) {
  if (!host || !text) return false;
  if (!host->syms && !lainv2_host_init_symbols(host, 4096, 256u * 1024u))
    return false;
  return v2_add_source(host, path, text, NULL, length, index_out, NULL);
}

const char *lainv2_host_output(const LainMetaHostV2 *host, uint32_t *length) {
  if (!host) return NULL;
  if (length) *length = (uint32_t)host->text_length;
  return host->text;
}

uint32_t lainv2_host_diag_count(const LainMetaHostV2 *host) {
  return host ? host->diag_count : 0;
}

bool lainv2_host_diag_at(const LainMetaHostV2 *host, uint32_t index,
                        LainMetaV2Diag *out) {
  if (!host || !out || index >= host->diag_count) return false;
  *out = host->diags[index];
  return true;
}

const L1Module *lainv2_host_module(const LainMetaHostV2 *host, uint32_t handle) {
  LainMetaHostV2 *mutable_host = (LainMetaHostV2 *)host;
  V2Module *m = v2_module(mutable_host, handle);
  return m ? m->module : NULL;
}
