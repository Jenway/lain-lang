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

static int endpoint_destroy_cleanup_test(void) {
  LainirVmEndpoint *endpoint = lainir_vm_endpoint_new(7);
  LainirVmControl *aborted = lainir_vm_control_new(8);
  LainirVmControl *waiting = lainir_vm_control_new(8);
  uint32_t kind = 0;
  uint64_t value = 0;
  int ok = endpoint && aborted && waiting &&
           lainir_vm_control_start(aborted, 7) &&
           lainir_vm_endpoint_send(endpoint, 7, aborted, 7, 1) == 0 &&
           lainir_vm_control_abort(aborted, 7) == LAINIR_VM_TRAPPED &&
           lainir_vm_endpoint_cancel(endpoint, 7, aborted, 7) == 1 &&
           lainir_vm_control_start(waiting, 7) &&
           lainir_vm_endpoint_receive(endpoint, 7, waiting, 7, NULL) == 0;
  lainir_vm_endpoint_free(endpoint);
  ok = ok && lainir_vm_control_state(waiting) == LAINIR_VM_RUNNING &&
       lainir_vm_control_take_endpoint_result(
           waiting, 7, &kind, &value) &&
       kind == LAINIR_VM_ENDPOINT_RESULT_CANCELLED && value == 0;
  if (aborted) lainir_vm_control_free(aborted);
  if (waiting) lainir_vm_control_free(waiting);
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
      "  #if 1 {\n"
      "    #return #add(#call test.make_value(), #call receive_leaf())\n"
      "  }\n"
      "  #return 0\n"
      "}\n"
      "#proc receive_main() -> #bits<64> {\n"
      "  #return #call receive_helper()\n"
      "}\n"
      "#proc send_helper() -> #bits<32> {\n"
      "  #let %value: #bits<64> = #call test.make_value()\n"
      "  #if 1 {\n"
      "    #return #call endpoint.send(%value)\n"
      "  }\n"
      "  #return 0\n"
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
      !lainir_caps_add(receiver_caps, "test.make_value", make_value,
                       &counter) ||
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
      lainir_vm_control_state(sender) != LAINIR_VM_BLOCKED) {
    goto cleanup;
  }
  error = NULL;
  if (!lainir_vm_control_begin_slice(receiver, 7, 1) ||
      lainir_run(&receiver_request, &receiver_result, &error) != LAINIR_RUN_OK ||
      error || receiver_result.kind != LAINIR_VALUE_BITS ||
      receiver_result.as.bits != 198 ||
      lainir_vm_control_state(receiver) != LAINIR_VM_DEAD) {
    goto cleanup;
  }
  error = NULL;
  if (!lainir_vm_control_begin_slice(sender, 7, 1) ||
      lainir_run(&sender_request, &sender_result, &error) != LAINIR_RUN_OK ||
      error || sender_result.kind != LAINIR_VALUE_BITS ||
      sender_result.as.bits != 1 || counter.calls != 2 ||
      lainir_vm_control_state(sender) != LAINIR_VM_DEAD) {
    goto cleanup;
  }
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

