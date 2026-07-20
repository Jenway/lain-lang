/**
 * compiler/native_runtime.c — Native compiler runtime
 *
 * Provides:
 *   - L1 IR type definitions shared with the bootstrap compiler
 *   - Token grouping (complements compiler/lexer.lain's flat tokenizer)
 *   - L1 IR builder FFI functions (for Chibi-Scheme meta passes)
 *   - C code emission (walks L1 IR, writes C source)
 *   - Top-level API functions callable from Lain via @foreign
 *
 * Linked with:
 *   - Native C compiler front-end/runtime
 *   - Scheme VM backend selected by vm_api
 */

#include "lainir_exec.h"
#include "compiler/vm_compat.h"
#include "lainir/lainir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#define chdir _chdir
#else
#include <unistd.h>
#endif

static char *lain_strdup(const char *s) {
  size_t n = strlen(s);
  char *out = (char *)malloc(n + 1);
  if (!out) return NULL;
  memcpy(out, s, n + 1);
  return out;
}

#define strdup lain_strdup


// ══════════════════════════════════════════════════════════════════════════════
// 2. read_byte_at — single byte from raw pointer (used by lexer.lain)
// ══════════════════════════════════════════════════════════════════════════════

uint8_t read_byte_at(const uint8_t *ptr, size_t offset) { return ptr[offset]; }

// C-Side Lexer (REMOVED — replaced by std/meta/lexer.scm)
// All lexing, grouping, form-splitting, and token-to-sexp conversion
// now happens in pure Scheme via meta.lex-source! (pure Scheme implementation).


// ══════════════════════════════════════════════════════════════════════════════
// 7-8. L1 IR Builder FFI (→ compiler/builder_ffi.c + compiler/builder_ffi.h)
// ══════════════════════════════════════════════════════════════════════════════
#include "compiler/builder_ffi.h"
#include "compiler/structured_unit.h"

// ══════════════════════════════════════════════════════════════════════════════
// 9. C Code Emission (→ lainir/emitter.c)
// ══════════════════════════════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════════════════════════════
// 9.5. L1 IR Text Dump (→ lainir/emit_text.c)
// ══════════════════════════════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════════════════════════════
// 9.75. LainIR Interpreter (→ lainir/interpreter.c)
// ══════════════════════════════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════════════════════════════
// 11. Scheme Initialization + FFI Registration
// ══════════════════════════════════════════════════════════════════════════════

// ============================================================================
// 11. Scheme Initialization and Meta Source Loading
// ============================================================================

static const char *native_bootstrap_root(void) {
  const char *configured = getenv("LAIN_BOOTSTRAP_ROOT");
  return configured && *configured ? configured : "../lain-bootstrap";
}

static int native_load_bootstrap_file(
    sexp ctx, sexp env, const char *relative_path) {
  char original_cwd[1024];
  const char *root = native_bootstrap_root();
  if (!getcwd(original_cwd, sizeof(original_cwd))) return 0;
  if (chdir(root) != 0) return 0;
  vm_load_file((vm_context *)ctx, (vm_value *)env, relative_path);
  if (chdir(original_cwd) != 0)
    fprintf(stderr, "Warning: could not restore compiler working directory\n");
  return 1;
}

static void native_inject_all_polyfills(sexp ctx, sexp env) {
  vm_import_base((vm_context *)ctx, (vm_value *)env);
  if (!native_load_bootstrap_file(ctx, env, "polyfills.scm"))
    fprintf(stderr, "Warning: could not find bootstrap polyfills.scm\n");
}

static void native_load_meta_sources(sexp ctx, sexp env) {
  if (!native_load_bootstrap_file(ctx, env, "std/meta/driver.scm"))
    fprintf(stderr, "Warning: could not find bootstrap std/meta/driver.scm\n");
}

// ============================================================================

// ══════════════════════════════════════════════════════════════════════════════
// 12. Top-Level API (called from Lain via @foreign)
// ══════════════════════════════════════════════════════════════════════════════

// ============================================================================
// 12. Top-Level API Functions (called from Lain via @foreign)
// ============================================================================

// File buffer for read_file result
static uint8_t *g_file_buffer = NULL;
static uint32_t g_file_len = 0;

