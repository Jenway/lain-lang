/** vm_gauche.c -- Gauche backend for vm_api.h
 *
 * This file is the only compiler file that may include Gauche headers.
 */

#include "vm_api.h"

#include <gauche.h>
#include <gauche/class.h>
#include <gauche/load.h>
#include <gauche/module.h>
#include <gauche/port.h>
#include <gauche/reader.h>
#include <gauche/string.h>
#include <gauche/symbol.h>
#include <gauche/writer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  ScmObj value;
  int is_exception;
} GaucheResult;

typedef struct {
  char *name;
  int arity;
  vm_ffi_fn fn;
} GaucheFfiEntry;

static GaucheResult g_last_result = { SCM_UNDEFINED, 0 };
static GaucheFfiEntry *g_ffi_entries = NULL;
static int g_ffi_count = 0;
static int g_ffi_cap = 0;
static ScmClass *g_cpointer_class = NULL;
static char *g_pending_load_path = NULL;

static char *vm_strdup(const char *s) {
  size_t n = strlen(s);
  char *out = (char *)malloc(n + 1);
  if (!out) {
    fprintf(stderr, "OOM\n");
    exit(1);
  }
  memcpy(out, s, n + 1);
  return out;
}

static ScmObj remember_value(ScmObj value) {
  g_last_result.value = value;
  g_last_result.is_exception = 0;
  return value;
}

static ScmObj remember_exception(ScmObj exn) {
  g_last_result.value = exn;
  g_last_result.is_exception = 1;
  return exn;
}

static ScmObj packet_result(ScmEvalPacket *packet, int rc) {
  if (rc < 0) return remember_exception(packet->exception);
  if (packet->numResults <= 0) return remember_value(SCM_UNDEFINED);
  return remember_value(packet->results[0]);
}

static GaucheFfiEntry *find_entry(const char *name) {
  for (int i = 0; i < g_ffi_count; i++) {
    if (strcmp(g_ffi_entries[i].name, name) == 0) return &g_ffi_entries[i];
  }
  return NULL;
}

static ScmObj list_ref_or_false(ScmObj args, int idx) {
  ScmObj cur = args;
  for (int i = 0; i < idx; i++) {
    if (!SCM_PAIRP(cur)) return SCM_FALSE;
    cur = SCM_CDR(cur);
  }
  return SCM_PAIRP(cur) ? SCM_CAR(cur) : SCM_FALSE;
}

static ScmObj gauche_lain_dispatch(ScmObj *argv, int argc, void *data) {
  (void)data;
  if (argc != 2 || !SCM_STRINGP(argv[0])) return SCM_FALSE;

  const char *name = Scm_GetStringConst(SCM_STRING(argv[0]));
  GaucheFfiEntry *entry = find_entry(name);
  if (!entry) return SCM_FALSE;

  ScmObj call_args = argv[1];
  vm_context *ctx = (vm_context *)Scm_VM();
  vm_value *self = NULL;
  int n = entry->arity;
  vm_value *a0 = (vm_value *)list_ref_or_false(call_args, 0);
  vm_value *a1 = (vm_value *)list_ref_or_false(call_args, 1);
  vm_value *a2 = (vm_value *)list_ref_or_false(call_args, 2);
  vm_value *a3 = (vm_value *)list_ref_or_false(call_args, 3);

  switch (n) {
  case 0:
    return (ScmObj)((vm_value *(*)(vm_context *, vm_value *, int))entry->fn)(ctx, self, n);
  case 1:
    return (ScmObj)((vm_value *(*)(vm_context *, vm_value *, int, vm_value *))entry->fn)(ctx, self, n, a0);
  case 2:
    return (ScmObj)((vm_value *(*)(vm_context *, vm_value *, int, vm_value *, vm_value *))entry->fn)(ctx, self, n, a0, a1);
  case 3:
    return (ScmObj)((vm_value *(*)(vm_context *, vm_value *, int, vm_value *, vm_value *, vm_value *))entry->fn)(ctx, self, n, a0, a1, a2);
  case 4:
    return (ScmObj)((vm_value *(*)(vm_context *, vm_value *, int, vm_value *, vm_value *, vm_value *, vm_value *))entry->fn)(ctx, self, n, a0, a1, a2, a3);
  default:
    fprintf(stderr, "gauche backend: unsupported FFI arity %d for %s\n", n, name);
    return SCM_FALSE;
  }
}

