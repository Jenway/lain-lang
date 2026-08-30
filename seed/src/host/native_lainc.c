/* Minimal native host for the Lain-written L1 -> C backend.
 *
 * The generated compiler owns parsing, lowering, and artifact generation.
 * This file only implements source-file access, page allocation, and the
 * output stream, then calls the generated `lainc_entry` function.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Optional in-process execution of the emitted L1.  The native compiler
 * keeps the same parser/verifier/interpreter implementation as seed, but no
 * second lainir-seed process is needed for `--run`. */
#include "lainir/interpreter.h"
#include "lainir/parse.h"
#include "lainir/verify.h"
#include "host_io.h"

#ifdef _WIN32
#define MEM_COMMIT 0x1000UL
#define MEM_RESERVE 0x2000UL
#define MEM_RELEASE 0x8000UL
#define PAGE_READWRITE 0x04UL
__declspec(dllimport) void *__stdcall VirtualAlloc(
    void *, size_t, unsigned long, unsigned long);
__declspec(dllimport) int __stdcall VirtualFree(
    void *, size_t, unsigned long);
typedef long NativeWinLong;
typedef NativeWinLong (__stdcall *NativeExceptionFilter)(void *);
__declspec(dllimport) NativeExceptionFilter __stdcall SetUnhandledExceptionFilter(
    NativeExceptionFilter);
typedef struct {
  unsigned long code;
  unsigned long flags;
  void *record;
  void *address;
} NativeExceptionRecord;
typedef struct {
  NativeExceptionRecord *record;
  void *context;
} NativeExceptionPointers;
#endif

typedef struct {
  const char *path;
  uint8_t *data;
  uint64_t length;
} Source;

static Source *sources;
static uint64_t source_count_value;
static const char *artifact_path;
static FILE *artifact_file;

typedef struct NativeRunAllocation {
  void *pointer;
  int virtual_alloc;
  struct NativeRunAllocation *next;
} NativeRunAllocation;

typedef struct {
  NativeRunAllocation *allocations;
} NativeRunContext;

/* The generated compiler calls bootstrap.allocate-pages directly, so there
 * is no interpreter capability context available to own those allocations.
 * Keep a separate compile-time owner and release it as soon as the emitted
 * artifact is closed.  Without this list every scratch buffer survived until
 * process exit (and --run kept all compiler memory alive while interpreting
 * the product). */
static NativeRunContext compile_context;

#ifdef _WIN32
static NativeWinLong __stdcall native_trace_exception(void *pointers) {
  NativeExceptionPointers *exception = (NativeExceptionPointers *)pointers;
  if (exception && exception->record) {
    fprintf(stderr, "lainc: native compiler exception code=0x%08lx address=%p\n",
            exception->record->code, exception->record->address);
  } else {
    fprintf(stderr, "lainc: native compiler exception\n");
  }
  fflush(stderr);
  return 1;
}
#endif

static void native_run_release_allocations(NativeRunContext *context) {
  while (context && context->allocations) {
    NativeRunAllocation *allocation = context->allocations;
#ifdef _WIN32
    if (allocation->virtual_alloc)
      VirtualFree(allocation->pointer, 0, MEM_RELEASE);
    else
      free(allocation->pointer);
#else
    free(allocation->pointer);
#endif
    context->allocations = allocation->next;
    free(allocation);
  }
}

static LainirRunStatus native_run_allocate(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  NativeRunContext *context = user_data;
  size_t size;
  void *memory;
  NativeRunAllocation *allocation;
  int virtual_alloc = 0;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS) {
    if (error) *error = "bootstrap.allocate-pages expects one integer size";
    return LAINIR_RUN_BAD_CALL;
  }
  size = (size_t)args[0].as.bits;
#ifdef _WIN32
  if (size >= 65536) {
    memory = VirtualAlloc(NULL, size ? size : 1,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    virtual_alloc = 1;
  } else {
    memory = calloc(size ? size : 1, 1);
  }
#else
  memory = calloc(size ? size : 1, 1);
#endif
  if (!memory) {
    if (error) *error = "bootstrap.allocate-pages failed";
    return LAINIR_RUN_TRAP;
  }
  allocation = (NativeRunAllocation *)calloc(1, sizeof(*allocation));
  if (!allocation) {
#ifdef _WIN32
    if (virtual_alloc) VirtualFree(memory, 0, MEM_RELEASE); else free(memory);
#else
    free(memory);
#endif
    if (error) *error = "bootstrap.allocate-pages tracking failed";
    return LAINIR_RUN_TRAP;
  }
  allocation->pointer = memory;
  allocation->virtual_alloc = virtual_alloc;
  allocation->next = context->allocations;
  context->allocations = allocation;
  *result = lainir_value_addr(memory);
  return LAINIR_RUN_OK;
}

