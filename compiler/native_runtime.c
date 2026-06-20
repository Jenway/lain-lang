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
 *   - Chibi-Scheme (libchibi)
 */

#include <alloca.h>
#include <unistd.h>
#include "l1_types.h"


// ══════════════════════════════════════════════════════════════════════════════
// 2. read_byte_at — single byte from raw pointer (used by lexer.lain)
// ══════════════════════════════════════════════════════════════════════════════

uint8_t read_byte_at(const uint8_t *ptr, size_t offset) { return ptr[offset]; }

// C-Side Lexer (REMOVED — replaced by std/meta/lexer.scm)
// All lexing, grouping, form-splitting, and token-to-sexp conversion
// now happens in pure Scheme via meta.lex-source! and native_lex_to_sexp.

// core.lex-to-sexp! — uses Scheme-side lexer
void *native_lex_to_sexp(void *ctx_ptr, const uint8_t *src, uint32_t len) {
  sexp ctx = (sexp)ctx_ptr;
  sexp src_str = sexp_c_string(ctx, (const char *)src, len);
  sexp len_val = sexp_make_integer(ctx, sexp_string_length(src_str));
  sexp env = sexp_context_env(ctx);
  sexp lex_proc = sexp_env_ref(ctx, env,
                               sexp_intern(ctx, "meta.lex-source!", -1),
                               SEXP_FALSE);
  sexp form_list = sexp_apply(ctx, lex_proc, sexp_list2(ctx, src_str, len_val));

  if (sexp_exceptionp(form_list))
    return SEXP_FALSE;

  // meta.lex-source! returns a list of form trees: ((root ...) (root ...))
  // For compatibility with old API (single tree), return the first form
  // if there's only one, otherwise wrap all in a root.
  if (sexp_pairp(form_list) && sexp_nullp(sexp_cdr(form_list)))
    return sexp_car(form_list);

  // Multiple forms: wrap in a root
  sexp result = sexp_cons(ctx, sexp_intern(ctx, "root", -1), form_list);
  return result;
}


// ══════════════════════════════════════════════════════════════════════════════
// 7-8. L1 IR Builder FFI (→ l1_builder.c)
// ══════════════════════════════════════════════════════════════════════════════
#include "l1_builder.c"

// ══════════════════════════════════════════════════════════════════════════════
// 9. C Code Emission (→ l1_emit_c.c)
// ══════════════════════════════════════════════════════════════════════════════
#include "l1_emit_c.c"

// ══════════════════════════════════════════════════════════════════════════════
// 9.5. L1 IR Text Dump (→ l1_emit_text.c)
// ══════════════════════════════════════════════════════════════════════════════
#include "l1_emit_text.c"

// ══════════════════════════════════════════════════════════════════════════════
// 11. Scheme Initialization + FFI Registration
// ══════════════════════════════════════════════════════════════════════════════

// ============================================================================
// 11. Scheme Initialization and Meta Source Loading
// ============================================================================

static void check_exception(sexp ctx, sexp res) {
  if (sexp_exceptionp(res)) {
    sexp_print_exception(ctx, res, sexp_current_error_port(ctx));
    fprintf(stderr, "\n");
    exit(1);
  }
}

static void native_eval_string(sexp ctx, sexp env, const char *code) {
  sexp res = sexp_eval_string(ctx, code, -1, env);
  check_exception(ctx, res);
}

static void native_load_file(sexp ctx, sexp env, const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "Warning: cannot open file: %s\n", path);
    return;
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc(len + 1);
  if (!buf) {
    fprintf(stderr, "OOM reading %s\n", path);
    exit(1);
  }
  fread(buf, 1, len, f);
  buf[len] = '\0';
  fclose(f);
  // Wrap in (begin ...) so ALL top-level expressions are evaluated.
  // sexp_eval_string only evaluates the first expression otherwise.
  int wrapped_len = len + 9; // "(begin " + content + ")" + NUL
  char *wrapped = malloc(wrapped_len);
  if (!wrapped) {
    fprintf(stderr, "OOM wrapping %s\n", path);
    exit(1);
  }
  sprintf(wrapped, "(begin %s)", buf);
  free(buf);
  sexp res = sexp_eval_string(ctx, wrapped, -1, env);
  check_exception(ctx, res);
  free(wrapped);
}