const uint8_t *native_read_file(const char *path) {
  if (g_file_buffer) {
    free(g_file_buffer);
    g_file_buffer = NULL;
  }
  FILE *f = fopen(path, "rb");
  if (!f) {
    g_file_len = 0;
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  g_file_buffer = malloc(len + 1);
  fread(g_file_buffer, 1, len, f);
  g_file_buffer[len] = '\0';
  fclose(f);
  g_file_len = (uint32_t)len;
  return g_file_buffer;
}

uint32_t native_file_len(void) { return g_file_len; }

// ── Import helper: read file → Scheme lex → list of form S-expressions ──

static sexp sexp_read_file_forms(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp arg_path) {
  const char *path = sexp_string_data(arg_path);
  const uint8_t *data = native_read_file(path);
  if (!data) return SEXP_FALSE;
  uint32_t len = native_file_len();

  // Use Scheme-side lexer
  sexp src_str = sexp_c_string(ctx, (const char *)data, len);
  sexp len_val = sexp_make_integer(ctx, sexp_string_length(src_str));
  sexp env = sexp_context_env(ctx);
  sexp lex_proc = sexp_env_ref(ctx, env,
                               sexp_intern(ctx, "meta.lex-source!", -1),
                               SEXP_FALSE);
  sexp result = sexp_apply(ctx, lex_proc, sexp_list2(ctx, src_str, len_val));

  return sexp_exceptionp(result) ? SEXP_FALSE : result;
}

// ── File helper: read file as raw string (no lexing) ──

static sexp sexp_read_file_string(sexp ctx, sexp self, sexp_sint_t n,
                                   sexp arg_path) {
  const char *path = sexp_string_data(arg_path);
  const uint8_t *data = native_read_file(path);
  if (!data) return SEXP_FALSE;
  uint32_t len = native_file_len();
  return sexp_c_string(ctx, (const char *)data, len);
}

// Bootstrap orchestration capability.  The compiler artifact produces text;
// this host helper only persists bytes to the path chosen by the harness.
static sexp sexp_write_file_string(sexp ctx, sexp self, sexp_sint_t n,
                                    sexp arg_path, sexp arg_text) {
  const char *path;
  const char *text;
  FILE *file;
  size_t length;
  size_t written;
  (void)ctx;
  (void)self;
  (void)n;
  if (!sexp_stringp(arg_path) || !sexp_stringp(arg_text))
    return sexp_make_fixnum(0);
  path = sexp_string_data(arg_path);
  text = sexp_string_data(arg_text);
  length = strlen(text);
  file = fopen(path, "wb");
  if (!file) return sexp_make_fixnum(0);
  written = fwrite(text, 1, length, file);
  if (fclose(file) != 0 || written != length)
    return sexp_make_fixnum(0);
  return sexp_make_fixnum(1);
}

// ── Interface parser: read .lci file ──
// Given a source .lain path like "std/io.lain", reads
// "std/io.lain.lci".

static sexp sexp_read_interface(sexp ctx, sexp self, sexp_sint_t n,
                                sexp arg_source_path) {
  const char *source_path = sexp_string_data(arg_source_path);
  char interface_path[2048];
  snprintf(interface_path, sizeof(interface_path), "%s.lci", source_path);
  const uint8_t *data = native_read_file(interface_path);
  if (!data) return SEXP_FALSE;
  uint32_t len = native_file_len();
  return sexp_read_from_string(ctx, (const char *)data, len);
}

// ── Build driver: compute compilation order via simple C scanning ──
// Scans source files for `import <path>;` statements using string matching,
// then does topological sort in C. Returns a Scheme list of file paths.



static sexp sexp_build_compute_order(sexp ctx, sexp self, sexp_sint_t n,
                                      sexp arg_root_path) {
#ifdef LAIN_DISABLE_NATIVE_BUILD
  (void)self;
  (void)n;
  (void)arg_root_path;
  return sexp_user_exception(ctx, NULL, "build: disabled in this host build", SEXP_NULL);
#else
  const char *root_path = sexp_string_data(arg_root_path);

  // Simple graph: array of {path, imports[], import_count, visited}
  #define MAX_MODULES 64
  #define MAX_IMPORTS 16
  typedef struct {
    char *path;
    char *imports[MAX_IMPORTS];
    int import_count;
    int scanned;  // 0=not scanned, 1=scanned
    int visited;  // 0=unvisited, 1=visiting, 2=done (for topo sort)
  } BuildNode;
  BuildNode nodes[MAX_MODULES];
  int node_count = 0;

  // Helper: find or create node
  int find_node(const char *p) {
    for (int i = 0; i < node_count; i++)
      if (strcmp(nodes[i].path, p) == 0) return i;
    if (node_count >= MAX_MODULES) return -1;
    nodes[node_count].path = strdup(p);
    nodes[node_count].import_count = 0;
    nodes[node_count].scanned = 0;
    nodes[node_count].visited = 0;
    return node_count++;
  }

  // Helper: scan file for imports
  void scan_file(const char *path, int node_idx) {
    const uint8_t *data = native_read_file(path);
    if (!data) return;
    const char *s = (const char*)data;
    while (*s) {
      // Skip whitespace and comments
      while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
      if (*s == '/' && s[1] == '/') {
        while (*s && *s != '\n') s++;
        continue;
      }
      // Look for "import "
      if (strncmp(s, "import ", 7) == 0 || strncmp(s, "import\t", 7) == 0) {
        s += 6; // skip "import"
        while (*s == ' ' || *s == '\t') s++;
        // Read module path: ident (:: ident)* ;
        char mod_path[512];
        int mp_len = 0;
        while (*s && *s != ';' && *s != '\n') {
          if (*s == ':' && s[1] == ':') {
            mod_path[mp_len++] = '/';
            s += 2;
          } else if (*s != ' ' && *s != '\t') {
            mod_path[mp_len++] = *s;
            s++;
          } else {
            s++;
          }
        }
        mod_path[mp_len] = '\0';
        if (mp_len > 0) {
          // Convert to "mod/path.lain"
          char full[1024];
          snprintf(full, sizeof(full), "%s.lain", mod_path);
          // Add to node's imports
          if (nodes[node_idx].import_count < MAX_IMPORTS)
            nodes[node_idx].imports[nodes[node_idx].import_count++] = strdup(full);
          // Recursively scan
          int dep_idx = find_node(full);
          if (nodes[dep_idx].scanned == 0) {
            nodes[dep_idx].scanned = 1;
            scan_file(full, dep_idx);
          }
        }
        while (*s && *s != ';') s++;
        if (*s == ';') s++;
      } else {
        s++;
      }
    }
  }

  // DFS topological sort
  int sorted[MAX_MODULES];
  int sorted_count = 0;

  int dfs(int idx) {
    if (nodes[idx].visited == 2) return 0; // already done
    if (nodes[idx].visited == 1) return -1; // cycle!
    nodes[idx].visited = 1;
    for (int j = 0; j < nodes[idx].import_count; j++) {
      // Find the node index for this import
      for (int k = 0; k < node_count; k++) {
        if (strcmp(nodes[k].path, nodes[idx].imports[j]) == 0) {
          if (dfs(k) != 0) return -1;
          break;
        }
      }
    }
    nodes[idx].visited = 2;
    sorted[sorted_count++] = idx;
    return 0;
  }

  // Seed: find or create root node
  int root_idx = find_node(root_path);
  nodes[root_idx].scanned = 1;
  scan_file(root_path, root_idx);

  // Topological sort all nodes
  for (int i = 0; i < node_count; i++) {
    if (dfs(i) != 0) {
      return sexp_user_exception(ctx, NULL, "build: circular dependency", SEXP_NULL);
    }
  }

  // Build Scheme list from sorted order (reverse to get correct order)
  sexp result = SEXP_NULL;
  for (int i = sorted_count - 1; i >= 0; i--) {
    int idx = sorted[i];
    result = sexp_cons(ctx, sexp_c_string(ctx, nodes[idx].path, -1), result);
  }

  // Cleanup
  for (int i = 0; i < node_count; i++) {
    free(nodes[i].path);
    for (int j = 0; j < nodes[i].import_count; j++)
      free(nodes[i].imports[j]);
  }

  return result;
#endif
}

static sexp sexp_core_execute_lainir(sexp ctx, sexp self, sexp_sint_t n,
                                     sexp arg_entry, sexp arg_args) {
  vm_context *c = (vm_context *)ctx;
  vm_value *entry_val = (vm_value *)arg_entry;
  vm_value *args_val = (vm_value *)arg_args;

  const char *entry_name = NULL;
  if (vm_is_symbol(entry_val))
    entry_name = vm_string_data((vm_value *)vm_symbol_name(c, entry_val));
  else if (vm_is_string(entry_val))
    entry_name = vm_string_data(entry_val);

  if (!entry_name) {
    return (sexp)vm_user_exception(c, "core.execute-lainir!: entry must be a symbol or string");
  }

  LainirExecRequest request = {
    .entry_name = entry_name,
    .args = args_val
  };
  vm_value *result = vm_false();
  LainirExecStatus status =
    lainir_exec_request(c, vm_context_env(c), &request, &result);

  return (sexp)result;
}

static vm_value *ffi_core_execute_lainir_text(
    vm_context *c, vm_value *self, intptr_t n,
    vm_value *text_val, vm_value *entry_val) {
  const char *entry_name = NULL;
  LainirValue result = lainir_value_unit();
  L1Diagnostic diagnostic;

  if (!vm_is_string(text_val))
    return vm_user_exception(c,
      "core.execute-lainir-text!: text must be a string");
  if (vm_is_symbol(entry_val))
    entry_name = vm_symbol_name(c, entry_val);
  else if (vm_is_string(entry_val))
    entry_name = vm_string_data(entry_val);
  if (!entry_name)
    return vm_user_exception(c,
      "core.execute-lainir-text!: entry must be a symbol or string");

  LainirExecTextRequest request = {
    .text = vm_string_data(text_val),
    .entry_name = entry_name,
    .args = NULL,
    .arg_count = 0,
    .host_ctx = NULL,
    .host_env = NULL,
  };
  if (lainir_exec_text_request(&request, &result, &diagnostic) != LAINIR_EXEC_OK) {
    if (diagnostic.message[0])
      fprintf(stderr, "[generated L1 ERROR %d] %s\n",
              diagnostic.code, diagnostic.message);
    return vm_user_exception(c,
      diagnostic.message[0] ? diagnostic.message :
                              "generated LAIN-IR execution failed");
  }
  if (result.kind == LAINIR_VALUE_BITS)
    return vm_make_integer(c, (int64_t)result.as.bits);
  if (result.kind == LAINIR_VALUE_STRING) {
    vm_value *value = vm_make_string(
      c, result.as.string ? result.as.string : "", -1);
    free((void *)result.as.string);
    return value;
  }
  if (result.kind == LAINIR_VALUE_UNIT)
    return vm_void();
  return vm_user_exception(c,
    "core.execute-lainir-text!: unsupported result kind");
}

static vm_value *ffi_core_execute_lainir_text_i32(
    vm_context *c, vm_value *self, intptr_t n,
    vm_value *text_val, vm_value *entry_val, vm_value *arg_val) {
  const char *entry_name = NULL;
  LainirValue result = lainir_value_unit();
  LainirValue arg;
  L1Diagnostic diagnostic;
  (void)self;
  (void)n;

  if (!vm_is_string(text_val))
    return vm_user_exception(c,
      "core.execute-lainir-text-i32!: text must be a string");
  if (vm_is_symbol(entry_val))
    entry_name = vm_symbol_name(c, entry_val);
  else if (vm_is_string(entry_val))
    entry_name = vm_string_data(entry_val);
  if (!entry_name)
    return vm_user_exception(c,
      "core.execute-lainir-text-i32!: entry must be a symbol or string");
  if (!vm_is_integer(arg_val))
    return vm_user_exception(c,
      "core.execute-lainir-text-i32!: argument must be an integer");

  arg = lainir_value_bits((uint32_t)vm_uint_value(arg_val), 32);
  LainirExecTextRequest request = {
    .text = vm_string_data(text_val),
    .entry_name = entry_name,
    .args = &arg,
    .arg_count = 1,
    .host_ctx = NULL,
    .host_env = NULL,
  };
  if (lainir_exec_text_request(&request, &result, &diagnostic) != LAINIR_EXEC_OK) {
    if (diagnostic.message[0])
      fprintf(stderr, "[generated L1 ERROR %d] %s\n",
              diagnostic.code, diagnostic.message);
    return vm_user_exception(c,
      diagnostic.message[0] ? diagnostic.message :
                              "generated LAIN-IR execution failed");
  }
  if (result.kind == LAINIR_VALUE_BITS)
    return vm_make_integer(c, (int64_t)result.as.bits);
  if (result.kind == LAINIR_VALUE_STRING) {
    vm_value *value = vm_make_string(
      c, result.as.string ? result.as.string : "", -1);
    free((void *)result.as.string);
    return value;
  }
  if (result.kind == LAINIR_VALUE_UNIT)
    return vm_void();
  return vm_user_exception(c,
    "core.execute-lainir-text-i32!: unsupported result kind");
}

static const char *const compiler_artifact_capabilities[] = {
  "compiler.storage-new!",
  "compiler.storage-reserve!",
  "compiler.storage-set-i32!",
  "compiler.storage-get-i32!",
  "compiler.storage-set-string!",
  "compiler.storage-get-string!",
  "compiler.storage-destroy!",
  "core.destroy-lainir-unit!",
  "core.string-append!",
  "core.string-from-addr!",
  "core.string-first-byte!",
  "core.string-length!",
  "core.i32-to-string!",
  "core.string-is-i32!",
  "core.string-to-i32!",
  "core.string-equal!",
  "ast.node-atom-class",
  "ast.node-line",
  "ast.node-origin",
  "ast.node-hygiene",
  "ast.node-is-infix-text",
  "ast.node-is-atom-text",
  "ast.node-text",
  "ast.node-string-value",
  "ast.node-next",
  "ast.node-op",
  "ast.node-right",
  "ast.node-left",
  "ast.node-kind",
  "ast.parse!",
  "ast.store-new!",
  "ast.store-destroy!",
  "ast.unit-parse!",
  "ast.unit-new-generated!",
  "ast.unit-atom!",
  "ast.unit-node!",
  "ast.unit-set-left!",
  "ast.unit-set-right!",
  "ast.unit-set-op!",
  "ast.unit-set-origin!",
  "ast.unit-set-hygiene!",
  "ast.unit-append!",
  "ast.unit-clone!",
  "ast.unit-seal!",
  "ast.unit-destroy!",
  "ast.unit-root",
  "ast.unit-node-count",
  "ast.unit-node-kind",
  "ast.unit-node-text",
  "ast.unit-node-string-value",
  "ast.unit-node-is-atom-text",
  "ast.unit-node-is-infix-text",
  "ast.unit-node-atom-class",
  "ast.unit-node-line",
  "ast.unit-node-col",
  "ast.unit-node-left",
  "ast.unit-node-right",
  "ast.unit-node-op",
  "ast.unit-node-next",
  "l1.call-arg!",
  "l1.expr-arg!",
  "l1.expr-binary!",
  "l1.expr-call!",
  "l1.expr-alloca!",
  "l1.expr-lea!",
  "l1.expr-load!",
  "l1.expr-i32!",
  "l1.expr-string!",
  "l1.expr-var!",
  "l1.proc-if-return!",
  "l1.proc-if-return-then!",
  "l1.block-if!",
  "l1.block-let!",
  "l1.block-return!",
  "l1.block-call!",
  "l1.block-store!",
  "l1.proc-let!",
  "l1.proc-new!",
  "l1.proc-extern!",
  "l1.proc-param!",
  "l1.proc-body!",
  "l1.proc-return!",
  "l1.proc-store!",
  "l1.unit-new!",
  "l1.unit-verify!",
  "l1.unit-verify-code!",
  "l1.unit-verify-message!",
  "core.lainir-unit-debug-text!",
  "l1.read-find-proc!",
  "l1.read-proc-is-extern!",
  "l1.read-proc-param-count!",
  "l1.read-proc-first-inst!",
  "l1.read-inst-next!",
  "l1.read-inst-kind!",
  "l1.read-inst-expr!",
  "l1.read-inst-name!",
  "l1.read-inst-branch!",
  "l1.read-expr-kind!",
  "l1.read-expr-i32!",
  "l1.read-expr-index!",
  "l1.read-expr-name!",
  "l1.read-expr-child!",
  "l1.read-call-arg-count!",
  "l1.read-call-arg!",
  "l1.eval-new!",
  "l1.eval-frame-new!",
  "l1.eval-frame-arg-set!",
  "l1.eval-frame-arg!",
  "l1.eval-frame-var-set!",
  "l1.eval-frame-var!",
  "l1.eval-fail!",
  "l1.eval-status!",
  "l1.eval-destroy!",
  "l1.result-new!",
  "l1.result-status!",
  "l1.result-value!",
  "l1.result-destroy!",
};

LainirExecStatus native_execute_compiler_artifact(
    void *ctx_ptr,
    const char *artifact_text,
    const char *entry_name,
    const LainirValue *args,
    uint32_t arg_count,
    LainirValue *result_out,
    L1Diagnostic *diagnostic) {
  vm_context *ctx = (vm_context *)ctx_ptr;
  LainirExecTextRequest request = {
    .text = artifact_text,
    .entry_name = entry_name,
    .args = args,
    .arg_count = arg_count,
    .host_ctx = ctx,
    .host_env = ctx ? vm_context_env(ctx) : NULL,
    .allowed_capabilities = compiler_artifact_capabilities,
    .allowed_capability_count =
      (uint32_t)(sizeof(compiler_artifact_capabilities) /
                 sizeof(compiler_artifact_capabilities[0])),
  };
  return lainir_exec_text_request(&request, result_out, diagnostic);
}

static vm_value *ffi_core_execute_compiler_artifact(
    vm_context *c, vm_value *self, intptr_t n,
    vm_value *artifact_val, vm_value *entry_val,
    vm_value *source_val, vm_value *length_val) {
  const char *entry_name = NULL;
  LainirValue result = lainir_value_unit();
  LainirValue args[2];
  L1Diagnostic diagnostic;
  (void)self;
  (void)n;
  if (!vm_is_string(artifact_val) || !vm_is_string(source_val) ||
      !vm_is_integer(length_val))
    return vm_user_exception(c,
      "core.execute-compiler-artifact!: expected artifact, entry, source, length");
  if (vm_is_symbol(entry_val))
    entry_name = vm_symbol_name(c, entry_val);
  else if (vm_is_string(entry_val))
    entry_name = vm_string_data(entry_val);
  if (!entry_name)
    return vm_user_exception(c,
      "core.execute-compiler-artifact!: entry must be a symbol or string");

  args[0] = lainir_value_string(vm_string_data(source_val));
  args[1] = lainir_value_bits((uint32_t)vm_uint_value(length_val), 32);
  if (native_execute_compiler_artifact(
        c, vm_string_data(artifact_val), entry_name, args, 2,
        &result, &diagnostic) != LAINIR_EXEC_OK) {
    if (diagnostic.message[0])
      fprintf(stderr, "[compiler artifact ERROR %d] %s\n",
              diagnostic.code, diagnostic.message);
    return vm_user_exception(c,
      diagnostic.message[0] ? diagnostic.message :
                              "compiler artifact execution failed");
  }
  if (result.kind == LAINIR_VALUE_STRING) {
    vm_value *value = vm_make_string(
      c, result.as.string ? result.as.string : "", -1);
    free((void *)result.as.string);
    return value;
  }
  if (result.kind == LAINIR_VALUE_BITS)
    return vm_make_integer(c, (int64_t)result.as.bits);
  return vm_user_exception(c,
    "core.execute-compiler-artifact!: unsupported result kind");
}

static vm_value *ffi_core_execute_compiler_artifact_named(
    vm_context *c, vm_value *self, intptr_t n,
    vm_value *artifact_val, vm_value *source_val,
    vm_value *length_val, vm_value *name_val) {
  const char *entry_name = "mini_meta_compile_named";
  LainirValue result = lainir_value_unit();
  LainirValue args[3];
  L1Diagnostic diagnostic;
  (void)self;
  (void)n;
  if (!vm_is_string(artifact_val) || !vm_is_string(source_val) ||
      !vm_is_integer(length_val) || !vm_is_string(name_val))
    return vm_user_exception(c,
      "core.execute-compiler-artifact-named!: expected artifact, source, length, name");

  args[0] = lainir_value_string(vm_string_data(source_val));
  args[1] = lainir_value_bits((uint32_t)vm_uint_value(length_val), 32);
  args[2] = lainir_value_string(vm_string_data(name_val));
  if (native_execute_compiler_artifact(
        c, vm_string_data(artifact_val), entry_name, args, 3,
        &result, &diagnostic) != LAINIR_EXEC_OK) {
    if (diagnostic.message[0])
      fprintf(stderr, "[compiler artifact ERROR %d] %s\n",
              diagnostic.code, diagnostic.message);
    return vm_user_exception(c,
      diagnostic.message[0] ? diagnostic.message :
                              "named compiler artifact execution failed");
  }
  if (result.kind == LAINIR_VALUE_STRING) {
    vm_value *value = vm_make_string(
      c, result.as.string ? result.as.string : "", -1);
    free((void *)result.as.string);
    return value;
  }
  if (result.kind == LAINIR_VALUE_BITS)
    return vm_make_integer(c, (int64_t)result.as.bits);
  return vm_user_exception(c,
    "core.execute-compiler-artifact-named!: unsupported result kind");
}

/* These are generic bootstrap text capabilities.  They do not understand L1
 * syntax: Lain owns the emitter grammar and only asks the host to concatenate
 * strings or render a primitive integer. */
static vm_value *ffi_core_string_append(
    vm_context *c, vm_value *self, intptr_t n,
    vm_value *left, vm_value *right) {
  const char *left_text;
  const char *right_text;
  size_t left_len;
  size_t right_len;
  char *joined;
  vm_value *result;

  (void)self;
  (void)n;
  if (!vm_is_string(left) || !vm_is_string(right))
    return vm_user_exception(c, "core.string-append!: expected two strings");
  left_text = vm_string_data(left);
  right_text = vm_string_data(right);
  left_len = strlen(left_text);
  right_len = strlen(right_text);
  joined = malloc(left_len + right_len + 1);
  if (!joined)
    return vm_user_exception(c, "core.string-append!: out of memory");
  memcpy(joined, left_text, left_len);
  memcpy(joined + left_len, right_text, right_len + 1);
  result = vm_make_string(c, joined, (int)(left_len + right_len));
  free(joined);
  return result;
}

/* Restore an owned VM string from a NUL-terminated address kept in a Lain
 * aggregate.  The aggregate stores the physical pointer, not the interpreter
 * STRING tag, so this conversion must be explicit at the host boundary. */
static vm_value *ffi_core_string_from_addr(
    vm_context *c, vm_value *self, intptr_t n, vm_value *value) {
  const char *text;
  (void)self;
  (void)n;
  text = (const char *)vm_cpointer_value(value);
  return vm_make_string(c, text ? text : "", -1);
}

/* Generic bootstrap text predicate.  This compares opaque source text only;
 * it deliberately has no knowledge of syntax nodes or L1 grammar. */
static vm_value *ffi_core_string_equal(
    vm_context *c, vm_value *self, intptr_t n,
    vm_value *left, vm_value *right) {
  (void)self;
  (void)n;
  if (!vm_is_string(left) || !vm_is_string(right))
    return vm_user_exception(c, "core.string-equal!: expected two strings");
  return vm_make_integer(c,
    strcmp(vm_string_data(left), vm_string_data(right)) == 0 ? 1 : 0);
}

static vm_value *ffi_core_string_length(
    vm_context *c, vm_value *self, intptr_t n, vm_value *value) {
  (void)self;
  (void)n;
  if (!vm_is_string(value))
    return vm_user_exception(c, "core.string-length!: expected a string");
  return vm_make_integer(c, (int64_t)strlen(vm_string_data(value)));
}

static vm_value *ffi_core_string_first_byte(
    vm_context *c, vm_value *self, intptr_t n, vm_value *value) {
  const unsigned char *text;
  (void)self;
  (void)n;
  if (!vm_is_string(value))
    return vm_user_exception(c, "core.string-first-byte!: expected a string");
  text = (const unsigned char *)vm_string_data(value);
  return vm_make_integer(c, text[0] ? (int64_t)text[0] : -1);
}

static vm_value *ffi_core_i32_to_string(
    vm_context *c, vm_value *self, intptr_t n, vm_value *value) {
  char text[32];
  (void)self;
  (void)n;
  if (!vm_is_integer(value))
    return vm_user_exception(c, "core.i32-to-string!: expected an integer");
  snprintf(text, sizeof(text), "%d", (int32_t)vm_uint_value(value));
  return vm_make_string(c, text, -1);
}

static vm_value *ffi_core_string_to_i32(
    vm_context *c, vm_value *self, intptr_t n, vm_value *text_value) {
  char *end = NULL;
  long value;
  (void)self;
  (void)n;
  if (!vm_is_string(text_value))
    return vm_user_exception(c, "core.string-to-i32!: expected a string");
  value = strtol(vm_string_data(text_value), &end, 10);
  if (!end || *end != '\0' || value < INT32_MIN || value > INT32_MAX)
    return vm_user_exception(c, "core.string-to-i32!: invalid i32 literal");
  return vm_make_integer(c, (int64_t)value);
}

static vm_value *ffi_core_string_is_i32(
    vm_context *c, vm_value *self, intptr_t n, vm_value *text_value) {
  char *end = NULL;
  long value;
  (void)self;
  (void)n;
  if (!vm_is_string(text_value))
    return vm_make_integer(c, 0);
  value = strtol(vm_string_data(text_value), &end, 10);
  return vm_make_integer(c,
    end && end != vm_string_data(text_value) && *end == '\0' &&
    value >= INT32_MIN && value <= INT32_MAX ? 1 : 0);
}

// ── Build driver: full multi-file build ──
// Steps: compute order (Scheme) → compile each module → gcc link
// compile_fn and interface_fn are provided by the caller (native_compiler.c)

int32_t native_build_with_funcs(const char *root_path, const char *output_path,
    uint32_t (*compile_fn)(const void*, const void*),
    uint32_t (*interface_fn)(const void*, const void*)) {
#ifdef LAIN_DISABLE_NATIVE_BUILD
  (void)root_path;
  (void)output_path;
  (void)compile_fn;
  (void)interface_fn;
  fprintf(stderr, "native --build is disabled in this host build\n");
  return 1;
#else
  fprintf(stderr, "[build] computing dependency graph for: %s\n", root_path);

  // Step 1: Initialize Scheme and compute build order
  void *ctx = native_init_scheme();
  if (!ctx) return 1;
  sexp sc = (sexp)ctx;
  sexp env = sexp_context_env(sc);
  sexp proc = sexp_env_ref(sc, env,
                           sexp_intern(sc, "core.build-compute-order!", -1),
                           SEXP_FALSE);
  sexp root_str = sexp_c_string(sc, root_path, -1);
  sexp order_list = sexp_apply(sc, proc, sexp_list1(sc, root_str));
  if (sexp_exceptionp(order_list)) {
    fprintf(stderr, "[build] compute-order failed\n");
    return 1;
  }

  // Step 2: Collect paths
  int n = 0;
  sexp curr = order_list;
  while (sexp_pairp(curr)) { n++; curr = sexp_cdr(curr); }
  fprintf(stderr, "[build] %d modules in order\n", n);

  char **src = malloc(sizeof(char*) * n);
  char **obj = malloc(sizeof(char*) * n);
  curr = order_list;
  for (int i = 0; i < n; i++) {
    src[i] = strdup(sexp_string_data(sexp_car(curr)));
    // Generate .o in /tmp/lain_build/
    const char *s = src[i], *slash = strrchr(s, '/');
    if (slash) s = slash + 1;
    char *stem = strdup(s);
    char *dot = strstr(stem, ".lain"); if (dot) *dot = 0;
    char buf[512];
    snprintf(buf, sizeof(buf), "/tmp/lain_build/%s.o", stem);
    obj[i] = strdup(buf);
    free(stem);
    curr = sexp_cdr(curr);
  }
  system("mkdir -p /tmp/lain_build 2>/dev/null");

  // Step 3: Generate interface + compile each module
  for (int i = 0; i < n; i++) {
    fprintf(stderr, "[build] %d/%d: %s\n", i+1, n, src[i]);
    // Interface goes next to source (so import resolution finds it)
    char ifc[1024];
    snprintf(ifc, sizeof(ifc), "%s.lci", src[i]);
    if (interface_fn(src[i], ifc) != 0) {
      fprintf(stderr, "[build] interface emission failed: %s\n", src[i]);
      for (int j = 0; j < n; j++) { free(src[j]); free(obj[j]); }
      free(src); free(obj);
      return 1;
    }
    // Compile to .c then gcc -c to .o
    char cf[1024];
    snprintf(cf, sizeof(cf), "/tmp/lain_build/tmp_%d.c", i);
    if (compile_fn(src[i], cf) != 0) {
      fprintf(stderr, "[build] module compile failed: %s\n", src[i]);
      for (int j = 0; j < n; j++) { free(src[j]); free(obj[j]); }
      free(src); free(obj);
      return 1;
    }
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "gcc -c %s -o %s -I. 2>&1", cf, obj[i]);
    if (system(cmd) != 0) {
      fprintf(stderr, "[build] object compile failed: %s\n", src[i]);
      for (int j = 0; j < n; j++) { free(src[j]); free(obj[j]); }
      free(src); free(obj);
      return 1;
    }
  }

  // Step 4: Link
  fprintf(stderr, "[build] linking → %s\n", output_path);
  char link[8192];
  int pos = snprintf(link, sizeof(link), "gcc -o %s ", output_path);
  for (int i = 0; i < n; i++)
    pos += snprintf(link + pos, sizeof(link) - pos, "%s ", obj[i]);
#if defined(LAIN_SCHEME_BACKEND_GAUCHE)
  fprintf(stderr,
          "[build] legacy C-side build is not supported by the Gauche backend\n");
  for (int i = 0; i < n; i++) { free(src[i]); free(obj[i]); }
  free(src); free(obj);
  return 1;
#else
  pos += snprintf(link + pos, sizeof(link) - pos,
    "src/compiler/native_runtime.c src/compiler/vm_chibi.c src/compiler/lainir_exec.c "
    "-Isrc -Ithird_party/chibi-scheme/include -DLAIN_SCHEME_BACKEND_CHIBI "
    "-Lthird_party/chibi-scheme -lchibi-scheme -lm -ldl "
    "-Wl,-rpath,$PWD/third_party/chibi-scheme");
#endif
  fprintf(stderr, "[build] %s\n", link);
  int ret = system(link);

  for (int i = 0; i < n; i++) { free(src[i]); free(obj[i]); }
  free(src); free(obj);
  return ret ? 1 : 0;
#endif
}

