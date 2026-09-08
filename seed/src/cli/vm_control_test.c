#include "lainir/interpreter.h"
#include "lainir/eval_source.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *message) {
  fprintf(stderr, "vm control test: %s\n", message);
  return 1;
}

typedef struct {
  LainirVmControl *control;
  uint64_t expected_procedure;
  int observed;
} ObserveContext;

typedef struct {
  uint32_t calls;
} CounterContext;

static LainirRunStatus make_value(
    const LainirValue *args, uint32_t arg_count, LainirValue *result_out,
    const char **error_out, void *user_data) {
  (void)args;
  (void)error_out;
  CounterContext *context = user_data;
  if (context) context->calls++;
  if (arg_count != 0 || !result_out) return LAINIR_RUN_BAD_CALL;
  *result_out = lainir_value_bits(99, 64);
  return LAINIR_RUN_OK;
}

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

static int endpoint_contract_test(void) {
  LainirVmEndpoint *endpoint = lainir_vm_endpoint_new(7);
  LainirVmControl *sender = lainir_vm_control_new(16);
  LainirVmControl *receiver = lainir_vm_control_new(16);
  LainirVmControl *foreign = lainir_vm_control_new(16);
  uint64_t value = 0;
  uint32_t result_kind = 0;
  uint64_t result_value = 0;
  int ok = endpoint && sender && receiver &&
           lainir_vm_control_start(sender, 7) &&
           lainir_vm_control_start(receiver, 7) &&
           lainir_vm_control_start(foreign, 8) &&
           lainir_vm_endpoint_send(endpoint, 8, foreign, 8, 1) == -1 &&
           lainir_vm_endpoint_send(endpoint, 7, sender, 7, 99) == 0 &&
           lainir_vm_control_state(sender) == LAINIR_VM_BLOCKED &&
           lainir_vm_control_suspend_reason(sender) ==
               LAINIR_VM_SUSPEND_ENDPOINT &&
           lainir_vm_endpoint_send(endpoint, 7, sender, 7, 100) == -1 &&
           lainir_vm_endpoint_receive(endpoint, 7, receiver, 7, &value) == 1 &&
           value == 99 && lainir_vm_control_state(sender) == LAINIR_VM_RUNNING &&
           lainir_vm_control_take_endpoint_result(
               sender, 7, &result_kind, &result_value) &&
           result_kind == LAINIR_VM_ENDPOINT_RESULT_SEND &&
           result_value == 1 &&
           lainir_vm_endpoint_receive(endpoint, 7, receiver, 7, NULL) == 0 &&
           lainir_vm_control_state(receiver) == LAINIR_VM_BLOCKED &&
           lainir_vm_endpoint_cancel(endpoint, 7, receiver, 7) == 1 &&
           lainir_vm_control_state(receiver) == LAINIR_VM_RUNNING &&
           lainir_vm_endpoint_send(endpoint, 7, sender, 7, 123) == 0 &&
           lainir_vm_endpoint_cancel(endpoint, 7, sender, 7) == 1 &&
           lainir_vm_control_state(sender) == LAINIR_VM_RUNNING;
  if (sender && lainir_vm_control_state(sender) == LAINIR_VM_RUNNING)
    (void)lainir_vm_control_finish(sender, 7);
  if (receiver && lainir_vm_control_state(receiver) == LAINIR_VM_RUNNING)
    (void)lainir_vm_control_finish(receiver, 7);
  lainir_vm_endpoint_free(endpoint);
  lainir_vm_control_free(sender);
  lainir_vm_control_free(receiver);
  if (foreign && lainir_vm_control_state(foreign) == LAINIR_VM_RUNNING)
    (void)lainir_vm_control_finish(foreign, 8);
  lainir_vm_control_free(foreign);
  return ok;
}

