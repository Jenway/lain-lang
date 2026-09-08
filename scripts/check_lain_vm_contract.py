#!/usr/bin/env python3
"""Exercise the provider-neutral TCB, VSpace, Trap, and scheduler contract."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class ArenaHandle:
    """Provider-owned handle invalidated by the VSpace generation reset."""

    owner: int
    generation: int
    vspace_id: int
    released: bool = False

    def use(self, vspace: "VSpace", owner: int) -> None:
        if (
            self.released
            or owner != self.owner
            or self.generation != vspace.generation
            or self.vspace_id != id(vspace)
            or vspace.released
        ):
            raise ValueError("stale or foreign arena handle")


@dataclass
class ContinuationFrame:
    procedure: int
    region: int
    position: int
    activation: int
    return_procedure: int
    return_region: int
    return_position: int


@dataclass
class VSpace:
    owner: int
    allocation_limit: int
    bytes_used: int = 0
    reset_count: int = 0
    provider_release_count: int = 0
    generation: int = 1
    released: bool = False
    handles: list[ArenaHandle] = None  # type: ignore[assignment]

    def __post_init__(self) -> None:
        if self.handles is None:
            self.handles = []

    def allocate(self, owner: int, size: int) -> None:
        if owner != self.owner or size < 0:
            raise ValueError("invalid VSpace owner or allocation")
        if self.allocation_limit and self.bytes_used + size > self.allocation_limit:
            raise MemoryError("VSpace allocation quota exceeded")
        self.bytes_used += size

    def reset(self, owner: int) -> None:
        if owner != self.owner or self.released:
            raise ValueError("VSpace reset by a non-owner")
        for handle in self.handles:
            handle.released = True
        self.bytes_used = 0
        self.provider_release_count += 1
        self.generation += 1
        self.reset_count += 1

    def allocate_handle(self, owner: int) -> ArenaHandle:
        if owner != self.owner or self.released:
            raise ValueError("VSpace handle allocation rejected")
        handle = ArenaHandle(
            owner=owner,
            generation=self.generation,
            vspace_id=id(self),
        )
        self.handles.append(handle)
        return handle

    def release(self, owner: int) -> None:
        if owner != self.owner or self.released:
            raise ValueError("VSpace release rejected")
        for handle in self.handles:
            handle.released = True
        self.released = True
        self.provider_release_count += 1
        self.generation += 1


@dataclass
class TCB:
    owner: int
    vspace: VSpace
    step_limit: int
    capability_mask: int
    state: str = "READY"
    steps_used: int = 0
    procedure: int = 0
    region: int = 0
    position: int = 0
    saved_position: int = 0
    suspend_reason: str = ""
    frames: list[ContinuationFrame] = None  # type: ignore[assignment]

    def __post_init__(self) -> None:
        if self.frames is None:
            self.frames = []

    def start(self, owner: int) -> None:
        if owner != self.owner or self.state != "READY":
            raise ValueError("TCB start rejected")
        self.state = "RUNNING"

    def consume_step(self) -> None:
        if self.state != "RUNNING":
            raise ValueError("TCB is not running")
        if self.step_limit and self.steps_used >= self.step_limit:
            raise RuntimeError("TCB step quota exceeded")
        self.steps_used += 1

    def set_position(self, position: int) -> None:
        if self.state != "RUNNING" or position < 0:
            raise ValueError("TCB position update rejected")
        self.position = position
        if self.frames:
            self.frames[-1].position = position

    def push_frame(self, procedure: int, region: int, activation: int) -> None:
        if self.state != "RUNNING":
            raise ValueError("continuation push rejected")
        self.frames.append(
            ContinuationFrame(
                procedure=procedure,
                region=region,
                position=0,
                activation=activation,
                return_procedure=self.procedure,
                return_region=self.region,
                return_position=self.position,
            )
        )
        self.procedure = procedure
        self.region = region
        self.position = 0

    def pop_frame(self) -> None:
        if self.state != "RUNNING" or not self.frames:
            raise ValueError("continuation pop rejected")
        frame = self.frames.pop()
        self.procedure = frame.return_procedure
        self.region = frame.return_region
        self.position = frame.return_position

    def suspend(self, owner: int, reason: str = "yield") -> None:
        if owner != self.owner or self.state != "RUNNING":
            raise ValueError("TCB suspend rejected")
        self.saved_position = self.position
        self.suspend_reason = reason
        self.state = "BLOCKED"

    def resume(self, owner: int) -> None:
        if owner != self.owner or self.state != "BLOCKED":
            raise ValueError("TCB resume rejected")
        self.position = self.saved_position
        self.suspend_reason = ""
        self.state = "RUNNING"

    def finish(self, owner: int) -> None:
        if owner != self.owner or self.state != "RUNNING":
            raise ValueError("TCB finish rejected")
        self.state = "DEAD"


@dataclass
class VmControl:
    """Opaque provider-owned controller for a resumable instruction slice."""

    owner: int
    tcb: TCB
    program: dict[tuple[int, int], list[tuple]]
    root_procedure: int
    root_region: int

    def __post_init__(self) -> None:
        self.tcb.start(self.owner)
        self.tcb.push_frame(self.root_procedure, self.root_region, 1)

    def run_slice(self, fuel: int) -> str:
        if fuel <= 0 or self.tcb.state != "RUNNING":
            raise ValueError("VM slice rejected")
        consumed = 0
        while consumed < fuel:
            if not self.tcb.frames:
                self.tcb.finish(self.owner)
                return "DONE"
            frame = self.tcb.frames[-1]
            code = self.program.get((frame.procedure, frame.region))
            if code is None:
                raise ValueError("missing continuation code")
            if frame.position >= len(code):
                self.tcb.pop_frame()
                continue
            operation = code[frame.position]
            self.tcb.set_position(frame.position + 1)
            self.tcb.consume_step()
            consumed += 1
            if operation[0] == "call":
                self.tcb.push_frame(operation[1], operation[2], operation[3])
            elif operation[0] == "return":
                self.tcb.pop_frame()
        return "RUNNABLE"


@dataclass
class Scheduler:
    """Single-runnable control plane used before multi-TCB scheduling."""

    current: int | None = None
    runnable: set[int] = None  # type: ignore[assignment]

    def __post_init__(self) -> None:
        if self.runnable is None:
            self.runnable = set()

    def add(self, tcb_id: int, tcb: TCB, owner: int) -> None:
        if owner != tcb.owner or tcb.state != "READY":
            raise ValueError("scheduler add rejected")
        self.runnable.add(tcb_id)

    def start(self, tcb_id: int, tcb: TCB, owner: int) -> None:
        if self.current is not None or tcb_id not in self.runnable:
            raise ValueError("scheduler start rejected")
        tcb.start(owner)
        self.runnable.remove(tcb_id)
        self.current = tcb_id

    def suspend(self, tcb_id: int, tcb: TCB, owner: int) -> None:
        if self.current != tcb_id:
            raise ValueError("scheduler suspend rejected")
        tcb.suspend(owner)
        self.current = None

    def resume(self, tcb_id: int, tcb: TCB, owner: int) -> None:
        if self.current is not None or tcb.state != "BLOCKED":
            raise ValueError("scheduler resume rejected")
        tcb.resume(owner)
        self.current = tcb_id


@dataclass
class Endpoint:
    sender: tuple[int, int] | None = None
    receiver: int | None = None

    def send(self, tcb: int, value: int) -> tuple[int, int] | None:
        if self.receiver is not None:
            receiver = self.receiver
            self.receiver = None
            return receiver, value
        if self.sender is not None:
            raise RuntimeError("endpoint already has a waiting sender")
        self.sender = (tcb, value)
        return None

    def receive(self, tcb: int) -> tuple[int, int] | None:
        if self.sender is not None:
            sender, value = self.sender
            self.sender = None
            return sender, value
        if self.receiver is not None:
            raise RuntimeError("endpoint already has a waiting receiver")
        self.receiver = tcb
        return None

    def cancel(self, tcb: int) -> bool:
        if self.sender is not None and self.sender[0] == tcb:
            self.sender = None
            return True
        if self.receiver == tcb:
            self.receiver = None
            return True
        return False


@dataclass(frozen=True)
class Trap:
    kind: str
    source: str
    status: int


@dataclass
class CSpace:
    capabilities: set[str]
    handles: list["Capability"] = None  # type: ignore[assignment]

    def __post_init__(self) -> None:
        if self.handles is None:
            self.handles = []

    def require(self, capability: str) -> None:
        if capability not in self.capabilities:
            raise PermissionError(f"capability denied: {capability}")

    def bind(self, capability: "Capability") -> None:
        self.require(capability.name)
        self.handles.append(capability)

    def require_handle(self, capability: "Capability", owner: int) -> None:
        if capability not in self.handles:
            raise PermissionError("capability is not present in CSpace")
        capability.check(owner)


@dataclass
class Capability:
    name: str
    owner: int
    active: bool = True

    def transfer(self, owner: int, target: int) -> None:
        if not self.active or owner != self.owner or target == owner:
            raise ValueError("capability transfer rejected")
        self.owner = target

    def revoke(self, owner: int) -> None:
        if not self.active or owner != self.owner:
            raise ValueError("capability revoke rejected")
        self.active = False

    def check(self, owner: int) -> None:
        if not self.active or owner != self.owner:
            raise PermissionError("capability owner or active state rejected")


def expect_failure(action, error: type[BaseException]) -> None:
    try:
        action()
    except error:
        return
    raise SystemExit(f"expected {error.__name__} was not raised")


def main() -> int:
    vspace = VSpace(owner=7, allocation_limit=16)
    tcb = TCB(owner=7, vspace=vspace, step_limit=2, capability_mask=0b001)
    expect_failure(lambda: vspace.allocate(8, 1), ValueError)
    vspace.allocate(7, 8)
    expect_failure(lambda: vspace.allocate(7, 9), MemoryError)
    tcb.start(7)
    tcb.push_frame(3, 5, 1)
    tcb.set_position(17)
    tcb.push_frame(4, 6, 2)
    tcb.set_position(23)
    if (
        len(tcb.frames) != 2
        or tcb.procedure != 4
        or tcb.region != 6
        or tcb.frames[-1].position != 23
    ):
        raise SystemExit("continuation frame push lost current position")
    tcb.pop_frame()
    if tcb.procedure != 3 or tcb.region != 5 or tcb.position != 17:
        raise SystemExit("continuation frame pop did not restore caller")
    tcb.consume_step()
    tcb.consume_step()
    expect_failure(tcb.consume_step, RuntimeError)
    tcb.suspend(7, "endpoint")
    if (
        tcb.state != "BLOCKED"
        or tcb.saved_position != 17
        or tcb.suspend_reason != "endpoint"
    ):
        raise SystemExit("TCB suspend did not preserve execution position")
    expect_failure(lambda: tcb.suspend(7), ValueError)
    tcb.position = 99
    tcb.resume(7)
    if tcb.state != "RUNNING" or tcb.position != 17 or tcb.suspend_reason:
        raise SystemExit("TCB resume did not restore execution position")
    expect_failure(lambda: tcb.resume(7), ValueError)
    scheduler = Scheduler()
    scheduled = TCB(owner=7, vspace=vspace, step_limit=0, capability_mask=0b001)
    scheduler.add(3, scheduled, 7)
    scheduler.start(3, scheduled, 7)
    expect_failure(lambda: scheduler.start(3, scheduled, 7), ValueError)
    scheduler.suspend(3, scheduled, 7)
    if scheduler.current is not None or scheduled.state != "BLOCKED":
        raise SystemExit("scheduler suspend did not release the current TCB")
    scheduler.resume(3, scheduled, 7)
    if scheduler.current != 3 or scheduled.state != "RUNNING":
        raise SystemExit("scheduler resume did not select the TCB")
    expect_failure(lambda: scheduler.resume(3, scheduled, 7), ValueError)
    scheduled.state = "DEAD"
    scheduler.current = None
    second = TCB(owner=7, vspace=vspace, step_limit=0, capability_mask=0b001)
    scheduler.add(4, second, 7)
    scheduled.state = "READY"
    scheduler.runnable.add(3)
    scheduler.start(3, scheduled, 7)
    expect_failure(lambda: scheduler.start(4, second, 7), ValueError)
    scheduler.suspend(3, scheduled, 7)
    scheduler.start(4, second, 7)
    if scheduler.current != 4 or second.state != "RUNNING":
        raise SystemExit("scheduler did not hand off to the second TCB")
    scheduler.suspend(4, second, 7)
    scheduler.resume(3, scheduled, 7)
    if scheduler.current != 3 or scheduled.state != "RUNNING":
        raise SystemExit("scheduler did not resume the first TCB")
    scheduler.suspend(3, scheduled, 7)
    expect_failure(lambda: tcb.finish(8), ValueError)
    arena_handle = vspace.allocate_handle(7)
    arena_handle.use(vspace, 7)
    generation_before_finish = vspace.generation
    bytes_before_finish = vspace.bytes_used
    tcb.finish(7)
    if (
        vspace.bytes_used != bytes_before_finish
        or vspace.reset_count != 0
        or vspace.provider_release_count != 0
        or vspace.generation != generation_before_finish
        or tcb.state != "DEAD"
    ):
        raise SystemExit("TCB finish changed its shared VSpace")
    arena_handle.use(vspace, 7)
    generation_after_finish = vspace.generation
    vspace.reset(7)
    if (
        vspace.generation != generation_after_finish + 1
        or vspace.provider_release_count != 1
    ):
        raise SystemExit("VSpace reset did not advance its generation")
    slice_space = VSpace(owner=7, allocation_limit=0)
    slice_tcb = TCB(owner=7, vspace=slice_space, step_limit=0, capability_mask=0)
    slice_vm = VmControl(
        owner=7,
        tcb=slice_tcb,
        root_procedure=1,
        root_region=0,
        program={
            (1, 0): [("call", 2, 1, 2), ("nop",), ("return",)],
            (2, 1): [("nop",), ("return",)],
        },
    )
    if slice_vm.run_slice(1) != "RUNNABLE":
        raise SystemExit("VM slice did not yield after fuel exhaustion")
    if (
        len(slice_tcb.frames) != 2
        or slice_tcb.procedure != 2
        or slice_tcb.region != 1
        or slice_tcb.position != 0
    ):
        raise SystemExit("VM slice did not preserve callee continuation")
    if slice_vm.run_slice(1) != "RUNNABLE":
        raise SystemExit("VM slice did not resume callee")
    if slice_tcb.procedure != 2 or slice_tcb.position != 1:
        raise SystemExit("VM slice lost callee position")
    while slice_tcb.state != "DEAD":
        slice_vm.run_slice(1)
    if slice_space.reset_count != 0:
        raise SystemExit("VM completion changed its shared VSpace")
    released_space = VSpace(owner=7, allocation_limit=0)
    released_handle = released_space.allocate_handle(7)
    released_space.release(7)
    if released_space.provider_release_count != 1:
        raise SystemExit("VSpace release did not release its provider arena")
    expect_failure(lambda: released_handle.use(released_space, 7), ValueError)
    expect_failure(lambda: released_space.release(7), ValueError)
    sibling_space = VSpace(owner=7, allocation_limit=0)
    sibling_arena = sibling_space.allocate_handle(7)
    expect_failure(lambda: sibling_arena.use(vspace, 7), ValueError)
    sibling_arena.use(sibling_space, 7)
    first_space = VSpace(owner=7, allocation_limit=0)
    second_space = VSpace(owner=7, allocation_limit=0)
    first_arena = first_space.allocate_handle(7)
    second_arena = second_space.allocate_handle(7)
    first_tcb = TCB(owner=7, vspace=first_space, step_limit=0, capability_mask=0)
    second_tcb = TCB(owner=7, vspace=second_space, step_limit=0, capability_mask=0)
    first_tcb.start(7)
    second_tcb.start(7)
    second_generation = second_space.generation
    first_tcb.finish(7)
    first_arena.use(first_space, 7)
    second_arena.use(second_space, 7)
    if second_space.generation != second_generation or second_tcb.state != "RUNNING":
        raise SystemExit("TCB finish crossed VSpace lifetime boundary")
    second_tcb.finish(7)
    endpoint = Endpoint()
    if endpoint.send(1, 42) is not None:
        raise SystemExit("endpoint send without receiver did not block")
    if endpoint.receive(2) != (1, 42):
        raise SystemExit("endpoint rendezvous did not transfer the payload")
    if endpoint.sender is not None or endpoint.receiver is not None:
        raise SystemExit("endpoint retained a rendezvous after delivery")
    if endpoint.receive(4) is not None:
        raise SystemExit("endpoint receive unexpectedly completed")
    if not endpoint.cancel(4) or endpoint.receiver is not None:
        raise SystemExit("endpoint receiver cancellation did not clear wait")
    if endpoint.cancel(4):
        raise SystemExit("endpoint cancelled the same receiver twice")
    if endpoint.send(5, 99) is not None:
        raise SystemExit("endpoint send without receiver did not block")
    if not endpoint.cancel(5) or endpoint.sender is not None:
        raise SystemExit("endpoint sender cancellation did not clear wait")
    cspace = CSpace({"eval.read"})
    cspace.require("eval.read")
    expect_failure(lambda: cspace.require("host.fs"), PermissionError)
    capability = Capability("eval.read", 7)
    cspace.bind(capability)
    cspace.require_handle(capability, 7)
    capability.transfer(7, 9)
    cspace.require_handle(capability, 9)
    expect_failure(lambda: cspace.require_handle(capability, 7), PermissionError)
    capability.revoke(9)
    expect_failure(lambda: cspace.require_handle(capability, 9), PermissionError)
    expect_failure(lambda: capability.revoke(9), ValueError)
    trap = Trap("quota", "eval.main", 7101)
    if (trap.kind, trap.source, trap.status) != ("quota", "eval.main", 7101):
        raise SystemExit("trap record lost its classification")
    print("PASS LAIN-VM TCB/VSpace contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