// Environment variable access
const char *native_getenv(const char *name) { return getenv(name); }

// ── Deferred lex: store source for later Scheme-side lexing ──────────────
// native_lex_and_group is called before native_init_scheme, so we can't
// use the Scheme context yet. Store a COPY of the source (because
// native_init_scheme calls native_read_file which frees g_file_buffer).
// Actual lexing happens in native_run_pipeline.

static uint8_t *g_pending_src = NULL;
static uint32_t g_pending_len = 0;
static int g_interpret_source_linking = 0;

void native_set_interpret_source_linking(int enabled) {
  g_interpret_source_linking = enabled ? 1 : 0;
}

void *native_lex_and_group(const uint8_t *src, uint32_t len) {
  // Free previous copy if any
  if (g_pending_src) {
    free(g_pending_src);
    g_pending_src = NULL;
  }
  // Copy the source — the original buffer may be freed by later
  // native_read_file calls (e.g. during native_init_scheme).
  g_pending_src = malloc(len);
  memcpy(g_pending_src, src, len);
  g_pending_len = len;
  return (void *)1; // dummy non-NULL
}

// ── Scheme FFI wrappers for module prefix (must be before native_init_scheme) ──

// Forward declarations for native functions defined later
void native_set_source_path(const char *path);
const char *native_get_module_prefix(void);

