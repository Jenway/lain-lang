#include "lainir/interpreter.h"

#include <stdlib.h>
#include <string.h>

struct LainirVmControl {
  uint64_t owner;
  uint64_t max_steps;
  uint64_t steps;
  uint64_t slice_fuel;
  uint32_t suspend_reason;
  LainirVmState state;
  LainirVmSliceResult result;
  int slice_exhausted;
  LainirVmFrame *frames;
  uint32_t frame_count;
  uint32_t frame_capacity;
  void *backend_state;
};

struct LainirVmEndpoint {
  uint64_t owner;
  LainirVmControl *sender;
  uint64_t sender_owner;
  uint64_t sender_value;
  LainirVmControl *receiver;
  uint64_t receiver_owner;
};

static int vm_owned(const LainirVmControl *control, uint64_t owner) {
  return control && control->owner != 0 && control->owner == owner;
}

LainirVmControl *lainir_vm_control_new(uint64_t max_steps) {
  LainirVmControl *control = calloc(1, sizeof(*control));
  if (!control) return NULL;
  control->max_steps = max_steps;
  control->state = LAINIR_VM_READY;
  control->result = LAINIR_VM_RUNNABLE;
  return control;
}

void lainir_vm_control_free(LainirVmControl *control) {
  if (!control) return;
  free(control->frames);
  free(control);
}

LainirVmState lainir_vm_control_state(const LainirVmControl *control) {
  return control ? control->state : LAINIR_VM_DEAD;
}

uint64_t lainir_vm_control_steps(const LainirVmControl *control) {
  return control ? control->steps : 0;
}

int lainir_vm_control_start(LainirVmControl *control, uint64_t owner) {
  if (!control || control->state != LAINIR_VM_READY || owner == 0) return 0;
  control->owner = owner;
  control->state = LAINIR_VM_RUNNING;
  control->result = LAINIR_VM_RUNNABLE;
  return 1;
}

int lainir_vm_control_suspend(LainirVmControl *control, uint64_t owner,
                              uint32_t reason) {
  if (!vm_owned(control, owner) || control->state != LAINIR_VM_RUNNING)
    return 0;
  control->suspend_reason = reason;
  control->state = LAINIR_VM_BLOCKED;
  control->result = LAINIR_VM_BLOCKED_RESULT;
  return 1;
}

int lainir_vm_control_resume(LainirVmControl *control, uint64_t owner) {
  if (!vm_owned(control, owner) || control->state != LAINIR_VM_BLOCKED)
    return 0;
  control->suspend_reason = 0;
  control->state = LAINIR_VM_RUNNING;
  control->result = LAINIR_VM_RUNNABLE;
  return 1;
}

int lainir_vm_control_begin_slice(LainirVmControl *control, uint64_t owner,
                                  uint64_t fuel) {
  if (!vm_owned(control, owner) || control->state != LAINIR_VM_RUNNING ||
      fuel == 0)
    return 0;
  control->slice_fuel = fuel;
  control->slice_exhausted = 0;
  control->result = LAINIR_VM_RUNNABLE;
  return 1;
}

int lainir_vm_control_consume_step(LainirVmControl *control, uint64_t owner) {
  if (!vm_owned(control, owner) || control->state != LAINIR_VM_RUNNING)
    return 0;
  if (control->max_steps && control->steps >= control->max_steps) {
    control->result = LAINIR_VM_TRAPPED;
    return 0;
  }
  if (control->slice_fuel == 0) {
    control->slice_exhausted = 1;
    control->result = LAINIR_VM_RUNNABLE;
    return 0;
  }
  control->slice_fuel--;
  control->steps++;
  return 1;
}

int lainir_vm_control_slice_exhausted(const LainirVmControl *control) {
  return control ? control->slice_exhausted : 0;
}

LainirVmSliceResult lainir_vm_control_slice_result(
    const LainirVmControl *control) {
  return control ? control->result : LAINIR_VM_TRAPPED;
}

int lainir_vm_control_push_frame(LainirVmControl *control, uint64_t owner,
                                 uint64_t procedure, uint64_t region,
                                 uint64_t activation) {
  if (!vm_owned(control, owner) || control->state != LAINIR_VM_RUNNING)
    return 0;
  if (control->frame_count == control->frame_capacity) {
    uint32_t next = control->frame_capacity ? control->frame_capacity * 2 : 4;
    LainirVmFrame *frames = realloc(control->frames, next * sizeof(*frames));
    if (!frames) return 0;
    control->frames = frames;
    control->frame_capacity = next;
  }
  LainirVmFrame frame = {
    .procedure = procedure,
    .region = region,
    .position = 0,
    .activation = activation,
    .return_procedure = 0,
    .return_region = 0,
    .return_position = 0,
  };
  if (control->frame_count) {
    const LainirVmFrame *caller =
        &control->frames[control->frame_count - 1];
    frame.return_procedure = caller->procedure;
    frame.return_region = caller->region;
    frame.return_position = caller->position;
  }
  control->frames[control->frame_count++] = frame;
  return 1;
}