static void native_inject_all_polyfills(sexp ctx, sexp env) {
#ifdef MINI_EVAL_MODE
  // In mini_eval mode, load polyfills from file using mini_load_file
  // (import is not needed — mini_eval uses flat namespace)
  mini_load_file(ctx, env, "polyfills.scm");
#else
  // Chibi mode: import R7RS, then load polyfills
  native_eval_string(ctx, env,
                     "(import (scheme base) (scheme cxr) (scheme load))");
  native_load_file(ctx, env, "polyfills.scm");
#endif
}

static void native_load_meta_sources(sexp ctx, sexp env) {
  // Load the single driver entry point instead of maintaining a duplicate
  // hardcoded file list. driver.scm has the authoritative load order.
  const char *search_paths[] = {"std/meta/driver.scm", "../std/meta/driver.scm",
                                "../../std/meta/driver.scm", NULL};
  for (int si = 0; search_paths[si]; si++) {
    FILE *test = fopen(search_paths[si], "r");
    if (test) {
      fclose(test);
      native_load_file(ctx, env, search_paths[si]);
      return;
    }
  }
  fprintf(stderr, "Warning: could not find std/meta/driver.scm\n");
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
}

// ── Build driver: full multi-file build ──
// Steps: compute order (Scheme) → compile each module → gcc link
// compile_fn and interface_fn are provided by the caller (native_compiler.c)

int32_t native_build_with_funcs(const char *root_path, const char *output_path,
    uint32_t (*compile_fn)(const void*, const void*),
    uint32_t (*interface_fn)(const void*, const void*)) {
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
    interface_fn(src[i], ifc);
    // Compile to .c then gcc -c to .o
    char cf[1024];
    snprintf(cf, sizeof(cf), "/tmp/lain_build/tmp_%d.c", i);
    compile_fn(src[i], cf);
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "gcc -c %s -o %s -I. 2>&1", cf, obj[i]);
    system(cmd);
  }

  // Step 4: Link
  fprintf(stderr, "[build] linking → %s\n", output_path);
  char link[8192];
  int pos = snprintf(link, sizeof(link), "gcc -o %s ", output_path);
  for (int i = 0; i < n; i++)
    pos += snprintf(link + pos, sizeof(link) - pos, "%s ", obj[i]);
  pos += snprintf(link + pos, sizeof(link) - pos,
    "compiler/native_runtime.c "
    "-Ibootstrap/chibi-scheme/include "
    "-Lbootstrap/chibi-scheme -lchibi-scheme -lm -ldl "
    "-Wl,-rpath,$PWD/bootstrap/chibi-scheme");
  fprintf(stderr, "[build] %s\n", link);
  int ret = system(link);

  for (int i = 0; i < n; i++) { free(src[i]); free(obj[i]); }
  free(src); free(obj);
  return ret ? 1 : 0;
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