static sexp sexp_set_module_prefix(sexp ctx, sexp self, sexp_sint_t n,
                                    sexp arg_path) {
  const char *path = sexp_string_data(arg_path);
  native_set_source_path(path);
  return SEXP_VOID;
}

static sexp sexp_get_module_prefix(sexp ctx, sexp self, sexp_sint_t n) {
  const char *prefix = native_get_module_prefix();
  return sexp_c_string(ctx, prefix, -1);
}

static sexp sexp_core_emit_l1(
    sexp ctx, sexp self, sexp_sint_t n, sexp arg_subs, sexp arg_path);
static sexp sexp_core_emit_interface(
    sexp ctx, sexp self, sexp_sint_t n, sexp arg_path);

void native_register_runtime_ffi(
    native_foreign_registrar registrar, void *user_data) {
#define REG(name, args, fn) registrar(user_data, name, args, (void *)(fn))
  REG("core.emit-l1!", 2, sexp_core_emit_l1);
  REG("core.read-file-forms!", 1, sexp_read_file_forms);
  REG("core.read-file-string!", 1, sexp_read_file_string);
  REG("core.write-file-string!", 2, sexp_write_file_string);
  REG("core.read-interface!", 1, sexp_read_interface);
  REG("core.build-compute-order!", 1, sexp_build_compute_order);
  REG("core.set-module-prefix!", 1, sexp_set_module_prefix);
  REG("core.module-prefix", 0, sexp_get_module_prefix);
  REG("core.emit-interface!", 1, sexp_core_emit_interface);
  REG("core.execute-lainir!", 2, sexp_core_execute_lainir);
  REG("core.execute-lainir-text!", 2, ffi_core_execute_lainir_text);
  REG("core.execute-lainir-text-i32!", 3, ffi_core_execute_lainir_text_i32);
  REG("core.execute-compiler-artifact!", 4, ffi_core_execute_compiler_artifact);
  REG("core.execute-compiler-artifact-named!", 4,
      ffi_core_execute_compiler_artifact_named);
  REG("core.string-append!", 2, ffi_core_string_append);
  REG("core.string-append-linear!", 2, ffi_core_string_append);
  REG("core.string-from-addr!", 1, ffi_core_string_from_addr);
  REG("core.string-equal!", 2, ffi_core_string_equal);
  REG("core.string-length!", 1, ffi_core_string_length);
  REG("core.string-first-byte!", 1, ffi_core_string_first_byte);
  REG("core.i32-to-string!", 1, ffi_core_i32_to_string);
  REG("core.string-to-i32!", 1, ffi_core_string_to_i32);
  REG("core.string-is-i32!", 1, ffi_core_string_is_i32);
#undef REG
}