static int endpoint_dispatch_test(void) {
  const char *source =
      "#extern #proc endpoint.send(#bits<64> %value) -> #bits<32>;\n"
      "#extern #proc endpoint.receive() -> #bits<64>;\n"
      "#extern #proc test.make_value() -> #bits<64>;\n"
      "#proc receive_leaf() -> #bits<64> {\n"
      "  #return #call endpoint.receive()\n"
      "}\n"
      "#proc receive_helper() -> #bits<64> {\n"
      "  #return #call receive_leaf()\n"
      "}\n"
      "#proc receive_main() -> #bits<64> {\n"
      "  #return #call receive_helper()\n"
      "}\n"
      "#proc send_helper() -> #bits<32> {\n"
      "  #let %value: #bits<64> = #call test.make_value()\n"
      "  #return #call endpoint.send(%value)\n"
      "}\n"
      "#proc send_outer() -> #bits<32> {\n"
      "  #return #call send_helper()\n"
      "}\n"
      "#proc send_main() -> #bits<32> {\n"
      "  #return #call send_outer()\n"
      "}\n";
  L1Diagnostic diagnostic = {0};
  LainirModuleHandle *handle = NULL;
  LainirVmEndpoint *endpoint = NULL;
  LainirVmControl *receiver = NULL;
  LainirVmControl *sender = NULL;
  LainirCapabilityTable *receiver_caps = NULL;
  LainirCapabilityTable *sender_caps = NULL;
  CounterContext counter = {0};
  int ok = 0;
  if (lainir_module_parse_handle(source, &handle, &diagnostic) != LAINIR_RUN_OK)
    goto cleanup;
  if (lainir_module_handle_verify_entry(handle, "receive_main", &diagnostic) !=
          LAINIR_RUN_OK ||
      lainir_module_handle_verify_entry(handle, "send_main", &diagnostic) !=
          LAINIR_RUN_OK)
    goto cleanup;
  endpoint = lainir_vm_endpoint_new(7);
  receiver = lainir_vm_control_new(16);
  sender = lainir_vm_control_new(16);
  receiver_caps = lainir_caps_new();
  sender_caps = lainir_caps_new();
  LainirVmEndpointBinding receiver_binding = {endpoint, receiver, 7};
  LainirVmEndpointBinding sender_binding = {endpoint, sender, 7};
  if (!endpoint || !receiver || !sender || !receiver_caps || !sender_caps ||
      !lainir_vm_control_start(receiver, 7) ||
      !lainir_vm_control_start(sender, 7) ||
      !lainir_vm_endpoint_bind(receiver_caps, "endpoint.send",
                               "endpoint.receive", &receiver_binding) ||
      !lainir_vm_endpoint_bind(sender_caps, "endpoint.send",
                               "endpoint.receive", &sender_binding) ||
      !lainir_caps_add(sender_caps, "test.make_value", make_value,
                       &counter))
    goto cleanup;
  LainirRunRequest receiver_request = {0};
  receiver_request.module = (L1Subroutine *)lainir_module_handle_first(handle);
  receiver_request.entry_name = "receive_main";
  receiver_request.caps = receiver_caps;
  receiver_request.vm_control = receiver;
  receiver_request.vm_owner = 7;
  LainirValue receiver_result = {0};
  LainirRunRequest sender_request = {0};
  sender_request.module = (L1Subroutine *)lainir_module_handle_first(handle);
  sender_request.entry_name = "send_main";
  sender_request.caps = sender_caps;
  sender_request.vm_control = sender;
  sender_request.vm_owner = 7;
  LainirValue sender_result = {0};
  const char *error = NULL;
  if (!lainir_vm_control_begin_slice(sender, 7, 1) ||
      lainir_run(&sender_request, &sender_result, &error) !=
          LAINIR_RUN_BLOCKED || error ||
      counter.calls != 1 ||
      lainir_vm_control_state(sender) != LAINIR_VM_BLOCKED)
    goto cleanup;
  error = NULL;
  if (!lainir_vm_control_begin_slice(receiver, 7, 1) ||
      lainir_run(&receiver_request, &receiver_result, &error) != LAINIR_RUN_OK ||
      error || receiver_result.kind != LAINIR_VALUE_BITS ||
      receiver_result.as.bits != 99 ||
      lainir_vm_control_state(receiver) != LAINIR_VM_DEAD)
    goto cleanup;
  error = NULL;
  if (!lainir_vm_control_begin_slice(sender, 7, 1) ||
      lainir_run(&sender_request, &sender_result, &error) != LAINIR_RUN_OK ||
      error || sender_result.kind != LAINIR_VALUE_BITS ||
      sender_result.as.bits != 1 || counter.calls != 1 ||
      lainir_vm_control_state(sender) != LAINIR_VM_DEAD)
    goto cleanup;
  ok = 1;
cleanup:
  if (receiver && lainir_vm_control_state(receiver) == LAINIR_VM_RUNNING)
    (void)lainir_vm_control_finish(receiver, 7);
  if (sender && lainir_vm_control_state(sender) == LAINIR_VM_RUNNING)
    (void)lainir_vm_control_finish(sender, 7);
  lainir_caps_free(receiver_caps);
  lainir_caps_free(sender_caps);
  lainir_vm_control_free(receiver);
  lainir_vm_control_free(sender);
  lainir_vm_endpoint_free(endpoint);
  lainir_module_handle_destroy(&handle);
  return ok;
}

