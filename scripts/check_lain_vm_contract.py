#!/usr/bin/env python3
"""Exercise the provider-neutral single-VSpace/single-TCB VM contract."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class VSpace:
    owner: int
    allocation_limit: int
    bytes_used: int = 0
    reset_count: int = 0

    def allocate(self, owner: int, size: int) -> None:
        if owner != self.owner or size < 0:
            raise ValueError("invalid VSpace owner or allocation")
        if self.allocation_limit and self.bytes_used + size > self.allocation_limit:
            raise MemoryError("VSpace allocation quota exceeded")
        self.bytes_used += size

    def reset(self, owner: int) -> None:
        if owner != self.owner:
            raise ValueError("VSpace reset by a non-owner")
        self.bytes_used = 0
        self.reset_count += 1


@dataclass
class TCB:
    owner: int
    vspace: VSpace
    step_limit: int
    capability_mask: int
    state: str = "READY"
    steps_used: int = 0
    position: int = 0
    saved_position: int = 0

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

    def suspend(self, owner: int) -> None:
        if owner != self.owner or self.state != "RUNNING":
            raise ValueError("TCB suspend rejected")
        self.saved_position = self.position
        self.state = "BLOCKED"

    def resume(self, owner: int) -> None:
        if owner != self.owner or self.state != "BLOCKED":
            raise ValueError("TCB resume rejected")
        self.position = self.saved_position
        self.state = "RUNNING"

    def finish(self, owner: int) -> None:
        if owner != self.owner or self.state != "RUNNING":
            raise ValueError("TCB finish rejected")
        self.state = "DEAD"
        self.vspace.reset(owner)


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


@dataclass
class OwnedHandle:
    """A provider-neutral result/arena handle with explicit ownership."""

    owner: int
    released: bool = False

    def transfer(self, owner: int, target: int) -> None:
        if self.released or owner != self.owner or target == owner:
            raise ValueError("handle ownership transfer rejected")
        self.owner = target

    def release(self, owner: int) -> None:
        if self.released or owner != self.owner:
            raise ValueError("handle release rejected")
        self.released = True

    def use(self, owner: int) -> None:
        if self.released or owner != self.owner:
            raise ValueError("released or foreign handle used")


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
    tcb.set_position(17)
    tcb.consume_step()
    tcb.consume_step()
    expect_failure(tcb.consume_step, RuntimeError)
    tcb.suspend(7)
    if tcb.state != "BLOCKED" or tcb.saved_position != 17:
        raise SystemExit("TCB suspend did not preserve execution position")
    expect_failure(lambda: tcb.suspend(7), ValueError)
    tcb.position = 99
    tcb.resume(7)
    if tcb.state != "RUNNING" or tcb.position != 17:
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
    tcb.finish(7)
    if vspace.bytes_used != 0 or vspace.reset_count != 1 or tcb.state != "DEAD":
        raise SystemExit("TCB finish did not reset its VSpace")
    handle = OwnedHandle(owner=7)
    handle.use(7)
    handle.transfer(7, 9)
    handle.use(9)
    expect_failure(lambda: handle.use(7), ValueError)
    handle.release(9)
    expect_failure(lambda: handle.use(9), ValueError)
    expect_failure(lambda: handle.release(9), ValueError)
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
    print("PASS LAIN-VM single-VSpace/single-TCB contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