// Initialize Scheme environment, register all FFI functions, load meta passes
void *native_init_scheme(void) {
  // Set CHIBI_MODULE_PATH so (import (scheme ...)) can find libraries
  {
    char cwd[2048];
    if (getcwd(cwd, sizeof(cwd))) {
      char project_root[2048];
      strcpy(project_root, cwd);
      // Strip /compiler suffix to find project root
      char *p;
      if ((p = strstr(project_root, "/compiler")))
        *p = '\0';
      char module_path[2048];
      snprintf(module_path, sizeof(module_path),
               "%s/bootstrap/chibi-scheme/lib", project_root);
      setenv("CHIBI_MODULE_PATH", module_path, 1);
    }
  }

  sexp ctx = sexp_make_eval_context(NULL, NULL, NULL, 0, 0);
  fprintf(stderr, "[init] 1: ctx created\n");
  sexp_load_standard_env(ctx, NULL, SEXP_SEVEN);
  fprintf(stderr, "[init] 2: std env loaded\n");
  sexp env = sexp_context_env(ctx);

  // Register core FFI functions (Layer B) — must be first because
  // polyfills (Layer A) contain type.* wrappers that reference these.
#define REG(name, args, fn) sexp_define_foreign(ctx, env, name, args, fn)
  REG("core.make-bits", 1, sexp_core_make_bits);
  REG("core.make-addr", 0, sexp_core_make_addr);
  REG("core.make-unit", 0, sexp_core_make_unit);
  REG("core.make-floats", 1, sexp_core_make_floats);
  REG("core.make-simd", 2, sexp_core_make_simd);
  REG("core.make-product-type!", 1, sexp_core_make_product_type);
  REG("type.registered-raw", 1, sexp_type_registered);
  REG("core.make-set", 2, sexp_core_make_set);
  REG("core.make-proc", 4, sexp_core_make_proc);
  REG("core.const-bits!", 3, sexp_core_const_bits);
  REG("core.const-string!", 3, sexp_core_const_string);
  REG("core.load!", 3, sexp_core_load);
  REG("core.store!", 3, sexp_core_store);
  REG("core.begin-function!", 3, sexp_core_begin_function);
  REG("core.function-by-name", 1, sexp_core_function_by_name);
  REG("core.append-block!", 1, sexp_core_append_block);
  REG("core.return-value!", 2, sexp_core_return_value);
  REG("core.return-none!", 1, sexp_core_return_none);
  REG("core.function-return-type", 1, sexp_core_function_return_type);
  REG("core.function-param-types", 1, sexp_core_function_param_types);
  REG("core.function-link-name", 1, sexp_core_function_link_name);
  REG("core.call!", 3, sexp_core_call);
  REG("core.primitive!", 4, sexp_core_primitive);
  REG("core.local-alloc!", 3, sexp_core_local_alloc);
  REG("core.param", 2, sexp_core_param);
  REG("core.block-function", 1, sexp_core_block_function);
  REG("core.branch!", 2, sexp_core_branch);
  REG("core.cond-branch!", 4, sexp_core_cond_branch);
  REG("core.type-is-void!", 1, sexp_core_type_is_void);
  REG("core.type-size-in-bytes!", 1, sexp_core_type_size);
  REG("core.field-offset!", 4, sexp_core_field_offset);
  REG("core.aggregate-layout!", 3, sexp_core_aggregate_layout);
  REG("core.call-indirect!", 4, sexp_core_call_indirect);
  REG("core.function-ref!", 1, sexp_core_function_ref);
  REG("core.declare-extern-function!", 4, sexp_core_declare_extern_function);
  REG("core.call-expr!", 3, sexp_core_call_expr);
  REG("core.set-current-block!", 1, sexp_core_set_current_block);
  REG("core.get-current-block", 0, sexp_core_get_current_block);
  REG("core.begin-if!", 2, sexp_core_begin_if);
  REG("core.end-if!", 4, sexp_core_end_if);
  REG("core.assign-temp!", 2, sexp_core_assign_temp);
  REG("core.emit-l1!", 2, sexp_core_emit_l1);
  REG("core.lex-to-sexp!", 2, sexp_lex_to_sexp);
  REG("core.read-file-forms!", 1, sexp_read_file_forms);
  REG("core.read-file-string!", 1, sexp_read_file_string);
  REG("core.read-interface!", 1, sexp_read_interface);
  REG("core.build-compute-order!", 1, sexp_build_compute_order);
  REG("core.set-module-prefix!", 1, sexp_set_module_prefix);
  REG("core.module-prefix", 0, sexp_get_module_prefix);
  REG("core.set-function-link-name!", 2, sexp_core_set_function_link_name);
  REG("core.declare-module!", 1, sexp_core_declare_module);
  REG("core.declare-signature!", 1, sexp_core_declare_signature);
  REG("core.mark-export!", 1, sexp_core_mark_export);
  REG("core.emit-interface!", 1, sexp_core_emit_interface);

#undef REG
  fprintf(stderr, "[init] 3: FFI registered\n");

  // Inject all polyfills (Layer A) — must be after FFI REG because
  // type.* wrappers reference core.* functions.
  native_inject_all_polyfills(ctx, env);
  fprintf(stderr, "[init] 4: polyfills loaded\n");

  // Smoke-test define-pass (validates polyfill layer works)
  {
    sexp r = sexp_eval_string(ctx,
                              "(begin (define-pass (form-parser test-pass "
                              "form) unit) (null? __lain-passes))",
                              -1, env);
    if (r == SEXP_FALSE)
      fprintf(stderr, "[diag] define-pass works!\n");
    else if (r == SEXP_TRUE)
      fprintf(stderr, "[diag] define-pass did NOT populate __lain-passes\n");
    else {
      fprintf(stderr, "[diag] define-pass test exception: ");
      sexp_print_exception(ctx, r, sexp_current_error_port(ctx));
      fprintf(stderr, "\n");
    }
  }

  // Load meta sources (define-pass registrations happen here)
  native_load_meta_sources(ctx, env);
  fprintf(stderr, "[init] 5: meta sources loaded\n");

  // compile-group-to-core is now loaded directly from std/meta/pipeline/driver.scm.
  // Smoke test: lex-to-sexp produces correct S-expression
  {
    const char *test_src = "fn main() -> i32 { 42 }";
    sexp tree = (sexp)native_lex_to_sexp(ctx, (const uint8_t *)test_src, strlen(test_src));
    fprintf(stderr, "[init] 6: lex-to-sexp smoke test: %s\n",
            sexp_exceptionp(tree) ? "FAILED" : "OK");
    if (!sexp_exceptionp(tree)) {
      sexp out = sexp_open_output_string(ctx);
      sexp_write(ctx, tree, out);
      sexp str = sexp_get_output_string(ctx, out);
      fprintf(stderr, "[init] 6:   => %s\n", sexp_string_data(str));
      sexp_close_port(ctx, out);
    }
  }
  // Just verify the key entry point is available.
  {
    sexp sym = sexp_intern(ctx, "compile-group-to-core", -1);
    sexp val = sexp_env_ref(ctx, env, sym, SEXP_FALSE);
    fprintf(stderr, "[init] 6a: compile-group-to-core %s\n",
            val == SEXP_FALSE      ? "MISSING"
            : sexp_procedurep(val) ? "is proc"
                                   : "is defined but not proc");
  }

  return ctx;
}

