#include "lainir/interpreter.h"

#include <stdio.h>

static int fail(const char *message) {
  fprintf(stderr, "vm control test: %s\n", message);
  return 1;
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
  puts("PASS LAIN-VM opaque control API");
  return 0;
}