static int trap_record_test(void) {
  const char *source =
      "#proc helper() -> #bits<32> {\n"
      "  #return #call missing()\n"
      "}\n"
      "#proc main() -> #bits<32> {\n"
      "  #return #call helper()\n"
      "}\n";
  L1Diagnostic diagnostic = {0};
  LainirModuleHandle *handle = NULL;
  LainirVmControl *control = NULL;
  if (lainir_module_parse_handle(source, &handle, &diagnostic) != LAINIR_RUN_OK)
    return 0;
  control = lainir_vm_control_new(8);
  if (!control || !lainir_vm_control_start(control, 7) ||
      !lainir_vm_control_begin_slice(control, 7, 1)) {
    lainir_vm_control_free(control);
    lainir_module_handle_destroy(&handle);
    return 0;
  }
  LainirRunRequest request = {0};
  request.module = (L1Subroutine *)lainir_module_handle_first(handle);
  request.entry_name = "main";
  request.vm_control = control;
  request.vm_owner = 7;
  LainirValue result = {0};
  const char *error = NULL;
  const L1Subroutine *helper = lainir_module_handle_first(handle);
  LainirRunStatus status = lainir_run(&request, &result, &error);
  const LainirVmTrap *trap = lainir_vm_control_trap(control);
  int ok = status == LAINIR_RUN_TRAP && error && trap && trap->active &&
           trap->kind == LAINIR_VM_TRAP_INTERPRETER &&
           trap->status == LAINIR_RUN_TRAP && trap->procedure &&
           helper && trap->procedure == (uint64_t)(uintptr_t)helper &&
           trap->region && trap->position && trap->line == 2 &&
           trap->column > 0 && trap->source_end > trap->source_start &&
           source[trap->source_start] == '#' &&
           !strncmp(source + trap->source_start, "#call", 5) &&
           source[trap->source_end - 1] == ')' &&
           lainir_vm_control_state(control) == LAINIR_VM_DEAD &&
           lainir_vm_control_slice_result(control) == LAINIR_VM_TRAPPED;
  LainirVmTrap consumed = {0};
  ok = ok && lainir_vm_control_take_trap(control, 7, &consumed) &&
       consumed.kind == LAINIR_VM_TRAP_INTERPRETER && consumed.active &&
       lainir_vm_control_trap(control) == NULL &&
       lainir_vm_control_state(control) == LAINIR_VM_DEAD &&
       lainir_vm_control_slice_result(control) == LAINIR_VM_TRAPPED &&
       !lainir_vm_control_take_trap(control, 7, &consumed);
  lainir_vm_control_free(control);
  lainir_module_handle_destroy(&handle);
  return ok;
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
  if (!endpoint_contract_test()) return fail("endpoint rendezvous contract failed");
  if (!endpoint_dispatch_test())
    return fail("endpoint capability dispatch failed");
  if (!trap_record_test()) return fail("structured trap record failed");

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
  if (!lainir_vm_control_suspend(control, 9, 3) ||
      lainir_vm_control_state(control) != LAINIR_VM_BLOCKED ||
      !lainir_vm_control_resume(control, 9) ||
      lainir_vm_control_state(control) != LAINIR_VM_RUNNING) {
    lainir_module_handle_destroy(&handle);
    lainir_caps_free(caps);
    lainir_vm_control_free(control);
    return fail("interpreter continuation suspend/resume failed");
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