// Run the Scheme pipeline on a token tree
int32_t native_run_pipeline(void *ctx_ptr, void *root_group) {
  sexp ctx = (sexp)ctx_ptr;
  g_subroutines_head = NULL;
  g_export_names_head = NULL;
  g_declared_module_names_head = NULL;
  g_declared_signature_names_head = NULL;
  g_has_explicit_exports = 0;

  sexp env = sexp_context_env(ctx);

  // Clear previous declarations
  sexp_eval_string(ctx, "(set! *lain-declarations* (list))", -1, env);
  sexp_eval_string(ctx, "(set! *all-declarations* '())", -1, env);

  // Use Scheme-side lexer (meta.lex-source!) to tokenize + group + split
  // into forms. This replaces the old C-side lex_one_token_from_mem +
  // group_tokens_recursive + split_root_group + token_to_sexp pipeline.
  sexp src_str = sexp_c_string(ctx, (const char *)g_pending_src, g_pending_len);
  sexp len_val = sexp_make_integer(ctx, sexp_string_length(src_str));

  sexp lex_proc = sexp_env_ref(ctx, env,
                               sexp_intern(ctx, "meta.lex-source!", -1),
                               SEXP_FALSE);
  sexp form_list = sexp_apply(ctx, lex_proc, sexp_list2(ctx, src_str, len_val));

  if (sexp_exceptionp(form_list)) {
    fprintf(stderr, "[pipeline ERROR] Scheme lexer failed:\n");
    sexp_print_exception(ctx, form_list, sexp_current_error_port(ctx));
    fprintf(stderr, "\n");
    return 1;
  }

  sexp compile_sym = sexp_intern(ctx, "compile-group-to-core", -1);
  sexp proc = sexp_env_ref(ctx, env, compile_sym, SEXP_FALSE);

  // Iterate over form list produced by meta.lex-source!
  sexp_preserve_object(ctx, form_list);
  for (sexp forms = form_list; sexp_pairp(forms); forms = sexp_cdr(forms)) {
    sexp form = sexp_car(forms);
    sexp result = sexp_apply(ctx, proc, sexp_list1(ctx, form));

    if (sexp_exceptionp(result)) {
      fprintf(stderr, "[pipeline ERROR] ");
      sexp_print_exception(ctx, result, sexp_current_error_port(ctx));
      fprintf(stderr, "\n");
      sexp_release_object(ctx, form_list);
      return 1;
    }
  }
  sexp_release_object(ctx, form_list);

  return 0;
}