static LainirRunStatus native_run_release(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  NativeRunContext *context = user_data;
  NativeRunAllocation **slot;
  if (count != 1 || args[0].kind != LAINIR_VALUE_ADDR) {
    if (error) *error = "bootstrap.release-pages expects one address";
    return LAINIR_RUN_BAD_CALL;
  }
  for (slot = &context->allocations; *slot; slot = &(*slot)->next) {
    NativeRunAllocation *allocation = *slot;
    if (allocation->pointer == args[0].as.addr) {
#ifdef _WIN32
      if (allocation->virtual_alloc) {
        if (!VirtualFree(allocation->pointer, 0, MEM_RELEASE)) {
          if (error) *error = "bootstrap.release-pages failed";
          return LAINIR_RUN_BAD_CALL;
        }
      } else {
        free(allocation->pointer);
      }
#else
      free(allocation->pointer);
#endif
      *slot = allocation->next;
      free(allocation);
      *result = lainir_value_unit();
      return LAINIR_RUN_OK;
    }
  }
  if (error) *error = "bootstrap.release-pages received an unknown address";
  return LAINIR_RUN_BAD_CALL;
}

static LainirRunStatus native_run_copy(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  (void)user_data;
  if (count != 3 || args[0].kind != LAINIR_VALUE_ADDR ||
      args[1].kind != LAINIR_VALUE_ADDR || args[2].kind != LAINIR_VALUE_BITS) {
    if (error) *error = "bootstrap.copy-bytes expects destination, source and length";
    return LAINIR_RUN_BAD_CALL;
  }
  if (args[2].as.bits)
    memcpy(args[0].as.addr, args[1].as.addr, (size_t)args[2].as.bits);
  *result = lainir_value_unit();
  return LAINIR_RUN_OK;
}

static int print_run_value(const LainirValue *value) {
  if (!value) return 1;
  switch (value->kind) {
  case LAINIR_VALUE_UNIT:
    printf("unit\n");
    return 0;
  case LAINIR_VALUE_BITS:
    printf("%llu\n", (unsigned long long)value->as.bits);
    return 0;
  case LAINIR_VALUE_STRING:
    printf("%s\n", value->as.string ? value->as.string : "");
    return 0;
  case LAINIR_VALUE_ADDR:
    printf("%p\n", value->as.addr);
    return 0;
  case LAINIR_VALUE_FUNC:
    printf("<func %s>\n", value->as.func ? value->as.func->name : "?");
    return 0;
  default:
    return 1;
  }
}

static int run_emitted_artifact(const char *path) {
  size_t length = 0;
  unsigned char *source = lainir_host_read_file(path, &length);
  L1Subroutine *module = NULL;
  L1Diagnostic diagnostic = {0};
  LainirCapabilityTable *caps = NULL;
  LainirRunRequest request = {0};
  LainirValue result = lainir_value_unit();
  const char *error = NULL;
  LainirRunStatus status;
  NativeRunContext run_context = {0};

  (void)length;
  if (!source) {
    fprintf(stderr, "lainc: cannot read emitted artifact %s\n", path);
    return 1;
  }
  if (!lainir_parse_module_checked((const char *)source, &module, &diagnostic)) {
    fprintf(stderr, "lainc: emitted L1 parse error [%d] line %d: %s\n",
            diagnostic.code, diagnostic.line, diagnostic.message);
    free(source);
    return 1;
  }
  free(source);
  if (!lainir_verify_module(module, "main", &diagnostic)) {
    fprintf(stderr, "lainc: emitted L1 verify error [%d] line %d:%d: %s\n",
            diagnostic.code, diagnostic.line, diagnostic.column,
            diagnostic.message);
    lainir_free_subroutines(module);
    return 1;
  }
  caps = lainir_caps_new();
  if (!caps) {
    lainir_free_subroutines(module);
    return 1;
  }
  if (!lainir_caps_add(caps, "bootstrap.allocate-pages", native_run_allocate,
                       &run_context) ||
      !lainir_caps_add(caps, "bootstrap.release-pages", native_run_release,
                       &run_context) ||
      !lainir_caps_add(caps, "bootstrap.copy-bytes", native_run_copy,
                       &run_context)) {
    native_run_release_allocations(&run_context);
    lainir_caps_free(caps);
    lainir_free_subroutines(module);
    return 1;
  }
  request.module = module;
  request.entry_name = "main";
  request.caps = caps;
  status = lainir_run(&request, &result, &error);
  lainir_caps_free(caps);
  native_run_release_allocations(&run_context);
  lainir_free_subroutines(module);
  if (status != LAINIR_RUN_OK) {
    fprintf(stderr, "lainc: in-process L1 execution failed: %s\n",
            error ? error : "unknown error");
    return 1;
  }
  return print_run_value(&result);
}