typedef struct {
  sexp ctx;
  sexp env;
} NativeVmRegistrarCtx;

static void native_register_foreign_with_vm(
    void *user_data, const char *name, int arity, void *fn) {
  NativeVmRegistrarCtx *state = (NativeVmRegistrarCtx *)user_data;
  vm_register_ffi((vm_context *)state->ctx, (vm_value *)state->env,
                  name, arity, (vm_ffi_fn)fn);
}

static void *native_init_vm_host(int load_stage0_meta) {
  // Set module path for Chibi (computed from cwd)
  {
    char cwd[2048];
    if (getcwd(cwd, sizeof(cwd))) {
      char *p = strstr(cwd, "/src/compiler");
      if (p) *p = '\0';
      char buf[2048];
      snprintf(buf, sizeof(buf), "%s/third_party/chibi-scheme/lib", cwd);
      vm_set_module_path(buf);
    }
  }

  sexp ctx = (sexp)vm_create();
  sexp env = (sexp)vm_context_env((vm_context *)ctx);

  // Register core FFI functions (Layer B)
  NativeVmRegistrarCtx reg = {.ctx = ctx, .env = env};
  native_register_core_ffi(ctx, env, native_register_foreign_with_vm, &reg);
  native_register_structured_unit_ffi(
      ctx, env, native_register_foreign_with_vm, &reg);
  native_register_runtime_ffi(native_register_foreign_with_vm, &reg);

  if (load_stage0_meta) {
    // Stage-0 only: the self-hosted compiler artifact does not execute these
    // Scheme sources and must remain usable outside the repository root.
    native_inject_all_polyfills(ctx, env);
    native_load_meta_sources(ctx, env);
  }

  return ctx;
}