static char *scheme_quote_string(const char *s) {
  size_t extra = 0;
  for (const char *p = s; *p; p++) {
    if (*p == '\\' || *p == '"') extra++;
  }
  size_t n = strlen(s);
  char *out = (char *)malloc(n + extra + 3);
  if (!out) {
    fprintf(stderr, "OOM\n");
    exit(1);
  }
  char *w = out;
  *w++ = '"';
  for (const char *p = s; *p; p++) {
    if (*p == '\\' || *p == '"') *w++ = '\\';
    *w++ = *p;
  }
  *w++ = '"';
  *w = '\0';
  return out;
}

static void define_dispatch_wrapper(const char *name) {
  char *quoted = scheme_quote_string(name);
  size_t len = strlen(name) + strlen(quoted) + 128;
  char *code = (char *)malloc(len);
  if (!code) {
    fprintf(stderr, "OOM\n");
    exit(1);
  }
  snprintf(code, len,
           "(define (%s . args) (%%lain-dispatch %s args))",
           name, quoted);
  Scm_EvalCStringRec(code, SCM_OBJ(Scm_UserModule()));
  free(code);
  free(quoted);
}

void vm_set_module_path(const char *path) {
  if (!path || !*path) return;
  if (!Scm_InitializedP()) {
    free(g_pending_load_path);
    g_pending_load_path = vm_strdup(path);
    return;
  }
  char *quoted = scheme_quote_string(path);
  size_t len = strlen(quoted) + 64;
  char *code = (char *)malloc(len);
  if (!code) {
    fprintf(stderr, "OOM\n");
    exit(1);
  }
  snprintf(code, len, "(add-load-path %s :after #f)", quoted);
  Scm_EvalCStringRec(code, SCM_OBJ(Scm_UserModule()));
  free(code);
  free(quoted);
}

vm_context *vm_create(void) {
  if (!Scm_InitializedP()) {
    Scm_Init(GAUCHE_SIGNATURE);
    const char *argv[] = { "lainc" };
    Scm_InitCommandLine(1, argv, SCM_COMMAND_LINE_BOTH);
  }
  vm_set_module_path(".");
  if (g_pending_load_path) {
    char *path = g_pending_load_path;
    g_pending_load_path = NULL;
    vm_set_module_path(path);
    free(path);
  }
  g_cpointer_class = Scm_MakeForeignPointerClass(
      Scm_UserModule(), "<lain-cpointer>", NULL, NULL,
      SCM_FOREIGN_POINTER_KEEP_IDENTITY);
  ScmObj dispatch = Scm_MakeSubr(gauche_lain_dispatch, NULL, 2, 0,
                                 SCM_INTERN("%lain-dispatch"));
  Scm_Define(Scm_UserModule(), SCM_SYMBOL(SCM_INTERN("%lain-dispatch")), dispatch);
  return (vm_context *)Scm_VM();
}

vm_value *vm_context_env(vm_context *ctx) {
  (void)ctx;
  return (vm_value *)SCM_OBJ(Scm_UserModule());
}

vm_value *vm_eval_string(vm_context *ctx, vm_value *env, const char *code) {
  (void)ctx;
  ScmEvalPacket packet;
  int rc = Scm_EvalCString(code, (ScmObj)env, &packet);
  return (vm_value *)packet_result(&packet, rc);
}

vm_value *vm_apply(vm_context *ctx, vm_value *proc, vm_value *args) {
  (void)ctx;
  ScmEvalPacket packet;
  int rc = Scm_Apply((ScmObj)proc, (ScmObj)args, &packet);
  return (vm_value *)packet_result(&packet, rc);
}