// Emit all subroutines to a file
void native_emit_module_to_file(void *subs_ptr, const char *output_path) {
  FILE *out = fopen(output_path, "w");
  if (!out) {
    fprintf(stderr, "Error: cannot open output file: %s\n", output_path);
    return;
  }

  fprintf(out, "#include <stdint.h>\n");
  fprintf(out, "#include <string.h>\n");
  fprintf(out, "#include <alloca.h>\n\n");

  // Hardcoded native runtime forward declarations
  // (needed because the bootstrap pipeline skips @foreign forms)
  fprintf(out, "// Native runtime forward declarations\n");
  fprintf(out, "void native_set_args(int argc, char **argv);\n");
  fprintf(out, "int32_t native_get_arg_count(void);\n");
  fprintf(out, "const char *native_get_arg(int32_t idx);\n");
  fprintf(out, "const uint8_t *native_read_file(const char *path);\n");
  fprintf(out, "uint32_t native_file_len(void);\n");
  fprintf(out,
          "void *native_lex_and_group(const uint8_t *src, uint32_t len);\n");
  fprintf(out, "void *native_init_scheme(void);\n");
  fprintf(out, "int32_t native_run_pipeline(void *ctx, void *root_group);\n");
  fprintf(out, "void native_emit_module_to_file(void *subs, const char "
               "*output_path);\n");
  fprintf(out, "void *native_get_subroutines(void);\n\n");

  // Emit forward declarations for all subroutines
  // Skip those already declared in the native runtime section
  static const char *native_funcs[] = {"native_set_args",
                                       "native_get_arg_count",
                                       "native_get_arg",
                                       "native_read_file",
                                       "native_file_len",
                                       "native_lex_and_group",
                                       "native_init_scheme",
                                       "native_run_pipeline",
                                       "native_emit_module_to_file",
                                       "native_get_subroutines",
                                       NULL};
  L1Subroutine *sub = g_subroutines_head;
  while (sub) {
    if (sub->blocks || sub->is_extern) {
      // Skip if already declared as native runtime function
      int is_native = 0;
      for (int i = 0; native_funcs[i]; i++) {
        if (strcmp(sub->name, native_funcs[i]) == 0 ||
            (sub->link_name && strcmp(sub->link_name, native_funcs[i]) == 0)) {
          is_native = 1;
          break;
        }
      }
      if (!is_native) {
        // Special-case: main with 0 params becomes int main(int, char**)
        if (strcmp(sub->name, "main") == 0 && sub->param_count == 0) {
          fprintf(out, "int main(int argc, char **argv);\n");
        } else {
          emit_c_type(sub->ret_ty, out);
          fprintf(out, " %s(", sub->link_name ? sub->link_name : sub->name);
          for (uint32_t i = 0; i < sub->param_count; i++) {
            if (sub->param_tys[i]) {
              emit_c_type(sub->param_tys[i], out);
            } else {
              fprintf(out, "void*");
            }
            fprintf(out, " arg%d%s", i,
                    (i == sub->param_count - 1) ? "" : ", ");
          }
          if (sub->param_count == 0)
            fprintf(out, "void");
          fprintf(out, ");\n");
        }
      }
    }
    sub = sub->next;
  }
  fprintf(out, "\n");

  // Emit each subroutine
  sub = g_subroutines_head;
  while (sub) {
    emit_c_subroutine(sub, out);
    sub = sub->next;
  }

  fclose(out);
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
  const char *basename = strrchr(path, '/');
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