int lainir_vm_control_set_position(LainirVmControl *control, uint64_t owner,
                                   uint64_t region, uint64_t position) {
  if (!vm_owned(control, owner) || control->state != LAINIR_VM_RUNNING ||
      !control->frame_count)
    return 0;
  LainirVmFrame *frame = &control->frames[control->frame_count - 1];
  frame->region = region;
  frame->position = position;
  return 1;
}

int lainir_vm_control_pop_frame(LainirVmControl *control, uint64_t owner) {
  if (!vm_owned(control, owner) || control->state != LAINIR_VM_RUNNING ||
      !control->frame_count)
    return 0;
  control->frame_count--;
  return 1;
}

const LainirVmFrame *lainir_vm_control_current_frame(
    const LainirVmControl *control) {
  if (!control || !control->frame_count) return NULL;
  return &control->frames[control->frame_count - 1];
}

void *lainir_vm_control_backend_state(const LainirVmControl *control) {
  return control ? control->backend_state : NULL;
}

int lainir_vm_control_set_backend_state(LainirVmControl *control,
                                        uint64_t owner, void *state) {
  if (!vm_owned(control, owner) || control->state == LAINIR_VM_DEAD)
    return 0;
  control->backend_state = state;
  return 1;
}

LainirVmEndpoint *lainir_vm_endpoint_new(uint64_t owner) {
  if (!owner) return NULL;
  LainirVmEndpoint *endpoint = calloc(1, sizeof(*endpoint));
  if (!endpoint) return NULL;
  endpoint->owner = owner;
  return endpoint;
}

void lainir_vm_endpoint_free(LainirVmEndpoint *endpoint) {
  if (!endpoint) return;
  free(endpoint);
}

static int endpoint_owned(const LainirVmEndpoint *endpoint, uint64_t owner) {
  return endpoint && endpoint->owner != 0 && endpoint->owner == owner;
}

int lainir_vm_endpoint_send(LainirVmEndpoint *endpoint, uint64_t owner,
                            LainirVmControl *sender, uint64_t sender_owner,
                            uint64_t value) {
  if (!endpoint_owned(endpoint, owner) || !vm_owned(sender, sender_owner) ||
      sender->state != LAINIR_VM_RUNNING)
    return -1;
  if (endpoint->sender) return -1;
  if (endpoint->receiver) {
    LainirVmControl *receiver = endpoint->receiver;
    uint64_t receiver_owner = endpoint->receiver_owner;
    endpoint->receiver = NULL;
    endpoint->receiver_owner = 0;
    if (!lainir_vm_control_resume(receiver, receiver_owner)) return -1;
    return 1;
  }
  if (!lainir_vm_control_suspend(sender, sender_owner,
                                 LAINIR_VM_SUSPEND_ENDPOINT))
    return -1;
  endpoint->sender = sender;
  endpoint->sender_owner = sender_owner;
  endpoint->sender_value = value;
  return 0;
}

int lainir_vm_endpoint_receive(LainirVmEndpoint *endpoint, uint64_t owner,
                               LainirVmControl *receiver,
                               uint64_t receiver_owner, uint64_t *value_out) {
  if (!endpoint_owned(endpoint, owner) || !vm_owned(receiver, receiver_owner) ||
      receiver->state != LAINIR_VM_RUNNING)
    return -1;
  if (endpoint->receiver) return -1;
  if (endpoint->sender) {
    LainirVmControl *sender = endpoint->sender;
    uint64_t sender_owner = endpoint->sender_owner;
    uint64_t value = endpoint->sender_value;
    endpoint->sender = NULL;
    endpoint->sender_owner = 0;
    if (!lainir_vm_control_resume(sender, sender_owner)) return -1;
    if (value_out) *value_out = value;
    return 1;
  }
  if (!lainir_vm_control_suspend(receiver, receiver_owner,
                                 LAINIR_VM_SUSPEND_ENDPOINT))
    return -1;
  endpoint->receiver = receiver;
  endpoint->receiver_owner = receiver_owner;
  return 0;
}

int lainir_vm_endpoint_cancel(LainirVmEndpoint *endpoint, uint64_t owner,
                              LainirVmControl *control, uint64_t control_owner) {
  if (!endpoint_owned(endpoint, owner) || !vm_owned(control, control_owner))
    return -1;
  if (endpoint->sender == control && endpoint->sender_owner == control_owner) {
    endpoint->sender = NULL;
    endpoint->sender_owner = 0;
    return lainir_vm_control_resume(control, control_owner) ? 1 : -1;
  }
  if (endpoint->receiver == control &&
      endpoint->receiver_owner == control_owner) {
    endpoint->receiver = NULL;
    endpoint->receiver_owner = 0;
    return lainir_vm_control_resume(control, control_owner) ? 1 : -1;
  }
  return 0;
}

LainirVmSliceResult lainir_vm_control_finish(LainirVmControl *control,
                                             uint64_t owner) {
  if (!vm_owned(control, owner) || control->state != LAINIR_VM_RUNNING)
    return LAINIR_VM_TRAPPED;
  control->frame_count = 0;
  control->backend_state = NULL;
  control->state = LAINIR_VM_DEAD;
  control->slice_fuel = 0;
  control->result = LAINIR_VM_DONE;
  return control->result;
}