vm_value *vm_load_file(vm_context *ctx, vm_value *env, const char *path) {
  (void)ctx;
  FILE *file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "Warning: cannot open file: %s\n", path);
    return (vm_value *)remember_value(SCM_UNDEFINED);
  }
  fseek(file, 0, SEEK_END);
  long len = ftell(file);
  fseek(file, 0, SEEK_SET);
  char *buf = (char *)malloc((size_t)len + 1);
  if (!buf) {
    fprintf(stderr, "OOM reading %s\n", path);
    exit(1);
  }
  fread(buf, 1, (size_t)len, file);
  buf[len] = '\0';
  fclose(file);

  size_t wrapped_len = (size_t)len + 10;
  char *wrapped = (char *)malloc(wrapped_len);
  if (!wrapped) {
    fprintf(stderr, "OOM wrapping %s\n", path);
    exit(1);
  }
  snprintf(wrapped, wrapped_len, "(begin %s)", buf);
  free(buf);

  ScmEvalPacket packet;
  int rc = Scm_EvalCString(wrapped, (ScmObj)env, &packet);
  free(wrapped);
  return (vm_value *)packet_result(&packet, rc);
}

int vm_load_first(vm_context *ctx, vm_value *env, const char **paths) {
  for (int i = 0; paths[i]; i++) {
    FILE *f = fopen(paths[i], "r");
    if (!f) continue;
    fclose(f);
    vm_value *result = vm_load_file(ctx, env, paths[i]);
    if (vm_is_exception(result)) {
      fprintf(stderr, "Error: failed to load Scheme file: %s\n", paths[i]);
      vm_print_exception(ctx, result);
      return 0;
    }
    return 1;
  }
  return 0;
}

void vm_import_base(vm_context *ctx, vm_value *env) {
  (void)ctx;
  (void)env;
}

void vm_check(vm_context *ctx, vm_value *v) {
  if (vm_is_exception(v)) {
    vm_print_exception(ctx, v);
    exit(1);
  }
}

vm_value *vm_intern(vm_context *ctx, const char *name) {
  (void)ctx;
  return (vm_value *)SCM_INTERN(name);
}

vm_value *vm_env_ref(vm_context *ctx, vm_value *env, vm_value *sym) {
  (void)ctx;
  ScmModule *module = env ? SCM_MODULE(env) : Scm_UserModule();
  ScmObj value = Scm_GlobalVariableRef(module, SCM_SYMBOL(sym), SCM_FIND_MODULE_QUIET);
  return (vm_value *)remember_value(value);
}

int vm_is_exception(vm_value *v) { return g_last_result.is_exception && (ScmObj)v == g_last_result.value; }
int vm_is_pair(vm_value *v) { return SCM_PAIRP((ScmObj)v); }
int vm_is_null(vm_value *v) { return SCM_NULLP((ScmObj)v); }
int vm_is_string(vm_value *v) { return SCM_STRINGP((ScmObj)v); }
int vm_is_symbol(vm_value *v) { return SCM_SYMBOLP((ScmObj)v); }
int vm_is_procedure(vm_value *v) { return SCM_PROCEDUREP((ScmObj)v); }
int vm_is_fixnum(vm_value *v) { return SCM_INTP((ScmObj)v); }
int vm_is_integer(vm_value *v) { return SCM_INTEGERP((ScmObj)v); }

vm_value *vm_car(vm_value *pair) { return (vm_value *)SCM_CAR((ScmObj)pair); }
vm_value *vm_cdr(vm_value *pair) { return (vm_value *)SCM_CDR((ScmObj)pair); }
const char *vm_string_data(vm_value *str) {
  return Scm_GetStringConst(SCM_STRING(str));
}
const char *vm_symbol_name(vm_context *ctx, vm_value *sym) {
  (void)ctx;
  return Scm_GetStringConst(SCM_SYMBOL_NAME(sym));
}
int64_t vm_fixnum_value(vm_value *fixnum) {
  if (SCM_INTP((ScmObj)fixnum)) return (int64_t)SCM_INT_VALUE((ScmObj)fixnum);
  return (int64_t)Scm_IntegerToSsize((ScmObj)fixnum);
}
int vm_string_length(vm_value *str) { return (int)SCM_STRING_LENGTH((ScmObj)str); }
int vm_list_length(vm_value *list) { return (int)Scm_Length((ScmObj)list); }
uint32_t vm_uint_value(vm_value *v) {
  if (SCM_INTP((ScmObj)v)) return (uint32_t)SCM_INT_VALUE((ScmObj)v);
  return (uint32_t)Scm_IntegerToSize((ScmObj)v);
}