// Initialize Scheme environment, register all FFI functions, load meta passes
void *native_init_scheme(void) {
  return native_init_vm_host(1);
}

void *native_init_compiler_artifact_host(void) {
  return native_init_vm_host(0);
}

// Run the Scheme pipeline on a token tree
int32_t native_run_pipeline(void *ctx_ptr, void *root_group) {
  vm_context *c = (vm_context *)ctx_ptr;
  lainir_reset_module_state();
  native_reset_interface_metadata();
  (void)root_group;

  vm_value *env = vm_context_env(c);

  // Clear previous compilation state through the meta-owned API.
  vm_eval_string(c, env, "(compiler-state.reset!)");
  vm_eval_string(c, env, "(set! *all-declarations* '())");
  vm_eval_string(c, env,
                 g_interpret_source_linking
                     ? "(import.configure-source-linking! #t)"
                     : "(import.configure-source-linking! #f)");

  vm_value *src_str = vm_make_string(c, (const char *)g_pending_src, (int)g_pending_len);
  vm_value *out_path = vm_make_string(c, "", 0);
  vm_value *compile_sym = vm_intern(c, "compile");
  vm_value *proc = vm_env_ref(c, env, compile_sym);
  if (!vm_is_procedure(proc)) {
    fprintf(stderr, "[pipeline ERROR] compile entry point is not registered\n");
    return 1;
  }

  vm_value *result = vm_apply(c, proc, vm_list2(c, src_str, out_path));
  if (vm_is_exception(result)) {
    fprintf(stderr, "[pipeline ERROR] ");
    vm_print_exception(c, result);
    return 1;
  }

  return 0;
}