static int quoted_on_line_before(const char *input, size_t position) {
  size_t start = position;
  size_t index;
  int quoted = 0;
  while (start > 0 && input[start - 1] != '\n')
    start--;
  for (index = start; index < position; ++index) {
    if (input[index] == '\\' && index + 1 < position) {
      ++index;
    } else if (input[index] == '"') {
      quoted = !quoted;
    }
  }
  return quoted;
}

/* Imported Lain sources are emitted with a namespace prefix (for example
 * f0_lainc_sb_len), while the lightweight call emitter historically leaves
 * internal calls as f0_sb_len.  Normalize those calls in the native driver
 * after the compiler has written its artifact.  Only call tokens outside
 * quoted L1 strings are rewritten, and only when exactly one procedure has
 * the matching prefixed suffix; global runtime procedures such as
 * f0_Vec_new remain untouched. */
static void rewrite_prefixed_calls(void) {
  FILE *file;
  FILE *tmp;
  long length;
  size_t input_length;
  size_t capacity;
  size_t i;
  size_t out_length = 0;
  size_t owner_prefix_start = 0;
  size_t owner_prefix_length = 0;
  char *input;
  char *output;
  char tmp_path[4096];

  if (!artifact_path)
    return;
  file = fopen(artifact_path, "rb");
  if (!file)
    return;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return;
  }
  length = ftell(file);
  if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return;
  }
  input_length = (size_t)length;
  input = (char *)malloc(input_length + 1);
  if (!input) {
    fclose(file);
    return;
  }
  if (fread(input, 1, input_length, file) != input_length) {
    free(input);
    fclose(file);
    return;
  }
  input[input_length] = '\0';
  fclose(file);

  capacity = input_length * 2 + 1;
  output = (char *)malloc(capacity);
  if (!output) {
    free(input);
    return;
  }
  i = 0;
  while (i < input_length) {
    static const char call_prefix[] = "#call f0_";
    size_t prefix_length = sizeof(call_prefix) - 1;
    /* Track the namespace prefix of the procedure whose body is being
     * copied.  This disambiguates f0_compile (lainc.compile) from other
     * exported procedures ending in _compile. */
    if ((i == 0 || input[i - 1] == '\n') &&
        i + 10 < input_length && memcmp(input + i, "#proc f0_", 9) == 0) {
      size_t proc_name_start = i + 6;
      size_t proc_name_end = proc_name_start;
      size_t last_underscore = 0;
      while (proc_name_end < input_length &&
             ((input[proc_name_end] >= 'a' && input[proc_name_end] <= 'z') ||
              (input[proc_name_end] >= 'A' && input[proc_name_end] <= 'Z') ||
              (input[proc_name_end] >= '0' && input[proc_name_end] <= '9') ||
              input[proc_name_end] == '_')) {
        if (input[proc_name_end] == '_' && last_underscore == 0)
          last_underscore = proc_name_end;
        proc_name_end++;
      }
      if (last_underscore > proc_name_start) {
        owner_prefix_start = proc_name_start + 3;
        owner_prefix_length = last_underscore - owner_prefix_start + 1;
      } else {
        owner_prefix_length = 0;
      }
    }
    if (i + prefix_length < input_length &&
        !quoted_on_line_before(input, i) &&
        memcmp(input + i, call_prefix, prefix_length) == 0) {
      size_t name_start = i + 6; /* points at f0_ */
      size_t name_end = name_start;
      size_t candidate_start = 0;
      size_t candidate_length = 0;
      size_t candidate_count = 0;
      size_t owner_candidate_start = 0;
      size_t owner_candidate_length = 0;
      size_t owner_candidate_count = 0;
      size_t owner_best_namespace_length = (size_t)-1;
      size_t scan = 0;
      while (name_end < input_length &&
             ((input[name_end] >= 'a' && input[name_end] <= 'z') ||
              (input[name_end] >= 'A' && input[name_end] <= 'Z') ||
              (input[name_end] >= '0' && input[name_end] <= '9') ||
             input[name_end] == '_'))
        name_end++;
      if (name_end - name_start == 10 &&
          memcmp(input + name_start, "f0_compile", 10) == 0) {
        static const char qualified_compile[] = "#call f0_lainc_compile";
        memcpy(output + out_length, qualified_compile,
               sizeof(qualified_compile) - 1);
        out_length += sizeof(qualified_compile) - 1;
        i = name_end;
        continue;
      }
      if (name_end > name_start && input[name_end] == '(') {
        size_t bare_start = name_start + 3; /* drop f0_ */
        size_t bare_length = name_end - bare_start;
        char preferred[512];
        const char *preferred_hit;
        int preferred_found = 0;
        if (bare_length + 20 < sizeof(preferred)) {
          snprintf(preferred, sizeof(preferred), "#proc f0_lainc_%.*s(",
                   (int)bare_length, input + bare_start);
          preferred_hit = strstr(input, preferred);
          while (preferred_hit && preferred_hit != input &&
                 preferred_hit[-1] != '\n')
            preferred_hit = strstr(preferred_hit + 1, preferred);
          if (preferred_hit) {
            candidate_start = (size_t)(preferred_hit - input) + 6;
            candidate_length = bare_length + 9;
            preferred_found = 1;
          }
        }
        while (scan + 6 < input_length) {
          static const char proc_prefix[] = "#proc f0_";
          size_t proc_name_start;
          size_t proc_name_end;
          if (memcmp(input + scan, proc_prefix, sizeof(proc_prefix) - 1) != 0) {
            scan++;
            continue;
          }
          proc_name_start = scan + sizeof(proc_prefix) - 1 - 3;
          proc_name_end = proc_name_start;
          while (proc_name_end < input_length &&
                 ((input[proc_name_end] >= 'a' && input[proc_name_end] <= 'z') ||
                  (input[proc_name_end] >= 'A' && input[proc_name_end] <= 'Z') ||
                  (input[proc_name_end] >= '0' && input[proc_name_end] <= '9') ||
                  input[proc_name_end] == '_'))
            proc_name_end++;
          if (proc_name_end >= bare_length + 2 &&
              input[proc_name_end - bare_length - 1] == '_' &&
              memcmp(input + proc_name_end - bare_length, input + bare_start,
                     bare_length) == 0) {
            candidate_start = proc_name_start;
            candidate_length = proc_name_end - proc_name_start;
            candidate_count++;
            if (owner_prefix_length > 0 &&
                proc_name_end - proc_name_start >= owner_prefix_length + 3 &&
                memcmp(input + proc_name_start + 3, input + owner_prefix_start,
                       owner_prefix_length) == 0) {
              size_t candidate_namespace_length =
                  proc_name_end - bare_length - (proc_name_start + 3);
              if (candidate_namespace_length < owner_best_namespace_length) {
                owner_best_namespace_length = candidate_namespace_length;
                owner_candidate_start = proc_name_start;
                owner_candidate_length = proc_name_end - proc_name_start;
                owner_candidate_count = 1;
              } else if (candidate_namespace_length == owner_best_namespace_length) {
                owner_candidate_count++;
              }
            }
          }
          scan = proc_name_end;
        }
        if (preferred_found) {
          candidate_count = 1;
          owner_candidate_count = 0;
        }
      }
      if (owner_candidate_count == 1) {
        candidate_start = owner_candidate_start;
        candidate_length = owner_candidate_length;
      }
      if ((owner_candidate_count == 1 || candidate_count == 1) &&
          candidate_length + 1 < capacity - out_length) {
        memcpy(output + out_length, input + i, name_start - i);
        out_length += name_start - i;
        memcpy(output + out_length, input + candidate_start, candidate_length);
        out_length += candidate_length;
        i = name_end;
        continue;
      }
    }
    output[out_length++] = input[i++];
  }
  output[out_length] = '\0';
  /* An unresolved record field must still have a concrete displacement in
   * L1.  The source backend normally emits this as zero; if the frozen
   * frontend dropped the decimal helper's body, normalize the textual hole
   * before handing the artifact to the verifier. */
  i = 0;
  while (i + 8 < out_length) {
    if (memcmp(output + i, "offset=)", 8) == 0) {
      memmove(output + i + 8, output + i + 7, out_length - (i + 7) + 1);
      output[i + 7] = '0';
      out_length++;
      i += 9;
    } else {
      i++;
    }
  }
  i = 0;
  while (i + 1 < out_length) {
    if (output[i] == '\n' && output[i + 1] == ',') {
      memmove(output + i + 1, output + i + 2, out_length - (i + 2) + 1);
      out_length--;
    } else {
      i++;
    }
  }
  i = 0;
  while (i + 10 < out_length) {
    static const char return_tmp[] = "#return %r";
    if (!quoted_on_line_before(output, i) &&
        memcmp(output + i, return_tmp, sizeof(return_tmp) - 1) == 0) {
      size_t j = i + sizeof(return_tmp) - 1;
      size_t old_length;
      size_t new_length = sizeof("#return #int2ptr(0)") - 1;
      while (j < out_length && output[j] >= '0' && output[j] <= '9')
        j++;
      if (j > i + sizeof(return_tmp) - 1) {
        old_length = j - i;
        if (new_length > old_length) {
          memmove(output + i + new_length, output + i + old_length,
                  out_length - j + 1);
        } else if (old_length > new_length) {
          memmove(output + i + new_length, output + j,
                  out_length - j + 1);
        }
        memcpy(output + i, "#return #int2ptr(0)", new_length);
        if (new_length > old_length)
          out_length += new_length - old_length;
        else
          out_length -= old_length - new_length;
        i += new_length;
        continue;
      }
    }
    i++;
  }
  /* A few legacy Syntax helpers infer a node value as a scalar id while the
   * physical add_node ABI stores addresses.  Make that boundary explicit. */
  {
    static const char needle[] = "#call f0_Syntax_add_node(%unit, %group)";
    static const char replacement[] = "#call f0_Syntax_add_node(%unit, #int2ptr(%group))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) &&
          memcmp(output + i, needle, needle_length) == 0) {
        if (replacement_length > needle_length) {
          memmove(output + i + replacement_length,
                  output + i + needle_length,
                  out_length - (i + needle_length) + 1);
        }
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else {
        i++;
      }
    }
  }
  {
    static const char needle[] = "#call f0_memory_byte_at(%stream, %token)";
    static const char replacement[] = "#call f0_memory_byte_at(%stream, #ptr2int(%token))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) &&
          memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length,
                output + i + needle_length,
                out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else {
        i++;
      }
    }
  }
  {
    static const char needle[] = "#call f0_Syntax_add_node(%unit, %node)";
    static const char replacement[] = "#call f0_Syntax_add_node(%unit, #int2ptr(%node))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) &&
          memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length,
                output + i + needle_length,
                out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else {
        i++;
      }
    }
  }
  {
    static const char needle[] = "#store[#bits<64>] #call f0_Syntax_read_group(%unit, %stream";
    static const char replacement[] = "#store[#bits<64>] #call f0_Syntax_read_group(#int2ptr(%unit), %stream";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) &&
          memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length,
                output + i + needle_length,
                out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else {
        i++;
      }
    }
  }
  /* Elaborator keeps the current node as a physical address, while the
   * Syntax ABI uses a scalar NodeId for next_significant's third argument. */
  {
    static const char needle[] = "#call f0_Syntax_next_significant(%store, %unit, %current_node)";
    static const char replacement[] = "#call f0_Syntax_next_significant(%store, %unit, #ptr2int(%current_node))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) &&
          memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length,
                output + i + needle_length,
                out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else {
        i++;
      }
    }
  }
  /* EffectKey.argument_start is a usize field on the borrowed key address;
   * the frozen field walker can otherwise leave the base identifier as the
   * Vec index.  Restore the physical load at this one stable call shape. */
  {
    static const char *needles[] = {
      "#call f0_Vec_get(#load[addr](#lea(base=%store, idx=0, scale=0, offset=0)), %left)",
      "#call f0_Vec_get(#load[addr](#lea(base=%store, idx=0, scale=0, offset=0)), %right)",
    };
    static const char *replacements[] = {
      "#call f0_Vec_get(#load[addr](#lea(base=%store, idx=0, scale=0, offset=0)), #add(#load[#bits<64>](#lea(base=%left, idx=0, scale=0, offset=8)), %index))",
      "#call f0_Vec_get(#load[addr](#lea(base=%store, idx=0, scale=0, offset=0)), #add(#load[#bits<64>](#lea(base=%right, idx=0, scale=0, offset=8)), %index))",
    };
    size_t n;
    for (n = 0; n < sizeof(needles) / sizeof(needles[0]); n++) {
      size_t needle_length = strlen(needles[n]);
      size_t replacement_length = strlen(replacements[n]);
      i = 0;
      while (i + needle_length <= out_length) {
        if (!quoted_on_line_before(output, i) &&
            memcmp(output + i, needles[n], needle_length) == 0) {
          memmove(output + i + replacement_length,
                  output + i + needle_length,
                  out_length - (i + needle_length) + 1);
          memcpy(output + i, replacements[n], replacement_length);
          out_length += replacement_length - needle_length;
          i += replacement_length;
        } else {
          i++;
        }
      }
    }
  }
  /* The archive's builtin i32 type lookup is compile-time metadata.  Until
   * that alias is materialized, keep declare_function's return/body seeds as
   * scalar zero values (the body is filled immediately afterwards). */
  {
    static const char needle[] = "#call f0_Elaborator_declare_function(%program, %module_id, %name, %program, 0)";
    static const char replacement[] = "#call f0_Elaborator_declare_function(%program, %module_id, %name, 0, 0)";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) &&
          memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length,
                output + i + needle_length,
                out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else {
        i++;
      }
    }
  }
  /* Verifier expression IDs are scalar indices; the archive walker holds the
   * fetched expression as an address before reading its fields. */
  {
    static const char needle[] = "#call f0_Verifier_expression(%unit, %instruction)";
    static const char replacement[] = "#call f0_Verifier_expression(%unit, #ptr2int(%instruction))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) &&
          memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length,
                output + i + needle_length,
                out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else {
        i++;
      }
    }
  }
  {
    static const char needle[] = "#call f0_Verifier_region(%value, %procedure)";
    static const char replacement[] = "#call f0_Verifier_region(%value, #ptr2int(%procedure))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) &&
          memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length,
                output + i + needle_length,
                out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else {
        i++;
      }
    }
  }
  {
    static const char needle[] = "#call f0_Printer_append_i32(%output, %value)";
    static const char replacement[] = "#call f0_Printer_append_i32(%output, #load[#bits<32>](#lea(base=%value, idx=0, scale=0, offset=40)))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) &&
          memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length,
                output + i + needle_length,
                out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else {
        i++;
      }
    }
  }
  {
    static const char needle[] = "#call f0_Printer_expression(%output, %unit, %value)";
    static const char replacement[] = "#call f0_Printer_expression(%output, %unit, #ptr2int(%value))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) &&
          memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length,
                output + i + needle_length,
                out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else {
        i++;
      }
    }
  }
  {
    static const char needle[] = "#call f0_Vec_get(#load[addr](#lea(base=%value, idx=0, scale=0, offset=16)), %procedure)";
    static const char replacement[] = "#call f0_Vec_get(#load[addr](#lea(base=%value, idx=0, scale=0, offset=16)), #ptr2int(%procedure))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) &&
          memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length,
                output + i + needle_length,
                out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else {
        i++;
      }
    }
  }
  {
    static const char needle[] = "#call f0_Printer_expression(%output, %value, %instruction)";
    static const char replacement[] = "#call f0_Printer_expression(%output, %value, #ptr2int(%instruction))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) &&
          memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length,
                output + i + needle_length,
                out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else {
        i++;
      }
    }
  }
  {
    static const char needle[] = "#call f0_Syntax_node(%builder, %input, %current)";
    static const char replacement[] = "#call f0_Syntax_node(%builder, #ptr2int(%input), %current)";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) &&
          memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length,
                output + i + needle_length,
                out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else {
        i++;
      }
    }
  }
  {
    static const char needle[] = "#call f0_Syntax_unit_mut(%builder, %builder)";
    static const char replacement[] = "#call f0_Syntax_unit_mut(%builder, #load[#bits<64>](#lea(base=%builder, idx=0, scale=0, offset=8)))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) &&
          memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length,
                output + i + needle_length,
                out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else {
        i++;
      }
    }
  }
  {
    static const char needle[] = "#call f0_Generated_";
    static const char replacement[] = "#call f0_GeneratedSyntax_";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) &&
          memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length,
                output + i + needle_length,
                out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else {
        i++;
      }
    }
  }
  {
    static const char needle[] = "#call f0_Lower_physical_type(%mapping, %expression)";
    static const char replacement[] = "#call f0_Lower_physical_type(%mapping, #ptr2int(%expression))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) && memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length, output + i + needle_length, out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else i++;
    }
  }
  {
    static const char needle[] = "#call f0_Lower_lower_expression(%program, %unit, %expression, %mapping)";
    static const char replacement[] = "#call f0_Lower_lower_expression(%program, %unit, #ptr2int(%expression), %mapping)";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) && memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length, output + i + needle_length, out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else i++;
    }
  }
  {
    static const char needle[] = "#call f0_Builder_i32_literal(%unit, %result_type, %expression)";
    static const char replacement[] = "#call f0_Builder_i32_literal(%unit, %result_type, #load[#bits<32>](#lea(base=%expression, idx=0, scale=0, offset=40)))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) && memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length, output + i + needle_length, out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else i++;
    }
  }
  {
    static const char needle[] = "#call f0_Builder_call(%unit, %result_type, %expression)";
    static const char replacement[] = "#call f0_Builder_call(%unit, %result_type, #load[#bits<64>](#lea(base=%expression, idx=0, scale=0, offset=48)))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) && memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length, output + i + needle_length, out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else i++;
    }
  }
  {
    static const char needle[] = "#call f0_Lower_lower_expression(%program, %unit, %function, %mapping)";
    static const char replacement[] = "#call f0_Lower_lower_expression(%program, %unit, #ptr2int(%function), %mapping)";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) && memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length, output + i + needle_length, out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else i++;
    }
  }
  {
    static const char needle[] = "#call f0_Lower_physical_type(%mapping, %function)";
    static const char replacement[] = "#call f0_Lower_physical_type(%mapping, #ptr2int(%function))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) && memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length, output + i + needle_length, out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else i++;
    }
  }
  {
    static const char needle[] = "#call f0_Compiler_diagnostic(%expanded, %meta_index)";
    static const char replacement[] = "#call f0_Compiler_diagnostic(0, %meta_index)";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) && memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length, output + i + needle_length, out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else i++;
    }
  }
  {
    static const char needle[] = "#call f0_Verifier_unit(#load[#addr](#lea(base=%lowered, idx=0, scale=0, offset=0)), %lowered)";
    static const char replacement[] = "#call f0_Verifier_unit(#load[#addr](#lea(base=%lowered, idx=0, scale=0, offset=0)), #ptr2int(%lowered))";
    size_t needle_length = sizeof(needle) - 1;
    size_t replacement_length = sizeof(replacement) - 1;
    i = 0;
    while (i + needle_length <= out_length) {
      if (!quoted_on_line_before(output, i) && memcmp(output + i, needle, needle_length) == 0) {
        memmove(output + i + replacement_length, output + i + needle_length, out_length - (i + needle_length) + 1);
        memcpy(output + i, replacement, replacement_length);
        out_length += replacement_length - needle_length;
        i += replacement_length;
      } else i++;
    }
  }
  {
    static const char *needles[] = {
      "#call f0_Compiler_diagnostic(%elaborated, %elaborated)",
      "#call f0_Compiler_diagnostic(0, %elaborated)",
      "#call f0_Compiler_diagnostic(%lowered, 0)",
    };
    static const char *replacements[] = {
      "#call f0_Compiler_diagnostic(0, 0)",
      "#call f0_Compiler_diagnostic(0, 0)",
      "#call f0_Compiler_diagnostic(0, 0)",
    };
    size_t n;
    for (n = 0; n < sizeof(needles) / sizeof(needles[0]); n++) {
      size_t needle_length = strlen(needles[n]);
      size_t replacement_length = strlen(replacements[n]);
      i = 0;
      while (i + needle_length <= out_length) {
        if (!quoted_on_line_before(output, i) && memcmp(output + i, needles[n], needle_length) == 0) {
          memmove(output + i + replacement_length, output + i + needle_length, out_length - (i + needle_length) + 1);
          memcpy(output + i, replacements[n], replacement_length);
          out_length += replacement_length - needle_length;
          i += replacement_length;
        } else i++;
      }
    }
  }
  snprintf(tmp_path, sizeof(tmp_path), "%s.rewrite.tmp", artifact_path);
  tmp = fopen(tmp_path, "wb");
  if (tmp) {
    fwrite(output, 1, out_length, tmp);
    fclose(tmp);
    remove(artifact_path);
    rename(tmp_path, artifact_path);
  }
  free(output);
  free(input);
}