vm_value *vm_cons(vm_context *ctx, vm_value *car, vm_value *cdr) {
  (void)ctx;
  return (vm_value *)Scm_Cons((ScmObj)car, (ScmObj)cdr);
}
vm_value *vm_make_string(vm_context *ctx, const char *s, int len) {
  (void)ctx;
  return (vm_value *)Scm_MakeString(s, len, len, SCM_STRING_COPYING);
}
vm_value *vm_make_integer(vm_context *ctx, int64_t n) {
  (void)ctx;
  return (vm_value *)Scm_MakeInteger64(n);
}
vm_value *vm_make_fixnum(int n) { return (vm_value *)Scm_MakeInteger(n); }
vm_value *vm_list1(vm_context *ctx, vm_value *a) {
  (void)ctx;
  return (vm_value *)SCM_LIST1((ScmObj)a);
}
vm_value *vm_list2(vm_context *ctx, vm_value *a, vm_value *b) {
  (void)ctx;
  return (vm_value *)SCM_LIST2((ScmObj)a, (ScmObj)b);
}

vm_value *vm_make_cpointer(vm_context *ctx, void *ptr) {
  (void)ctx;
  return (vm_value *)Scm_MakeForeignPointer(g_cpointer_class, ptr);
}
void *vm_cpointer_value(vm_value *v) {
  return Scm_ForeignPointerRef(SCM_FOREIGN_POINTER(v));
}

void vm_preserve(vm_context *ctx, vm_value *v) { (void)ctx; (void)v; }
void vm_release(vm_context *ctx, vm_value *v) { (void)ctx; (void)v; }

vm_value *vm_open_output_string(vm_context *ctx) {
  (void)ctx;
  return (vm_value *)Scm_MakeOutputStringPort(TRUE);
}
void vm_close_port(vm_context *ctx, vm_value *port) {
  (void)ctx;
  Scm_ClosePort(SCM_PORT(port));
}
vm_value *vm_get_output_string(vm_context *ctx, vm_value *port) {
  (void)ctx;
  return (vm_value *)Scm_GetOutputString(SCM_PORT(port), 0);
}
void vm_write(vm_context *ctx, vm_value *obj, vm_value *port) {
  (void)ctx;
  Scm_Write((ScmObj)obj, (ScmObj)port, SCM_WRITE_WRITE);
}

char *vm_to_c_string(vm_value *str) {
  return vm_strdup(Scm_GetStringConst(SCM_STRING(str)));
}
vm_value *vm_read_from_string(vm_context *ctx, const char *s, int len) {
  (void)ctx;
  (void)len;
  return (vm_value *)remember_value(Scm_ReadFromCString(s));
}

void vm_print_exception(vm_context *ctx, vm_value *exn) {
  (void)ctx;
  Scm_ReportError((ScmObj)exn, SCM_OBJ(Scm_CurrentErrorPort()));
}
vm_value *vm_user_exception(vm_context *ctx, const char *msg) {
  (void)ctx;
  return (vm_value *)remember_exception(Scm_MakeString(msg, -1, -1, SCM_STRING_COPYING));
}
vm_value *vm_error_port(vm_context *ctx) {
  (void)ctx;
  return (vm_value *)SCM_OBJ(Scm_CurrentErrorPort());
}

vm_value *vm_false(void) { return (vm_value *)SCM_FALSE; }
vm_value *vm_true(void) { return (vm_value *)SCM_TRUE; }
vm_value *vm_null(void) { return (vm_value *)SCM_NIL; }
vm_value *vm_void(void) { return (vm_value *)SCM_UNDEFINED; }

void vm_register_ffi(vm_context *ctx, vm_value *env,
                     const char *scheme_name, int arity, vm_ffi_fn fn) {
  (void)ctx;
  (void)env;
  if (g_ffi_count == g_ffi_cap) {
    g_ffi_cap = g_ffi_cap ? g_ffi_cap * 2 : 128;
    g_ffi_entries = (GaucheFfiEntry *)realloc(
        g_ffi_entries, sizeof(GaucheFfiEntry) * g_ffi_cap);
    if (!g_ffi_entries) {
      fprintf(stderr, "OOM\n");
      exit(1);
    }
  }
  g_ffi_entries[g_ffi_count].name = vm_strdup(scheme_name);
  g_ffi_entries[g_ffi_count].arity = arity;
  g_ffi_entries[g_ffi_count].fn = fn;
  g_ffi_count++;

  define_dispatch_wrapper(scheme_name);
}