void native_emit_module_to_file(void *subs_ptr, const char *output_path) {
  (void)subs_ptr;
  lainir_emit_c_module_to_file(g_subroutines_head, output_path, 1);
}

void native_emit_l1_module(const char *output_path) {
  lainir_emit_text_module_to_file(g_subroutines_head, output_path);
}

static sexp sexp_core_emit_l1(sexp ctx, sexp self, sexp_sint_t n,
                              sexp arg_subs, sexp arg_path) {
  const char *path = sexp_string_data(arg_path);
  (void)self;
  (void)n;
  (void)arg_subs;
  native_emit_l1_module(path);
  return SEXP_VOID;
}

typedef struct NativeInterfaceField {
  char *name;
  char *type_name;
  uint32_t offset;
} NativeInterfaceField;

typedef struct NativeInterfaceType {
  char *name;
  char *identity;
  uint32_t size;
  uint32_t align;
  uint32_t field_count;
  NativeInterfaceField *fields;
  struct NativeInterfaceType *next;
} NativeInterfaceType;

typedef struct NativeInterfaceFunction {
  char *name;
  uint32_t param_count;
  char **param_types;
  char *ret_type;
  struct NativeInterfaceFunction *next;
} NativeInterfaceFunction;

static NativeInterfaceType *g_interface_types_head = NULL;
static NativeInterfaceType *g_interface_types_tail = NULL;
static NativeInterfaceFunction *g_interface_functions_head = NULL;
static NativeInterfaceFunction *g_interface_functions_tail = NULL;

static char *native_interface_strdup(const char *s) {
  size_t n = strlen(s ? s : "");
  char *copy = (char *)malloc(n + 1);
  if (!copy) abort();
  memcpy(copy, s ? s : "", n + 1);
  return copy;
}

void native_reset_interface_metadata(void) {
  NativeInterfaceType *type = g_interface_types_head;
  NativeInterfaceFunction *fn = g_interface_functions_head;
  while (type) {
    NativeInterfaceType *next = type->next;
    uint32_t i;
    free(type->name);
    free(type->identity);
    for (i = 0; i < type->field_count; ++i) {
      free(type->fields[i].name);
      free(type->fields[i].type_name);
    }
    free(type->fields);
    free(type);
    type = next;
  }
  while (fn) {
    NativeInterfaceFunction *next = fn->next;
    uint32_t i;
    free(fn->name);
    for (i = 0; i < fn->param_count; ++i)
      free(fn->param_types[i]);
    free(fn->param_types);
    free(fn->ret_type);
    free(fn);
    fn = next;
  }
  g_interface_types_head = g_interface_types_tail = NULL;
  g_interface_functions_head = g_interface_functions_tail = NULL;
}

void native_declare_interface_type(
    const char *name, const char *identity, uint32_t size, uint32_t align,
    uint32_t field_count, const char *const *field_names,
    const char *const *field_types, const uint32_t *field_offsets) {
  NativeInterfaceType *entry =
      (NativeInterfaceType *)calloc(1, sizeof(NativeInterfaceType));
  uint32_t i;
  if (!entry) abort();
  entry->name = native_interface_strdup(name);
  entry->identity = native_interface_strdup(identity);
  entry->size = size;
  entry->align = align;
  entry->field_count = field_count;
  entry->fields = (NativeInterfaceField *)calloc(
      field_count ? field_count : 1, sizeof(NativeInterfaceField));
  if (!entry->fields) abort();
  for (i = 0; i < field_count; ++i) {
    entry->fields[i].name = native_interface_strdup(field_names[i]);
    entry->fields[i].type_name = native_interface_strdup(field_types[i]);
    entry->fields[i].offset = field_offsets[i];
  }
  if (g_interface_types_tail)
    g_interface_types_tail->next = entry;
  else
    g_interface_types_head = entry;
  g_interface_types_tail = entry;
}