static int scheduler_handoff_test(void) {
  LainirVmScheduler *scheduler = lainir_vm_scheduler_new(7);
  LainirVmControl *first = lainir_vm_control_new(8);
  LainirVmControl *second = lainir_vm_control_new(8);
  int ok = scheduler && first && second &&
           lainir_vm_scheduler_attach(scheduler, 7, first, 11) &&
           lainir_vm_scheduler_attach(scheduler, 7, second, 12) &&
           lainir_vm_scheduler_start(scheduler, 7, first, 11) &&
           lainir_vm_scheduler_current(scheduler) == first &&
           !lainir_vm_scheduler_start(scheduler, 7, second, 12) &&
           lainir_vm_scheduler_suspend(scheduler, 7, LAINIR_VM_SUSPEND_YIELD) &&
           !lainir_vm_scheduler_current(scheduler) &&
           lainir_vm_control_state(first) == LAINIR_VM_BLOCKED &&
           lainir_vm_scheduler_start(scheduler, 7, second, 12) &&
           lainir_vm_scheduler_current(scheduler) == second &&
           !lainir_vm_scheduler_resume(scheduler, 7, first, 11) &&
           lainir_vm_scheduler_suspend(scheduler, 7, LAINIR_VM_SUSPEND_YIELD) &&
           lainir_vm_scheduler_resume(scheduler, 7, first, 11) &&
           lainir_vm_scheduler_current(scheduler) == first &&
           lainir_vm_scheduler_finish(scheduler, 7) == LAINIR_VM_DONE &&
           !lainir_vm_scheduler_current(scheduler) &&
           lainir_vm_control_state(first) == LAINIR_VM_DEAD;
  if (second && lainir_vm_control_state(second) == LAINIR_VM_RUNNING)
    (void)lainir_vm_control_finish(second, 12);
  lainir_vm_scheduler_free(scheduler);
  lainir_vm_control_free(first);
  lainir_vm_control_free(second);
  return ok;
}

static int scheduler_endpoint_handoff_test(void) {
  LainirVmScheduler *scheduler = lainir_vm_scheduler_new(7);
  LainirVmEndpoint *endpoint = lainir_vm_endpoint_new(7);
  LainirVmControl *sender = lainir_vm_control_new(8);
  LainirVmControl *receiver = lainir_vm_control_new(8);
  uint32_t result_kind = 0;
  uint64_t result_value = 0;
  uint64_t received = 0;
  int ok = scheduler && endpoint && sender && receiver &&
           lainir_vm_scheduler_attach(scheduler, 7, sender, 11) &&
           lainir_vm_scheduler_attach(scheduler, 7, receiver, 12) &&
           lainir_vm_scheduler_start(scheduler, 7, sender, 11) &&
           lainir_vm_endpoint_send(endpoint, 7, sender, 11, 42) == 0 &&
           lainir_vm_control_state(sender) == LAINIR_VM_BLOCKED &&
           lainir_vm_scheduler_release(scheduler, 7) &&
           lainir_vm_scheduler_start(scheduler, 7, receiver, 12) &&
           lainir_vm_endpoint_receive(endpoint, 7, receiver, 12, &received) == 1 &&
           received == 42 &&
           lainir_vm_scheduler_finish(scheduler, 7) == LAINIR_VM_DONE &&
           lainir_vm_scheduler_admit(scheduler, 7, sender, 11) &&
           lainir_vm_control_take_endpoint_result(
               sender, 11, &result_kind, &result_value) &&
           result_kind == LAINIR_VM_ENDPOINT_RESULT_SEND &&
           result_value == 1;
  if (sender && lainir_vm_control_state(sender) == LAINIR_VM_RUNNING)
    (void)lainir_vm_control_finish(sender, 11);
  lainir_vm_scheduler_free(scheduler);
  lainir_vm_endpoint_free(endpoint);
  lainir_vm_control_free(sender);
  lainir_vm_control_free(receiver);
  return ok;
}