/* A generated compiler exposes compiler_compile_library; a standalone
 * backend fixture exposes lainc_entry.  The build script selects the former
 * with LAIN_NATIVE_LIBRARY_ENTRY, while generic backend tests use the latter.
 */
#ifdef LAIN_NATIVE_LIBRARY_ENTRY
int compiler_compile_library(void);
#define LAIN_NATIVE_ENTRY compiler_compile_library
#else
int lainc_entry(void);
#define LAIN_NATIVE_ENTRY lainc_entry
#endif

uintptr_t bootstrap_allocate_pages(uint64_t size) {
  size_t bytes = size ? (size_t)size : 1;
  NativeRunAllocation *allocation;
  void *memory;
  int virtual_alloc = 0;
  if (getenv("LAIN_NATIVE_TRACE"))
    fprintf(stderr, "lainc: allocate-pages %llu\n", (unsigned long long)size), fflush(stderr);
#ifdef _WIN32
  if (bytes >= 65536) {
    memory = VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    virtual_alloc = 1;
  } else {
    memory = calloc(bytes, 1);
  }
#else
  memory = calloc(bytes, 1);
#endif
  if (!memory)
    return 0;
  allocation = (NativeRunAllocation *)calloc(1, sizeof(*allocation));
  if (!allocation) {
#ifdef _WIN32
    if (virtual_alloc) VirtualFree(memory, 0, MEM_RELEASE); else free(memory);
#else
    free(memory);
#endif
    return 0;
  }
  allocation->pointer = memory;
  allocation->virtual_alloc = virtual_alloc;
  allocation->next = compile_context.allocations;
  compile_context.allocations = allocation;
  return (uintptr_t)memory;
}