void native_declare_interface_function(
    const char *name, uint32_t param_count,
    const char *const *param_types, const char *ret_type) {
  NativeInterfaceFunction *entry =
      (NativeInterfaceFunction *)calloc(1, sizeof(NativeInterfaceFunction));
  uint32_t i;
  if (!entry) abort();
  entry->name = native_interface_strdup(name);
  entry->param_count = param_count;
  entry->param_types =
      (char **)calloc(param_count ? param_count : 1, sizeof(char *));
  if (!entry->param_types) abort();
  for (i = 0; i < param_count; ++i)
    entry->param_types[i] = native_interface_strdup(param_types[i]);
  entry->ret_type = native_interface_strdup(ret_type);
  if (g_interface_functions_tail)
    g_interface_functions_tail->next = entry;
  else
    g_interface_functions_head = entry;
  g_interface_functions_tail = entry;
}

static NativeInterfaceFunction *native_interface_function(const char *name) {
  NativeInterfaceFunction *entry;
  for (entry = g_interface_functions_head; entry; entry = entry->next)
    if (strcmp(entry->name, name) == 0)
      return entry;
  return NULL;
}

void native_emit_interface(const char *output_path) {
  FILE *out = fopen(output_path, "w");
  if (!out) {
    fprintf(stderr, "Error: cannot open interface output: %s\n", output_path);
    return;
  }

  const char *prefix = native_get_module_prefix();
  fprintf(out, "(module %s\n", prefix[0] ? prefix : "unknown");
  fprintf(out, "  (format lci-v2)\n");
  fprintf(out, "  (exports\n");

  for (NativeInterfaceType *type = g_interface_types_head; type;
       type = type->next) {
    uint32_t i;
    fprintf(out, "    (type\n");
    fprintf(out, "      (name %s)\n", type->name);
    fprintf(out, "      (identity \"%s\")\n", type->identity);
    fprintf(out, "      (size %u)\n", type->size);
    fprintf(out, "      (align %u)\n", type->align);
    fprintf(out, "      (fields\n");
    for (i = 0; i < type->field_count; ++i)
      fprintf(out,
              "        (field (name %s) (type %s) (offset %u))\n",
              type->fields[i].name, type->fields[i].type_name,
              type->fields[i].offset);
    fprintf(out, "      ))\n");
  }

  for (L1Subroutine *sub = g_subroutines_head; sub; sub = sub->next) {
    int exported = 0;
    NativeInterfaceFunction *semantic;
    if (native_has_explicit_exports())
      exported = sub->link_name && native_is_export_marked(sub->name);
    else
      exported = sub->link_name && !sub->is_extern && sub->blocks;

    if (!exported)
      continue;

    semantic = native_interface_function(sub->name);

    fprintf(out, "    (fn\n");
    fprintf(out, "      (name %s)\n", sub->name);
    fprintf(out, "      (params (");
    for (uint32_t i = 0; i < sub->param_count; i++) {
      if (semantic && i < semantic->param_count)
        fprintf(out, "%s", semantic->param_types[i]);
      else
        emit_l1_type(sub->param_tys[i], out);
      if (i < sub->param_count - 1)
        fprintf(out, " ");
    }
    fprintf(out, "))\n");
    fprintf(out, "      (ret ");
    if (semantic)
      fprintf(out, "%s", semantic->ret_type);
    else
      emit_l1_type(sub->ret_ty, out);
    fprintf(out, ")\n");
    fprintf(out, "      (abi_params (");
    for (uint32_t i = 0; i < sub->param_count; i++) {
      emit_l1_type(sub->param_tys[i], out);
      if (i < sub->param_count - 1)
        fprintf(out, " ");
    }
    fprintf(out, "))\n");
    fprintf(out, "      (abi_ret ");
    emit_l1_type(sub->ret_ty, out);
    fprintf(out, ")\n");
    fprintf(out, "      (link_name \"%s\"))\n", sub->link_name);
  }

  for (L1ExportName *entry = g_declared_signature_names_head; entry; entry = entry->next) {
    if (native_is_export_marked(entry->name))
      fprintf(out, "    (signature (name %s))\n", entry->name);
  }
  for (L1ExportName *entry = g_declared_module_names_head; entry; entry = entry->next) {
    if (native_is_export_marked(entry->name))
      fprintf(out, "    (module (name %s))\n", entry->name);
  }
  fprintf(out, "  ))\n");
  fclose(out);
}

static sexp sexp_core_emit_interface(sexp ctx, sexp self, sexp_sint_t n,
                                     sexp arg_path) {
  const char *path = sexp_string_data(arg_path);
  (void)self;
  (void)n;
  native_emit_interface(path);
  return SEXP_VOID;
}

// Get subroutine list head (for Lain code to inspect)
void *native_get_subroutines(void) { return g_subroutines_head; }

// ── Command-line argument access ─────────────────────────────────────────────

static int g_native_argc = 0;
static char **g_native_argv = NULL;

void native_set_args(int argc, char **argv) {
  g_native_argc = argc;
  g_native_argv = argv;
}

int32_t native_get_arg_count(void) { return g_native_argc; }

const char *native_get_arg(int32_t idx) {
  if (idx < 0 || idx >= g_native_argc)
    return NULL;
  return g_native_argv[idx];
}

// ── Module name extraction for @foreign(lain) name mangling ───────────────────

static const char *g_source_path = NULL;
static char g_module_prefix[256] = "";

void native_set_source_path(const char *path) {
  g_source_path = path;
  // Extract module name: strip directory + .lain extension
  const char *slash = strrchr(path, '/');
  const char *backslash = strrchr(path, '\\');
  const char *basename = slash > backslash ? slash : backslash;
  if (!basename) basename = path;
  else basename++;
  const char *dot = strrchr(basename, '.');
  size_t len = dot ? (size_t)(dot - basename) : strlen(basename);
  if (len > 200) len = 200;
  memcpy(g_module_prefix, basename, len);
  g_module_prefix[len] = '\0';
}

const char *native_get_module_prefix(void) {
  if (g_module_prefix[0] == '\0') return "";
  return g_module_prefix;
}