static int scheduler_evaluator_handoff_test(void) {
  const char *source =
      "#extern #proc endpoint.send(#bits<64> %value) -> #bits<32>;\n"
      "#extern #proc endpoint.receive() -> #bits<64>;\n"
      "#proc receive() -> #bits<64> {\n"
      "  #return #call endpoint.receive()\n"
      "}\n"
      "#proc send() -> #bits<32> {\n"
      "  #return #call endpoint.send(42)\n"
      "}\n";
  L1Diagnostic diagnostic = {0};
  LainirModuleHandle *handle = NULL;
  LainirVmScheduler *scheduler = NULL;
  LainirVmEndpoint *endpoint = NULL;
  LainirVmControl *sender = NULL;
  LainirVmControl *receiver = NULL;
  LainirCapabilityTable *sender_caps = NULL;
  LainirCapabilityTable *receiver_caps = NULL;
  LainirValue sender_result = {0};
  LainirValue receiver_result = {0};
  LainirRunRequest sender_request = {0};
  LainirRunRequest receiver_request = {0};
  const char *error = NULL;
  LainirRunStatus run_status = LAINIR_RUN_TRAP;
  int ok = 0;
  if (lainir_module_parse_handle(source, &handle, &diagnostic) != LAINIR_RUN_OK)
    goto cleanup;
  scheduler = lainir_vm_scheduler_new(7);
  endpoint = lainir_vm_endpoint_new(7);
  sender = lainir_vm_control_new(16);
  receiver = lainir_vm_control_new(16);
  sender_caps = lainir_caps_new();
  receiver_caps = lainir_caps_new();
  if (!scheduler || !endpoint || !sender || !receiver || !sender_caps ||
      !receiver_caps || !lainir_vm_scheduler_attach(scheduler, 7, sender, 7) ||
      !lainir_vm_scheduler_attach(scheduler, 7, receiver, 7))
    goto cleanup;
  LainirVmEndpointBinding sender_binding = {endpoint, sender, 7};
  LainirVmEndpointBinding receiver_binding = {endpoint, receiver, 7};
  if (!lainir_vm_endpoint_bind(sender_caps, "endpoint.send", "endpoint.receive",
                               &sender_binding) ||
      !lainir_vm_endpoint_bind(receiver_caps, "endpoint.send", "endpoint.receive",
                               &receiver_binding) ||
      !lainir_vm_scheduler_select(scheduler, 7, NULL))
    goto cleanup;
  sender_request.module = (L1Subroutine *)lainir_module_handle_first(handle);
  sender_request.entry_name = "send";
  sender_request.caps = sender_caps;
  receiver_request.module = sender_request.module;
  receiver_request.entry_name = "receive";
  receiver_request.caps = receiver_caps;
  if (lainir_vm_scheduler_run_slice(
          scheduler, 7, 8, &sender_request, &run_status, &sender_result,
          &error) != LAINIR_VM_BLOCKED_RESULT ||
      run_status != LAINIR_RUN_BLOCKED ||
      lainir_vm_scheduler_current(scheduler) != NULL ||
      lainir_vm_control_state(sender) != LAINIR_VM_BLOCKED) {
    goto cleanup;
  }
  error = NULL;
  if (!lainir_vm_scheduler_select(scheduler, 7, NULL) ||
      lainir_vm_scheduler_run_slice(
          scheduler, 7, 8, &receiver_request, &run_status, &receiver_result,
          &error) != LAINIR_VM_DONE ||
      run_status != LAINIR_RUN_OK ||
      error || receiver_result.kind != LAINIR_VALUE_BITS ||
      receiver_result.as.bits != 42 ||
      lainir_vm_scheduler_current(scheduler) != NULL) {
    goto cleanup;
  }
  error = NULL;
  if (!lainir_vm_scheduler_select(scheduler, 7, NULL) ||
      lainir_vm_scheduler_run_slice(
          scheduler, 7, 8, &sender_request, &run_status, &sender_result,
          &error) != LAINIR_VM_DONE ||
      run_status != LAINIR_RUN_OK ||
      error || sender_result.kind != LAINIR_VALUE_BITS ||
      sender_result.as.bits != 1 ||
      lainir_vm_scheduler_current(scheduler) != NULL) {
    goto cleanup;
  }
  ok = 1;
cleanup:
  if (sender && lainir_vm_control_state(sender) == LAINIR_VM_RUNNING)
    (void)lainir_vm_control_finish(sender, 7);
  if (receiver && lainir_vm_control_state(receiver) == LAINIR_VM_RUNNING)
    (void)lainir_vm_control_finish(receiver, 7);
  lainir_caps_free(sender_caps);
  lainir_caps_free(receiver_caps);
  lainir_vm_scheduler_free(scheduler);
  lainir_vm_endpoint_free(endpoint);
  lainir_vm_control_free(sender);
  lainir_vm_control_free(receiver);
  lainir_module_handle_destroy(&handle);
  return ok;
}

