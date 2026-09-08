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
  uint32_t endpoint_result_kind;
  uint64_t endpoint_result_value;
  LainirVmTrap trap;
};

struct LainirVmEndpoint {
  uint64_t owner;
  LainirVmControl *sender;
  uint64_t sender_owner;
  uint64_t sender_value;
  LainirVmControl *receiver;
  uint64_t receiver_owner;
};

struct LainirVmScheduler {
  uint64_t owner;
  LainirVmControl *current;
  uint64_t current_owner;
  LainirVmControl *attached[8];
  uint64_t attached_owner[8];
  uint32_t attached_count;
  uint32_t next_index;
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

uint32_t lainir_vm_control_suspend_reason(const LainirVmControl *control) {
  return control ? control->suspend_reason : 0;
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

const LainirVmTrap *lainir_vm_control_trap(const LainirVmControl *control) {
  return control && control->trap.active ? &control->trap : NULL;
}

int lainir_vm_control_take_trap(LainirVmControl *control, uint64_t owner,
                                LainirVmTrap *trap_out) {
  if (!vm_owned(control, owner) || !trap_out ||
      control->state != LAINIR_VM_DEAD ||
      control->result != LAINIR_VM_TRAPPED || !control->trap.active)
    return 0;
  *trap_out = control->trap;
  control->trap.active = 0;
  return 1;
}

int lainir_vm_control_record_trap(LainirVmControl *control, uint64_t owner,
                                  LainirVmTrapKind kind, int32_t status) {
  return lainir_vm_control_record_trap_at(control, owner, kind, status, 0, 0);
}

int lainir_vm_control_record_trap_at(
    LainirVmControl *control, uint64_t owner, LainirVmTrapKind kind,
    int32_t status, uint32_t line, uint32_t column) {
  return lainir_vm_control_record_trap_span(
      control, owner, kind, status, line, column, 0, 0);
}

int lainir_vm_control_record_trap_span(
    LainirVmControl *control, uint64_t owner, LainirVmTrapKind kind,
    int32_t status, uint32_t line, uint32_t column,
    uint64_t source_start, uint64_t source_end) {
  if (!vm_owned(control, owner) || control->trap.active) return 0;
  const LainirVmFrame *frame = lainir_vm_control_current_frame(control);
  control->trap.kind = kind;
  control->trap.status = status;
  control->trap.procedure = frame ? frame->procedure : 0;
  control->trap.region = frame ? frame->region : 0;
  control->trap.position = frame ? frame->position : 0;
  control->trap.line = line;
  control->trap.column = column;
  control->trap.source_start = source_start;
  control->trap.source_end = source_end;
  control->trap.active = 1;
  control->result = LAINIR_VM_TRAPPED;
  return 1;
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

int lainir_vm_control_take_endpoint_result(LainirVmControl *control,
                                           uint64_t owner, uint32_t *kind_out,
                                           uint64_t *value_out) {
  if (!vm_owned(control, owner) || !control->endpoint_result_kind)
    return 0;
  if (kind_out) *kind_out = control->endpoint_result_kind;
  if (value_out) *value_out = control->endpoint_result_value;
  control->endpoint_result_kind = 0;
  control->endpoint_result_value = 0;
  return 1;
}

int lainir_vm_control_has_endpoint_result(const LainirVmControl *control,
                                           uint64_t owner,
                                           uint32_t *kind_out) {
  if (!vm_owned(control, owner) || !control->endpoint_result_kind)
    return 0;
  if (kind_out) *kind_out = control->endpoint_result_kind;
  return 1;
}

static int endpoint_set_result(LainirVmControl *control, uint64_t owner,
                               uint32_t kind, uint64_t value) {
  if (!vm_owned(control, owner) ||
      control->state != LAINIR_VM_BLOCKED || control->endpoint_result_kind)
    return 0;
  control->endpoint_result_kind = kind;
  control->endpoint_result_value = value;
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
    if (!endpoint_set_result(receiver, receiver_owner,
                             LAINIR_VM_ENDPOINT_RESULT_RECEIVE, value))
      return -1;
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
    if (!endpoint_set_result(sender, sender_owner,
                             LAINIR_VM_ENDPOINT_RESULT_SEND, 1))
      return -1;
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
    if (!endpoint_set_result(control, control_owner,
                             LAINIR_VM_ENDPOINT_RESULT_CANCELLED, 0))
      return -1;
    endpoint->sender = NULL;
    endpoint->sender_owner = 0;
    return lainir_vm_control_resume(control, control_owner) ? 1 : -1;
  }
  if (endpoint->receiver == control &&
      endpoint->receiver_owner == control_owner) {
    if (!endpoint_set_result(control, control_owner,
                             LAINIR_VM_ENDPOINT_RESULT_CANCELLED, 0))
      return -1;
    endpoint->receiver = NULL;
    endpoint->receiver_owner = 0;
    return lainir_vm_control_resume(control, control_owner) ? 1 : -1;
  }
  return 0;
}

static LainirRunStatus endpoint_send_capability(
    const LainirValue *args, uint32_t arg_count, LainirValue *result_out,
    const char **error_out, void *user_data) {
  LainirVmEndpointBinding *binding = user_data;
  if (!binding || !binding->endpoint || !binding->control || arg_count != 1 ||
      !result_out || args[0].kind != LAINIR_VALUE_BITS) {
    if (error_out) *error_out = "endpoint.send expects one bits payload";
    return LAINIR_RUN_BAD_CALL;
  }
  uint32_t kind = 0;
  uint64_t value = 0;
  if (lainir_vm_control_take_endpoint_result(
          binding->control, binding->owner, &kind, &value)) {
    if (kind == LAINIR_VM_ENDPOINT_RESULT_SEND ||
        kind == LAINIR_VM_ENDPOINT_RESULT_CANCELLED) {
      *result_out = lainir_value_bits(value, 32);
      return LAINIR_RUN_OK;
    }
    if (error_out) *error_out = "endpoint.send resumed with wrong result";
    return LAINIR_RUN_BAD_CALL;
  }
  int result = lainir_vm_endpoint_send(
      binding->endpoint, binding->owner, binding->control, binding->owner,
      args[0].as.bits);
  if (result == 0) return LAINIR_RUN_BLOCKED;
  if (result < 0) {
    if (error_out) *error_out = "endpoint.send rejected";
    return LAINIR_RUN_BAD_CALL;
  }
  *result_out = lainir_value_bits(1, 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus endpoint_receive_capability(
    const LainirValue *args, uint32_t arg_count, LainirValue *result_out,
    const char **error_out, void *user_data) {
  LainirVmEndpointBinding *binding = user_data;
  if (!binding || !binding->endpoint || !binding->control || arg_count != 0 ||
      !result_out) {
    if (error_out) *error_out = "endpoint.receive expects no arguments";
    return LAINIR_RUN_BAD_CALL;
  }
  uint32_t kind = 0;
  uint64_t value = 0;
  if (lainir_vm_control_take_endpoint_result(
          binding->control, binding->owner, &kind, &value)) {
    if (kind == LAINIR_VM_ENDPOINT_RESULT_RECEIVE) {
      *result_out = lainir_value_bits(value, 64);
      return LAINIR_RUN_OK;
    }
    if (kind == LAINIR_VM_ENDPOINT_RESULT_CANCELLED) {
      *result_out = lainir_value_bits(0, 64);
      return LAINIR_RUN_OK;
    }
    if (error_out) *error_out = "endpoint.receive resumed with wrong result";
    return LAINIR_RUN_BAD_CALL;
  }
  uint64_t received = 0;
  int result = lainir_vm_endpoint_receive(
      binding->endpoint, binding->owner, binding->control, binding->owner,
      &received);
  if (result == 0) return LAINIR_RUN_BLOCKED;
  if (result < 0) {
    if (error_out) *error_out = "endpoint.receive rejected";
    return LAINIR_RUN_BAD_CALL;
  }
  *result_out = lainir_value_bits(received, 64);
  return LAINIR_RUN_OK;
}

int lainir_vm_endpoint_bind(LainirCapabilityTable *caps,
                            const char *send_name, const char *receive_name,
                            LainirVmEndpointBinding *binding) {
  if (!caps || !binding || !binding->endpoint || !binding->control ||
      !binding->owner || !send_name || !receive_name)
    return 0;
  return lainir_caps_add(caps, send_name, endpoint_send_capability, binding) &&
         lainir_caps_add(caps, receive_name, endpoint_receive_capability,
                         binding);
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

LainirVmSliceResult lainir_vm_control_abort(LainirVmControl *control,
                                            uint64_t owner) {
  if (!vm_owned(control, owner) || control->state == LAINIR_VM_DEAD)
    return LAINIR_VM_TRAPPED;
  control->frame_count = 0;
  control->backend_state = NULL;
  control->state = LAINIR_VM_DEAD;
  control->slice_fuel = 0;
  control->slice_exhausted = 0;
  control->result = LAINIR_VM_TRAPPED;
  return control->result;
}

LainirVmScheduler *lainir_vm_scheduler_new(uint64_t owner) {
  LainirVmScheduler *scheduler = calloc(1, sizeof(*scheduler));
  if (!scheduler || !owner) {
    free(scheduler);
    return NULL;
  }
  scheduler->owner = owner;
  return scheduler;
}

void lainir_vm_scheduler_free(LainirVmScheduler *scheduler) {
  free(scheduler);
}

static int scheduler_owned(const LainirVmScheduler *scheduler,
                           uint64_t owner) {
  return scheduler && scheduler->owner != 0 && scheduler->owner == owner;
}

static int scheduler_attached(const LainirVmScheduler *scheduler,
                              LainirVmControl *control, uint64_t owner) {
  if (!scheduler || !control) return 0;
  for (uint32_t i = 0; i < scheduler->attached_count; i++)
    if (scheduler->attached[i] == control && scheduler->attached_owner[i] == owner)
      return 1;
  return 0;
}

int lainir_vm_scheduler_attach(LainirVmScheduler *scheduler, uint64_t owner,
                               LainirVmControl *control, uint64_t control_owner) {
  if (!scheduler_owned(scheduler, owner) || !control || !control_owner ||
      scheduler->attached_count == 8 ||
      scheduler_attached(scheduler, control, control_owner))
    return 0;
  scheduler->attached[scheduler->attached_count] = control;
  scheduler->attached_owner[scheduler->attached_count++] = control_owner;
  return 1;
}

LainirVmControl *lainir_vm_scheduler_current(
    const LainirVmScheduler *scheduler) {
  return scheduler ? scheduler->current : NULL;
}

int lainir_vm_scheduler_start(LainirVmScheduler *scheduler, uint64_t owner,
                              LainirVmControl *control, uint64_t control_owner) {
  if (!scheduler_owned(scheduler, owner) || scheduler->current ||
      !scheduler_attached(scheduler, control, control_owner) ||
      !lainir_vm_control_start(control, control_owner))
    return 0;
  scheduler->current = control;
  scheduler->current_owner = control_owner;
  return 1;
}

int lainir_vm_scheduler_suspend(LainirVmScheduler *scheduler, uint64_t owner,
                                uint32_t reason) {
  if (!scheduler_owned(scheduler, owner) || !scheduler->current ||
      !lainir_vm_control_suspend(scheduler->current, scheduler->current_owner,
                                 reason))
    return 0;
  scheduler->current = NULL;
  scheduler->current_owner = 0;
  return 1;
}

int lainir_vm_scheduler_resume(LainirVmScheduler *scheduler, uint64_t owner,
                               LainirVmControl *control, uint64_t control_owner) {
  if (!scheduler_owned(scheduler, owner) || scheduler->current ||
      !scheduler_attached(scheduler, control, control_owner) ||
      !lainir_vm_control_resume(control, control_owner))
    return 0;
  scheduler->current = control;
  scheduler->current_owner = control_owner;
  return 1;
}

int lainir_vm_scheduler_release(LainirVmScheduler *scheduler, uint64_t owner) {
  if (!scheduler_owned(scheduler, owner) || !scheduler->current ||
      (lainir_vm_control_state(scheduler->current) != LAINIR_VM_BLOCKED &&
       lainir_vm_control_state(scheduler->current) != LAINIR_VM_DEAD))
    return 0;
  scheduler->current = NULL;
  scheduler->current_owner = 0;
  return 1;
}

int lainir_vm_scheduler_admit(LainirVmScheduler *scheduler, uint64_t owner,
                              LainirVmControl *control, uint64_t control_owner) {
  if (!scheduler_owned(scheduler, owner) || scheduler->current ||
      !scheduler_attached(scheduler, control, control_owner) ||
      lainir_vm_control_state(control) != LAINIR_VM_RUNNING)
    return 0;
  scheduler->current = control;
  scheduler->current_owner = control_owner;
  return 1;
}

LainirVmControl *lainir_vm_scheduler_select(
    LainirVmScheduler *scheduler, uint64_t owner, uint64_t *control_owner_out) {
  if (!scheduler_owned(scheduler, owner) || scheduler->current ||
      !scheduler->attached_count)
    return NULL;
  for (uint32_t offset = 0; offset < scheduler->attached_count; offset++) {
    uint32_t index = (scheduler->next_index + offset) % scheduler->attached_count;
    LainirVmControl *control = scheduler->attached[index];
    uint64_t control_owner = scheduler->attached_owner[index];
    LainirVmState state = lainir_vm_control_state(control);
    int admitted = 0;
    if (state == LAINIR_VM_READY)
      admitted = lainir_vm_scheduler_start(
          scheduler, owner, control, control_owner);
    else if (state == LAINIR_VM_RUNNING)
      admitted = lainir_vm_scheduler_admit(
          scheduler, owner, control, control_owner);
    if (admitted) {
      scheduler->next_index = (index + 1) % scheduler->attached_count;
      if (control_owner_out) *control_owner_out = control_owner;
      return control;
    }
  }
  return NULL;
}

LainirRunStatus lainir_vm_scheduler_run(
    LainirVmScheduler *scheduler, uint64_t owner, uint64_t fuel,
    const LainirRunRequest *request, LainirValue *result_out,
    const char **error_out) {
  LainirRunRequest run_request;
  LainirVmControl *control;
  uint64_t control_owner;
  LainirRunStatus status;
  if (!scheduler_owned(scheduler, owner) || !scheduler->current || !request ||
      !result_out || fuel == 0)
    return LAINIR_RUN_TRAP;
  control = scheduler->current;
  control_owner = scheduler->current_owner;
  if (lainir_vm_control_state(control) != LAINIR_VM_RUNNING ||
      (request->vm_control && request->vm_control != control))
    return LAINIR_RUN_TRAP;
  if (!lainir_vm_control_begin_slice(control, control_owner, fuel))
    return LAINIR_RUN_TRAP;
  run_request = *request;
  run_request.vm_control = control;
  run_request.vm_owner = control_owner;
  status = lainir_run(&run_request, result_out, error_out);
  if (lainir_vm_control_state(control) == LAINIR_VM_BLOCKED ||
      lainir_vm_control_state(control) == LAINIR_VM_DEAD) {
    (void)lainir_vm_scheduler_release(scheduler, owner);
  }
  return status;
}

LainirVmSliceResult lainir_vm_scheduler_finish(
    LainirVmScheduler *scheduler, uint64_t owner) {
  LainirVmSliceResult result;
  if (!scheduler_owned(scheduler, owner) || !scheduler->current)
    return LAINIR_VM_TRAPPED;
  result = lainir_vm_control_finish(scheduler->current,
                                    scheduler->current_owner);
  if (result == LAINIR_VM_DONE) {
    scheduler->current = NULL;
    scheduler->current_owner = 0;
  }
  return result;
}
