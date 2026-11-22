/* Native C99 host driver for compiled LAIN-Meta stages (stage1/stage2). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lain/backend/c99.h"
#include "lain/ir/build.h"
#include "lain/meta/host_v2.h"
#include "lain/text/parse.h"
#include "lain/vm/caps.h"
#include "lain/vm/quota.h"
#include "lain/vm/space.h"

#define NATIVE_MAX_SOURCES 128

static LainVmCaps g_caps;
static LainVmCapEntry g_cap_entries[128];

static uint64_t call_host_u64(const char *name, const uint64_t *args,
                              uint32_t argc) {
  const LainVmCapEntry *e = lainvm_caps_find(&g_caps, name);
  if (!e || !e->fn) {
    fprintf(stderr, "native-meta: missing host cap %s\n", name);
    abort();
  }
  uint64_t out = 0;
  uint32_t rc = e->fn(e->context, args, argc, &out);
  if (rc != 0) {
    fprintf(stderr, "native-meta: host cap %s failed rc=%u\n", name, rc);
    abort();
  }
  return out;
}

#define CAP0(name) \
  uint64_t name(void) { return call_host_u64(#name, NULL, 0); }
#define CAP1(name) \
  uint64_t name(uint64_t a0) { \
    uint64_t a[1] = {a0}; \
    return call_host_u64(#name, a, 1); \
  }
#define CAP2(name) \
  uint64_t name(uint64_t a0, uint64_t a1) { \
    uint64_t a[2] = {a0, a1}; \
    return call_host_u64(#name, a, 2); \
  }
#define CAP3(name) \
  uint64_t name(uint64_t a0, uint64_t a1, uint64_t a2) { \
    uint64_t a[3] = {a0, a1, a2}; \
    return call_host_u64(#name, a, 3); \
  }
#define CAP4(name) \
  uint64_t name(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) { \
    uint64_t a[4] = {a0, a1, a2, a3}; \
    return call_host_u64(#name, a, 4); \
  }
#define CAP5(name) \
  uint64_t name(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, \
                uint64_t a4) { \
    uint64_t a[5] = {a0, a1, a2, a3, a4}; \
    return call_host_u64(#name, a, 5); \
  }
#define CAP6(name) \
  uint64_t name(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, \
                uint64_t a4, uint64_t a5) { \
    uint64_t a[6] = {a0, a1, a2, a3, a4, a5}; \
    return call_host_u64(#name, a, 6); \
  }
#define CAP8(name) \
  uint64_t name(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, \
                uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) { \
    uint64_t a[8] = {a0, a1, a2, a3, a4, a5, a6, a7}; \
    return call_host_u64(#name, a, 8); \
  }

CAP0(lain2_source_count)
CAP1(lain2_source_root)
CAP1(lain2_source_name)
CAP2(lain2_source_import)

uint64_t lain2_sym_intern(void *p, uint64_t len) {
  uint64_t a[2] = {(uint64_t)(uintptr_t)p, len};
  return call_host_u64("lain2_sym_intern", a, 2);
}
uint64_t lain2_sym_find(void *p, uint64_t len) {
  uint64_t a[2] = {(uint64_t)(uintptr_t)p, len};
  return call_host_u64("lain2_sym_find", a, 2);
}
CAP2(lain2_sym_same)
void *lain2_sym_addr(uint64_t s) {
  uint64_t a[1] = {s};
  return (void *)(uintptr_t)call_host_u64("lain2_sym_addr", a, 1);
}
CAP1(lain2_sym_len)

uint64_t lain2_diag_emit(uint64_t span, uint64_t lvl, void *p, uint64_t len) {
  uint64_t a[4] = {span, lvl, (uint64_t)(uintptr_t)p, len};
  return call_host_u64("lain2_diag_emit", a, 4);
}

CAP1(lain2_ast_kind)
CAP1(lain2_ast_delim)
CAP1(lain2_ast_sym)
CAP1(lain2_ast_span)
CAP1(lain2_ast_span_length)
CAP1(lain2_ast_child_count)
CAP2(lain2_ast_child)
void *lain2_ast_literal_addr(uint64_t h) {
  uint64_t a[1] = {h};
  return (void *)(uintptr_t)call_host_u64("lain2_ast_literal_addr", a, 1);
}
CAP1(lain2_ast_literal_len)

CAP0(lain2_ast_tx_begin)
CAP1(lain2_ast_tx_commit)
CAP0(lain2_ast_tx_rollback)
CAP4(lain2_ast_mk_token)
CAP3(lain2_ast_mk_group)

CAP1(lain2_ir_mod_new)
uint64_t lain2_ir_data_add(uint64_t m, uint64_t sym, void *p, uint64_t sz,
                           uint64_t ro) {
  uint64_t a[5] = {m, sym, (uint64_t)(uintptr_t)p, sz, ro};
  return call_host_u64("lain2_ir_data_add", a, 5);
}
CAP2(lain2_ir_data_find)
CAP2(lain2_ir_proc_begin)
CAP3(lain2_ir_proc_extern)
CAP4(lain2_ir_proc_param)
CAP3(lain2_ir_proc_result)
CAP1(lain2_ir_proc_end)
CAP1(lain2_ir_region_begin)
CAP6(lain2_ir_region_param)
CAP3(lain2_ir_region_result)
CAP1(lain2_ir_region_end)
CAP1(lain2_ir_args_reset)
CAP2(lain2_ir_arg_ref)
CAP2(lain2_ir_arg_int)
CAP2(lain2_ir_arg_float)
CAP5(lain2_ir_inst)
CAP5(lain2_ir_inst_if)
CAP4(lain2_ir_inst_loop)
CAP3(lain2_ir_case_add)
CAP6(lain2_ir_inst_switch)
CAP3(lain2_ir_inst_jump)
CAP4(lain2_ir_inst_call)
CAP6(lain2_ir_inst_call_indirect)
CAP8(lain2_ir_inst_mem)
CAP4(lain2_ir_inst_symbol)
CAP1(lain2_ir_mod_verify)
CAP1(lain2_ir_mod_emit_output)
CAP1(lain2_ir_mod_clone_completed)
CAP1(lain2_ir_mod_free)

void *lain2_mem_alloc(uint64_t sz, uint64_t align) {
  uint64_t a[2] = {sz, align};
  return (void *)(uintptr_t)call_host_u64("lain2_mem_alloc", a, 2);
}
uint64_t lain2_apply_module(uint64_t m, uint64_t p, void *args, uint64_t ac,
                            void *out, uint64_t oc) {
  uint64_t a[6] = {m, p, (uint64_t)(uintptr_t)args, ac,
                   (uint64_t)(uintptr_t)out, oc};
  return call_host_u64("lain2_apply_module", a, 6);
}
uint64_t lain2_apply_module_bytes(uint64_t m, uint64_t p, void *out,
                                  uint64_t cap) {
  uint64_t a[4] = {m, p, (uint64_t)(uintptr_t)out, cap};
  return call_host_u64("lain2_apply_module_bytes", a, 4);
}
uint64_t lain2_ast_parse_snippet(void *p, uint64_t len) {
  uint64_t a[2] = {(uint64_t)(uintptr_t)p, len};
  return call_host_u64("lain2_ast_parse_snippet", a, 2);
}

extern uint64_t lain_fn_lain_meta_v2_entry(uint64_t count);
extern const void *lain_c99_data_anchor(void);

static int ends_with(const char *s, const char *suf) {
  size_t sl = strlen(s), fl = strlen(suf);
  return sl >= fl && strcmp(s + sl - fl, suf) == 0;
}

static int write_c99_output(const char *out_path, const char *out_text,
                            uint32_t out_len, const char *entry,
                            int print_result) {
  char *text = (char *)malloc((size_t)out_len + 1);
  if (!text) return 1;
  if (out_len > 0) memcpy(text, out_text, out_len);
  text[out_len] = '\0';

  L1Builder *builder = lainir_builder_new();
  if (!builder) {
    free(text);
    return 1;
  }
  L1Diagnostic diag;
  memset(&diag, 0, sizeof(diag));
  const L1Module *module = lainir_parse(builder, text, &diag);
  free(text);
  if (!module) {
    fprintf(stderr, "native-meta: C99 解析 LAINIR 失败: %s\n", diag.message);
    lainir_builder_free(builder);
    return 1;
  }

  FILE *f = fopen(out_path, "wb");
  if (!f) {
    fprintf(stderr, "cannot write %s\n", out_path);
    lainir_builder_free(builder);
    return 1;
  }
  LainC99Options opts;
  memset(&opts, 0, sizeof(opts));
  opts.entry = entry ? entry : "s0__main";
  opts.print_result = print_result != 0;
  memset(&diag, 0, sizeof(diag));
  int rc = lainir_emit_c99(module, f, &opts, &diag);
  fclose(f);
  if (rc != 0) {
    fprintf(stderr, "native-meta: C99 发射失败: %s\n", diag.message);
  }
  lainir_builder_free(builder);
  return rc == 0 ? 0 : 1;
}

static char *read_text(const char *path, uint32_t *len_out) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long sz = ftell(f);
  if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t nr = sz ? fread(buf, 1, (size_t)sz, f) : 0;
  fclose(f);
  buf[nr] = '\0';
  if (len_out) *len_out = (uint32_t)nr;
  return buf;
}

int main(int argc, char **argv) {
  const char *source_paths[NATIVE_MAX_SOURCES];
  char *source_texts[NATIVE_MAX_SOURCES] = {0};
  uint32_t source_count = 0;
  const char *out_path = NULL;
  const char *entry = NULL;
  int quiet = 0;
  int c99_mode = 0;
  int print_result = 0;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out_path = argv[++i];
    } else if (strcmp(argv[i], "--entry") == 0 && i + 1 < argc) {
      entry = argv[++i];
    } else if (strcmp(argv[i], "--c99") == 0) {
      c99_mode = 1;
    } else if (strcmp(argv[i], "--print-result") == 0) {
      print_result = 1;
    } else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) {
      if (source_count >= NATIVE_MAX_SOURCES) return 1;
      source_paths[source_count++] = argv[++i];
    } else if (strcmp(argv[i], "-q") == 0) {
      quiet = 1;
    } else if (strcmp(argv[i], "-v") == 0) {
      /* accepted for CLI parity */
    } else if ((strcmp(argv[i], "--fuel") == 0 ||
                strcmp(argv[i], "--slice") == 0 ||
                strcmp(argv[i], "--stack") == 0 ||
                strcmp(argv[i], "--quota") == 0 ||
                strcmp(argv[i], "--workspace") == 0 ||
                strcmp(argv[i], "--caps") == 0 ||
                strcmp(argv[i], "--depth") == 0 ||
                strcmp(argv[i], "--symbols") == 0 ||
                strcmp(argv[i], "--symbol-bytes") == 0) &&
               i + 1 < argc) {
      ++i;
    } else if (ends_with(argv[i], ".l1")) {
      /* drop-in parity with lain-meta-v2: ignore unit .l1 args */
    } else {
      if (source_count >= NATIVE_MAX_SOURCES) return 1;
      source_paths[source_count++] = argv[i];
    }
  }

  if (source_count == 0) {
    fprintf(stderr,
            "usage: lain-stage <source.lain> [--out <out.l1|out.c>] [--c99] "
            "[--entry <name>] [--print-result]\n");
    return 1;
  }

  LainVmQuota quota;
  lainvm_quota_init(&quota, 2ull << 30);
  LainVmSpace space;
  lainvm_space_init(&space);

  LainMetaHostV2 *host = lainv2_host_new();
  if (!host) return 1;
  lainv2_host_attach_space(host, &space);
  lainv2_host_attach_quota(host, &quota);
  if (!lainv2_host_init_symbols(host, 131072u, 8u << 20)) return 1;

  LainExpandLimits expand_limits;
  memset(&expand_limits, 0, sizeof(expand_limits));
  expand_limits.max_depth = 64;
  expand_limits.max_visits = 4000000;
  expand_limits.max_handler_calls = 1000000;
  expand_limits.max_replacement_refs = 65536;
  expand_limits.ast_out_capacity = 1u << 20;
  expand_limits.ast_out_node_capacity = 8192;
  if (!lainv2_host_init_ast_out(host, &expand_limits)) return 1;

  LainMetaApplyLimits apply_limits;
  memset(&apply_limits, 0, sizeof(apply_limits));
  apply_limits.caps = NULL;
  apply_limits.max_call_depth = 128;
  apply_limits.stack_bytes = 65536;
  apply_limits.fuel = 500000;
  apply_limits.quota_bytes = 524288;
  lainv2_host_set_apply_limits(host, &apply_limits);

  if (!lainv2_host_alloc_workspace(host, 384ull << 20, 64u)) {
    fprintf(stderr, "native-meta: workspace alloc failed\n");
    return 1;
  }

  uintptr_t stk = (uintptr_t)&quota;
  lainvm_space_map_external(&space, stk - 0x200000u, 0x400000u, 0x400000u,
                            LAINVM_MEM_READ | LAINVM_MEM_WRITE, 1);
  const void *anchor = lain_c99_data_anchor();
  if (anchor) {
    uintptr_t aaddr = (uintptr_t)anchor;
    lainvm_space_map_external(&space, aaddr - 0x400000u, 0x1000000u,
                              0x1000000u, LAINVM_MEM_READ | LAINVM_MEM_WRITE,
                              1);
  }

  for (uint32_t i = 0; i < source_count; ++i) {
    uint32_t len = 0, idx = 0;
    char *txt = read_text(source_paths[i], &len);
    if (!txt) {
      fprintf(stderr, "cannot open %s\n", source_paths[i]);
      return 1;
    }
    source_texts[i] = txt;
    if (!lainv2_host_add_source(host, source_paths[i], txt, len, &idx)) {
      fprintf(stderr, "add_source failed %s\n", source_paths[i]);
      return 1;
    }
  }

  lainvm_caps_init(&g_caps, g_cap_entries, 128);
  if (lainv2_host_register(host, &g_caps) != 0) {
    fprintf(stderr, "host_register failed\n");
    return 1;
  }
  lainvm_caps_freeze(&g_caps);

  uint64_t rc = lain_fn_lain_meta_v2_entry(source_count);
  uint32_t out_len = 0;
  const char *out_text = lainv2_host_output(host, &out_len);

  uint32_t diags = lainv2_host_diag_count(host);
  for (uint32_t d = 0; d < diags; ++d) {
    LainMetaV2Diag item;
    if (lainv2_host_diag_at(host, d, &item)) {
      printf("diag level=%u source=%d offset=%llu %s\n", item.level,
             (int)item.source, (unsigned long long)item.offset, item.message);
    }
  }

  int emit_c99 = c99_mode || (out_path && ends_with(out_path, ".c"));
  if (emit_c99 && out_path && rc == 0) {
    if (write_c99_output(out_path, out_text, out_len, entry, print_result) !=
        0) {
      for (uint32_t i = 0; i < source_count; ++i) free(source_texts[i]);
      lainv2_host_free(host);
      return 1;
    }
  } else if (out_path && rc == 0) {
    FILE *f = fopen(out_path, "wb");
    if (!f) {
      fprintf(stderr, "cannot write %s\n", out_path);
      return 1;
    }
    if (out_len > 0) fwrite(out_text, 1, out_len, f);
    fclose(f);
  } else if (!out_path && out_len > 0) {
    fwrite(out_text, 1, out_len, stdout);
  }

  if (!quiet) {
    printf("steps=0\n");
    printf("output bytes=%u\n", out_len);
    printf("workspace peak=%llu\n",
           (unsigned long long)lainv2_host_workspace_peak(host));
    printf("apply steps peak=%llu\n",
           (unsigned long long)lainv2_host_apply_fuel_peak(host));
    printf("apply quota peak=%llu\n",
           (unsigned long long)lainv2_host_apply_quota_peak(host));
    printf("apply stack peak=%llu\n",
           (unsigned long long)lainv2_host_apply_stack_peak(host));
    printf("result=%llu\n", (unsigned long long)rc);
  }

  for (uint32_t i = 0; i < source_count; ++i) free(source_texts[i]);
  lainv2_host_free(host);
  return rc == 0 ? 0 : 1;
}