static int scheduler_parallel_endpoint_wait_test(void) {
  const char *source =
      "#extern #proc endpoint.a_send(#bits<64> %value) -> #bits<32>;\n"
      "#extern #proc endpoint.a_receive() -> #bits<64>;\n"
      "#extern #proc endpoint.b_send(#bits<64> %value) -> #bits<32>;\n"
      "#extern #proc endpoint.b_receive() -> #bits<64>;\n"
      "#proc send_a_leaf() -> #bits<32> {\n"
      "  #return #call endpoint.a_send(10)\n"
      "}\n"
      "#proc send_a() -> #bits<32> {\n"
      "  #return #call send_a_leaf()\n"
      "}\n"
      "#proc send_b_leaf() -> #bits<32> {\n"
      "  #return #call endpoint.b_send(20)\n"
      "}\n"
      "#proc send_b() -> #bits<32> {\n"
      "  #return #call send_b_leaf()\n"
      "}\n"
      "#proc receive_a() -> #bits<64> {\n"
      "  #return #call endpoint.a_receive()\n"
      "}\n"
      "#proc receive_b() -> #bits<64> {\n"
      "  #return #call endpoint.b_receive()\n"
      "}\n";
  L1Diagnostic diagnostic = {0};
  LainirModuleHandle *handle = NULL;
  LainirVmScheduler *scheduler = NULL;
  LainirVmEndpoint *endpoint_a = NULL;
  LainirVmEndpoint *endpoint_b = NULL;
  LainirVmControl *send_a = NULL;
  LainirVmControl *send_b = NULL;
  LainirVmControl *receive_a = NULL;
  LainirVmControl *receive_b = NULL;
  LainirCapabilityTable *send_a_caps = NULL;
  LainirCapabilityTable *send_b_caps = NULL;
  LainirCapabilityTable *receive_a_caps = NULL;
  LainirCapabilityTable *receive_b_caps = NULL;
  LainirValue result_a = {0};
  LainirValue result_b = {0};
  LainirValue received_a = {0};
  LainirValue received_b = {0};
  LainirRunRequest request_a = {0};
  LainirRunRequest request_b = {0};
  LainirRunRequest receive_request_a = {0};
  LainirRunRequest receive_request_b = {0};
  const char *error = NULL;
  int ok = 0;
  if (lainir_module_parse_handle(source, &handle, &diagnostic) != LAINIR_RUN_OK)
    goto cleanup;
  scheduler = lainir_vm_scheduler_new(7);
  endpoint_a = lainir_vm_endpoint_new(7);
  endpoint_b = lainir_vm_endpoint_new(7);
  send_a = lainir_vm_control_new(16);
  send_b = lainir_vm_control_new(16);
  receive_a = lainir_vm_control_new(16);
  receive_b = lainir_vm_control_new(16);
  send_a_caps = lainir_caps_new();
  send_b_caps = lainir_caps_new();
  receive_a_caps = lainir_caps_new();
  receive_b_caps = lainir_caps_new();
  if (!scheduler || !endpoint_a || !endpoint_b || !send_a || !send_b ||
      !receive_a || !receive_b || !send_a_caps || !send_b_caps ||
      !receive_a_caps || !receive_b_caps ||
      !lainir_vm_scheduler_attach(scheduler, 7, send_a, 7) ||
      !lainir_vm_scheduler_attach(scheduler, 7, send_b, 7) ||
      !lainir_vm_scheduler_attach(scheduler, 7, receive_a, 7) ||
      !lainir_vm_scheduler_attach(scheduler, 7, receive_b, 7))
    goto cleanup;
  LainirVmEndpointBinding send_a_binding = {endpoint_a, send_a, 7};
  LainirVmEndpointBinding receive_a_binding = {endpoint_a, receive_a, 7};
  LainirVmEndpointBinding send_b_binding = {endpoint_b, send_b, 7};
  LainirVmEndpointBinding receive_b_binding = {endpoint_b, receive_b, 7};
  if (!lainir_vm_endpoint_bind(send_a_caps, "endpoint.a_send",
                               "endpoint.a_receive", &send_a_binding) ||
      !lainir_vm_endpoint_bind(receive_a_caps, "endpoint.a_send",
                               "endpoint.a_receive", &receive_a_binding) ||
      !lainir_vm_endpoint_bind(send_b_caps, "endpoint.b_send",
                               "endpoint.b_receive", &send_b_binding) ||
      !lainir_vm_endpoint_bind(receive_b_caps, "endpoint.b_send",
                               "endpoint.b_receive", &receive_b_binding) ||
      !lainir_vm_scheduler_select(scheduler, 7, NULL))
    goto cleanup;
  request_a.module = (L1Subroutine *)lainir_module_handle_first(handle);
  request_a.entry_name = "send_a";
  request_a.caps = send_a_caps;
  request_b = request_a;
  request_b.entry_name = "send_b";
  request_b.caps = send_b_caps;
  receive_request_a = request_a;
  receive_request_a.entry_name = "receive_a";
  receive_request_a.caps = receive_a_caps;
  receive_request_b = request_a;
  receive_request_b.entry_name = "receive_b";
  receive_request_b.caps = receive_b_caps;
  if (lainir_vm_scheduler_run(scheduler, 7, 8, &request_a, &result_a, &error) !=
          LAINIR_RUN_BLOCKED ||
      !lainir_vm_scheduler_select(scheduler, 7, NULL) || error ||
      lainir_vm_scheduler_run(scheduler, 7, 8, &request_b, &result_b, &error) !=
          LAINIR_RUN_BLOCKED ||
      !lainir_vm_scheduler_select(scheduler, 7, NULL) || error)
    goto cleanup;
  if (lainir_vm_scheduler_run(scheduler, 7, 8, &receive_request_a,
                              &received_a, &error) != LAINIR_RUN_OK ||
      error || received_a.kind != LAINIR_VALUE_BITS || received_a.as.bits != 10 ||
      !lainir_vm_scheduler_select(scheduler, 7, NULL) ||
      lainir_vm_scheduler_run(scheduler, 7, 8, &receive_request_b,
                              &received_b, &error) != LAINIR_RUN_OK ||
      error || received_b.kind != LAINIR_VALUE_BITS || received_b.as.bits != 20 ||
      !lainir_vm_scheduler_select(scheduler, 7, NULL))
    goto cleanup;
  if (lainir_vm_scheduler_run(scheduler, 7, 8, &request_a, &result_a, &error) !=
          LAINIR_RUN_OK ||
      error || result_a.kind != LAINIR_VALUE_BITS || result_a.as.bits != 1 ||
      !lainir_vm_scheduler_select(scheduler, 7, NULL) ||
      lainir_vm_scheduler_run(scheduler, 7, 8, &request_b, &result_b, &error) !=
          LAINIR_RUN_OK ||
      error || result_b.kind != LAINIR_VALUE_BITS || result_b.as.bits != 1 ||
      lainir_vm_scheduler_current(scheduler) != NULL)
    goto cleanup;
  ok = 1;
cleanup:
  if (send_a && lainir_vm_control_state(send_a) == LAINIR_VM_RUNNING)
    (void)lainir_vm_control_finish(send_a, 7);
  if (send_b && lainir_vm_control_state(send_b) == LAINIR_VM_RUNNING)
    (void)lainir_vm_control_finish(send_b, 7);
  if (receive_a && lainir_vm_control_state(receive_a) == LAINIR_VM_RUNNING)
    (void)lainir_vm_control_finish(receive_a, 7);
  if (receive_b && lainir_vm_control_state(receive_b) == LAINIR_VM_RUNNING)
    (void)lainir_vm_control_finish(receive_b, 7);
  lainir_caps_free(send_a_caps);
  lainir_caps_free(send_b_caps);
  lainir_caps_free(receive_a_caps);
  lainir_caps_free(receive_b_caps);
  lainir_vm_scheduler_free(scheduler);
  lainir_vm_endpoint_free(endpoint_a);
  lainir_vm_endpoint_free(endpoint_b);
  lainir_vm_control_free(send_a);
  lainir_vm_control_free(send_b);
  lainir_vm_control_free(receive_a);
  lainir_vm_control_free(receive_b);
  lainir_module_handle_destroy(&handle);
  return ok;
}