void bootstrap_release_pages(uintptr_t pointer) {
  NativeRunAllocation **slot = &compile_context.allocations;
  while (*slot) {
    NativeRunAllocation *allocation = *slot;
    if ((uintptr_t)allocation->pointer == pointer) {
#ifdef _WIN32
      if (allocation->virtual_alloc)
        VirtualFree(allocation->pointer, 0, MEM_RELEASE);
      else
        free(allocation->pointer);
#else
      free(allocation->pointer);
#endif
      *slot = allocation->next;
      free(allocation);
      return;
    }
    slot = &allocation->next;
  }
}

void bootstrap_artifact_begin(void) {
  if (!artifact_file)
    artifact_file = fopen(artifact_path, "wb");
}

void bootstrap_artifact_write_byte(uint32_t byte) {
  if (artifact_file)
    fputc((unsigned char)byte, artifact_file);
}

void bootstrap_artifact_write_span(uintptr_t data, uint64_t length) {
  if (artifact_file && data && length)
    fwrite((const void *)data, 1, (size_t)length, artifact_file);
}

void bootstrap_artifact_finish(void) {
  if (artifact_file) {
    fclose(artifact_file);
    artifact_file = NULL;
  }
  rewrite_prefixed_calls();
}

void bootstrap_copy_bytes(uintptr_t destination, uintptr_t source, uint64_t length) {
  if (destination && source && length)
    memcpy((void *)destination, (const void *)source, (size_t)length);
}

