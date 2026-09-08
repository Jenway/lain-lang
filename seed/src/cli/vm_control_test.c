#include "lainir/interpreter.h"
#include "lainir/eval_source.h"

#include <stdio.h>

static int fail(const char *message) {
  fprintf(stderr, "vm control test: %s\n", message);
  return 1;
}

typedef struct {
  LainirVmControl *control;
  uint64_t expected_procedure;
  int observed;
} ObserveContext;

static LainirRunStatus observe_nested_frame(
    const LainirValue *args, uint32_t arg_count, LainirValue *result_out,
    const char **error_out, void *user_data) {
  (void)args;
  (void)error_out;
  ObserveContext *context = user_data;
  const LainirVmFrame *frame =
      context ? lainir_vm_control_current_frame(context->control) : NULL;
  if (context && frame &&
      (!context->expected_procedure ||
       frame->procedure == context->expected_procedure) &&
      frame->return_procedure != 0)
    context->observed = 1;
  if (arg_count != 0 || !result_out) return LAINIR_RUN_BAD_CALL;
  *result_out = lainir_value_bits(42, 32);
  return LAINIR_RUN_OK;
}

int main(void) {
  LainirVmControl *control = lainir_vm_control_new(8);
  if (!control) return fail("allocation failed");
  if (!lainir_vm_control_start(control, 7)) return fail("start failed");
  if (lainir_vm_control_start(control, 7)) return fail("double start accepted");
  if (!lainir_vm_control_push_frame(control, 7, 1, 0, 1))
    return fail("root frame push failed");
  if (!lainir_vm_control_begin_slice(control, 7, 1))
    return fail("slice begin failed");
  if (!lainir_vm_control_consume_step(control, 7))
    return fail("first step rejected");
  if (lainir_vm_control_consume_step(control, 7) ||
      !lainir_vm_control_slice_exhausted(control) ||
      lainir_vm_control_slice_result(control) != LAINIR_VM_RUNNABLE)
    return fail("fuel exhaustion was not reported");
  if (!lainir_vm_control_set_position(control, 7, 0, 1))
    return fail("position update failed");
  if (!lainir_vm_control_push_frame(control, 7, 2, 1, 2))
    return fail("callee frame push failed");
  const LainirVmFrame *frame = lainir_vm_control_current_frame(control);
  if (!frame || frame->procedure != 2 || frame->return_procedure != 1)
    return fail("callee frame did not preserve caller");
  if (!lainir_vm_control_pop_frame(control, 7)) return fail("frame pop failed");
  frame = lainir_vm_control_current_frame(control);
  if (!frame || frame->procedure != 1 || frame->position != 1)
    return fail("caller frame was not restored");
  if (!lainir_vm_control_suspend(control, 7, 3)) return fail("suspend failed");
  if (lainir_vm_control_resume(control, 8)) return fail("foreign resume accepted");
  if (!lainir_vm_control_resume(control, 7)) return fail("resume failed");
  if (lainir_vm_control_finish(control, 7) != LAINIR_VM_DONE ||
      lainir_vm_control_state(control) != LAINIR_VM_DEAD)
    return fail("finish did not reach DEAD");
  lainir_vm_control_free(control);

  /* The interpreter consumes the provider-owned fuel gate at each instruction
   * boundary, exposes the active nested frame to a host callback, and resumes
   * the root frame on the next run. */
  const char *source =
      "#extern #proc test.observe() -> #bits<32>;\n"
      "#proc helper() -> #bits<32> {\n"
      "  #return #call test.observe()\n"
      "}\n"
      "#proc main() -> #bits<32> {\n"
      "  #let %value: #bits<32> = #call helper()\n"
      "  #return %value\n"
      "}\n";
  L1Diagnostic diagnostic = {0};
  LainirModuleHandle *handle = NULL;
  if (lainir_module_parse_handle(source, &handle, &diagnostic) != LAINIR_RUN_OK) {
    return fail("interpreter fixture parse failed");
  }
  if (lainir_module_handle_verify_entry(handle, "main", &diagnostic) !=
      LAINIR_RUN_OK) {
    lainir_module_handle_destroy(&handle);
    return fail("interpreter fixture verification failed");
  }
  control = lainir_vm_control_new(32);
  if (!control || !lainir_vm_control_start(control, 9) ||
      !lainir_vm_control_begin_slice(control, 9, 1))
    return fail("interpreter control setup failed");
  ObserveContext observe = {
      .control = control,
      .expected_procedure = 0,
      .observed = 0,
  };
  LainirCapabilityTable *caps = lainir_caps_new();
  if (!caps || !lainir_caps_add(caps, "test.observe", observe_nested_frame,
                                &observe)) {
    lainir_caps_free(caps);
    lainir_module_handle_destroy(&handle);
    lainir_vm_control_free(control);
    return fail("interpreter observation capability setup failed");
  }
  LainirRunRequest request = {0};
  request.module = (L1Subroutine *)lainir_module_handle_first(handle);
  request.entry_name = "main";
  request.caps = caps;
  request.vm_control = control;
  request.vm_owner = 9;
  LainirValue result = {0};
  const char *error = NULL;
  LainirRunStatus run_status = lainir_run(&request, &result, &error);
  if (run_status != LAINIR_RUN_SLICE ||
      !lainir_vm_control_slice_exhausted(control) || error) {
    lainir_module_handle_destroy(&handle);
    lainir_caps_free(caps);
    lainir_vm_control_free(control);
    return fail("interpreter did not report a VM slice boundary");
  }
  const LainirVmFrame *saved_frame = lainir_vm_control_current_frame(control);
  if (!saved_frame || !saved_frame->procedure || !saved_frame->position) {
    lainir_module_handle_destroy(&handle);
    lainir_caps_free(caps);
    lainir_vm_control_free(control);
    return fail("interpreter did not save continuation position");
  }
  if (!lainir_vm_control_begin_slice(control, 9, 1)) {
    lainir_module_handle_destroy(&handle);
    lainir_caps_free(caps);
    lainir_vm_control_free(control);
    return fail("interpreter resume slice setup failed");
  }
  error = NULL;
  run_status = lainir_run(&request, &result, &error);
  if (run_status != LAINIR_RUN_OK || error || result.kind != LAINIR_VALUE_BITS ||
      result.as.bits != 42 || !observe.observed ||
      lainir_vm_control_state(control) != LAINIR_VM_DEAD) {
    lainir_module_handle_destroy(&handle);
    lainir_caps_free(caps);
    lainir_vm_control_free(control);
    return fail("interpreter continuation did not resume to completion");
  }
  lainir_vm_control_free(control);
  lainir_caps_free(caps);
  lainir_module_handle_destroy(&handle);
  puts("PASS LAIN-VM opaque control API");
  return 0;
}