static int scheduler_nested_cancel_test(void) {
  const char *source =
      "#extern #proc endpoint.send(#bits<64> %value) -> #bits<32>;\n"
      "#proc send_leaf() -> #bits<32> {\n"
      "  #return #call endpoint.send(42)\n"
      "}\n"
      "#proc send() -> #bits<32> {\n"
      "  #return #call send_leaf()\n"
      "}\n";
  L1Diagnostic diagnostic = {0};
  LainirModuleHandle *handle = NULL;
  LainirVmScheduler *scheduler = NULL;
  LainirVmEndpoint *endpoint = NULL;
  LainirVmControl *control = NULL;
  LainirCapabilityTable *caps = NULL;
  LainirVmEndpointBinding binding = {0};
  LainirRunRequest request = {0};
  LainirValue result = {0};
  LainirRunStatus run_status = LAINIR_RUN_TRAP;
  const char *error = NULL;
  int ok = 0;
  if (lainir_module_parse_handle(source, &handle, &diagnostic) != LAINIR_RUN_OK)
    goto cleanup;
  scheduler = lainir_vm_scheduler_new(7);
  endpoint = lainir_vm_endpoint_new(7);
  control = lainir_vm_control_new(16);
  caps = lainir_caps_new();
  binding.endpoint = endpoint;
  binding.control = control;
  binding.owner = 7;
  if (!scheduler || !endpoint || !control || !caps ||
      !lainir_vm_scheduler_attach(scheduler, 7, control, 7) ||
      !lainir_vm_endpoint_bind(caps, "endpoint.send", "endpoint.receive",
                               &binding) ||
      !lainir_vm_scheduler_select(scheduler, 7, NULL))
    goto cleanup;
  request.module = (L1Subroutine *)lainir_module_handle_first(handle);
  request.entry_name = "send";
  request.caps = caps;
  if (lainir_vm_scheduler_run_slice(
          scheduler, 7, 8, &request, &run_status, &result, &error) !=
          LAINIR_VM_BLOCKED_RESULT ||
      run_status != LAINIR_RUN_BLOCKED || error ||
      lainir_vm_control_state(control) != LAINIR_VM_BLOCKED ||
      lainir_vm_scheduler_current(scheduler) != NULL ||
      lainir_vm_endpoint_cancel(endpoint, 7, control, 7) != 1 ||
      !lainir_vm_scheduler_select(scheduler, 7, NULL) ||
      lainir_vm_scheduler_run_slice(
          scheduler, 7, 8, &request, &run_status, &result, &error) !=
          LAINIR_VM_DONE ||
      run_status != LAINIR_RUN_OK || error || result.kind != LAINIR_VALUE_BITS ||
      result.as.bits != 0 || lainir_vm_scheduler_current(scheduler) != NULL)
    goto cleanup;
  ok = 1;
cleanup:
  if (control && lainir_vm_control_state(control) == LAINIR_VM_RUNNING)
    (void)lainir_vm_control_finish(control, 7);
  lainir_caps_free(caps);
  lainir_vm_scheduler_free(scheduler);
  lainir_vm_endpoint_free(endpoint);
  lainir_vm_control_free(control);
  lainir_module_handle_destroy(&handle);
  return ok;
}