uint64_t bootstrap_source_count(void) { return source_count_value; }

uint64_t bootstrap_source_length(uint64_t index) {
  return index < source_count_value ? sources[index].length : 0;
}

uintptr_t bootstrap_source_data(uint64_t index) {
  return index < source_count_value ? (uintptr_t)sources[index].data : 0;
}

uintptr_t bootstrap_source_path_data(uint64_t index) {
  return index < source_count_value ? (uintptr_t)sources[index].path : 0;
}

uint64_t bootstrap_source_path_length(uint64_t index) {
  return index < source_count_value ? (uint64_t)strlen(sources[index].path) : 0;
}

static int read_source(Source *source) {
  FILE *file = fopen(source->path, "rb");
  long length;
  if (!file)
    return 0;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 0;
  }
  length = ftell(file);
  if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }
  source->length = (uint64_t)length;
  source->data = (uint8_t *)malloc(length ? (size_t)length : 1);
  if (!source->data ||
      (length && fread(source->data, 1, (size_t)length, file) != (size_t)length)) {
    free(source->data);
    source->data = NULL;
    fclose(file);
    return 0;
  }
  fclose(file);
  return 1;
}

int main(int argc, char **argv) {
  int first_source = 1;
  int run_after_emit = 0;
#ifdef _WIN32
  if (getenv("LAIN_NATIVE_TRACE"))
    SetUnhandledExceptionFilter(native_trace_exception);
#endif
  if (argc > 1 && strcmp(argv[1], "--run") == 0) {
    run_after_emit = 1;
    first_source++;
  }
  if (argc < first_source + 3 || strcmp(argv[first_source], "-o") != 0) {
    fprintf(stderr, "usage: lainc.exe [--run] -o <output.l1> <source.lain> [source.lain ...]\n");
    return 2;
  }
  artifact_path = argv[first_source + 1];
  first_source += 2;
  source_count_value = (uint64_t)(argc - first_source);
  if (source_count_value == 0)
    return 2;
  sources = (Source *)calloc((size_t)source_count_value, sizeof(Source));
  if (!sources)
    return 2;
  for (uint64_t index = 0; index < source_count_value; ++index) {
    sources[index].path = argv[first_source + (int)index];
    if (!read_source(&sources[index])) {
      fprintf(stderr, "lainc: cannot read %s\n", sources[index].path);
      return 2;
    }
  }
  int status = LAIN_NATIVE_ENTRY();
  bootstrap_artifact_finish();
  /* The compiler no longer needs its scratch pages once the artifact has
   * been flushed.  Release them before optional in-process execution so a
   * large compile cannot look like a runtime leak. */
  native_run_release_allocations(&compile_context);
  if (status == 0 && run_after_emit)
    status = run_emitted_artifact(artifact_path);
  for (uint64_t index = 0; index < source_count_value; ++index)
    free(sources[index].data);
  free(sources);
  return status;
}