static int scheduler_fair_rotation_test(void) {
  const char *source =
      "#proc work() -> #bits<32> {\n"
      "  #let %a: #bits<32> = 1\n"
      "  #let %b: #bits<32> = 2\n"
      "  #let %c: #bits<32> = #add(%a, %b)\n"
      "  #return #add(%c, 4)\n"
      "}\n";
  L1Diagnostic diagnostic = {0};
  LainirModuleHandle *handle = NULL;
  LainirVmScheduler *scheduler = NULL;
  LainirVmControl *controls[3] = {NULL, NULL, NULL};
  LainirValue results[3] = {{0}, {0}, {0}};
  LainirRunRequest request = {0};
  const char *error = NULL;
  LainirRunStatus run_status = LAINIR_RUN_TRAP;
  int ok = 0;
  if (lainir_module_parse_handle(source, &handle, &diagnostic) != LAINIR_RUN_OK)
    goto cleanup;
  scheduler = lainir_vm_scheduler_new(7);
  if (!scheduler) goto cleanup;
  for (int i = 0; i < 3; i++) {
    controls[i] = lainir_vm_control_new(16);
    if (!controls[i] || !lainir_vm_scheduler_attach(scheduler, 7, controls[i], 7))
      goto cleanup;
  }
  request.module = (L1Subroutine *)lainir_module_handle_first(handle);
  request.entry_name = "work";
  for (int i = 0; i < 3; i++) {
    if (lainir_vm_scheduler_select(scheduler, 7, NULL) != controls[i] ||
        lainir_vm_scheduler_run_slice(
            scheduler, 7, 1, &request, &run_status, &results[i], &error) !=
            LAINIR_VM_RUNNABLE ||
        run_status != LAINIR_RUN_SLICE || error ||
        lainir_vm_scheduler_current(scheduler) != NULL ||
        lainir_vm_control_state(controls[i]) != LAINIR_VM_RUNNING)
      goto cleanup;
    error = NULL;
  }
  for (int i = 0; i < 3; i++) {
    if (lainir_vm_scheduler_select(scheduler, 7, NULL) != controls[i] ||
        lainir_vm_scheduler_run_slice(
            scheduler, 7, 16, &request, &run_status, &results[i], &error) !=
            LAINIR_VM_DONE ||
        run_status != LAINIR_RUN_OK || error || results[i].kind != LAINIR_VALUE_BITS ||
        results[i].as.bits != 7 || lainir_vm_scheduler_current(scheduler) != NULL)
      goto cleanup;
    error = NULL;
  }
  ok = 1;
cleanup:
  for (int i = 0; i < 3; i++) {
    if (controls[i] && lainir_vm_control_state(controls[i]) == LAINIR_VM_RUNNING)
      (void)lainir_vm_control_finish(controls[i], 7);
    lainir_vm_control_free(controls[i]);
  }
  lainir_vm_scheduler_free(scheduler);
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
  if (!endpoint_destroy_cleanup_test())
    return fail("endpoint destroy cleanup failed");
  if (!endpoint_dispatch_test())
    return fail("endpoint capability dispatch failed");
  if (!trap_record_test()) return fail("structured trap record failed");
  if (!scheduler_handoff_test()) return fail("scheduler handoff failed");
  if (!scheduler_endpoint_handoff_test())
    return fail("scheduler endpoint handoff failed");
  if (!scheduler_evaluator_handoff_test())
    return fail("scheduler evaluator handoff failed");
  if (!scheduler_parallel_endpoint_wait_test())
    return fail("scheduler parallel endpoint wait failed");
  if (!scheduler_nested_cancel_test())
    return fail("scheduler nested cancellation failed");
  if (!scheduler_fair_rotation_test())
    return fail("scheduler fair rotation failed");

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
