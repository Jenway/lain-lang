#!/usr/bin/env python3
"""Behavior contract shared by the seed provider and a recording test provider."""

from __future__ import annotations

import os
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEED_BIN = ROOT / "seed" / "zig-out" / "bin"
PRINT = SEED_BIN / ("lainir-print.exe" if os.name == "nt" else "lainir-print")
RUN = SEED_BIN / ("lainir-seed.exe" if os.name == "nt" else "lainir-seed")


@dataclass
class RecordingBuilder:
    """Small independent provider used to enforce observable API semantics."""

    procedures: list[str] = field(default_factory=list)
    data: list[str] = field(default_factory=list)

    @staticmethod
    def bits_type(width: int) -> str:
        if width <= 0:
            raise ValueError("bit width must be positive")
        return f"#bits<{width}>"

    @staticmethod
    def integer_literal(value: int) -> str:
        return str(value)

    @staticmethod
    def add(left: str, right: str) -> str:
        return f"#add({left}, {right})"

    @staticmethod
    def binary(operation: str, left: str, right: str) -> str:
        return f"#{operation}({left}, {right})"

    def add_data(self, name: str, size: int, alignment: int, text: str) -> str:
        if size <= 0 or alignment <= 0 or alignment & (alignment - 1):
            raise ValueError("invalid data layout")
        self.data.append(f'#data {name}({size}, {alignment}, "{text}");')
        return name

    def procedure(self, name: str, result_type: str, body: list[str]) -> str:
        rendered = "\n".join(f"  {line}" for line in body)
        self.procedures.append(
            f"#proc {name}() -> {result_type} {{\n{rendered}\n}}"
        )
        return name

    def finish(self) -> str:
        chunks = self.data + self.procedures
        return "\n\n".join(chunks) + "\n"


def command(*args: Path | str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(arg) for arg in args], cwd=ROOT, text=True, capture_output=True
    )


def canonical(path: Path, entry: str = "main") -> str:
    result = command(PRINT, path, entry)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())
    return result.stdout.replace("\r\n", "\n")


def execute(path: Path, entry: str = "main") -> int:
    result = command(RUN, "run", path, entry)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())
    return int(result.stdout.strip())


def main() -> int:
    if not PRINT.is_file() or not RUN.is_file():
        raise SystemExit("LAINIR API behavior check requires a built seed")

    builder = RecordingBuilder()
    i32 = builder.bits_type(32)
    builder.procedure(
        "main",
        i32,
        [f"#return {builder.add(builder.integer_literal(40), builder.integer_literal(2))}"],
    )
    expected = """#proc main() -> #bits<32> {
  #return #add(40, 2)
}
"""

    with tempfile.TemporaryDirectory(prefix="lainir-api-contract-") as directory:
        root = Path(directory)
        recorded = root / "recorded.l1"
        reference = root / "reference.l1"
        recorded.write_text(builder.finish(), encoding="utf-8", newline="\n")
        reference.write_text(expected, encoding="utf-8", newline="\n")
        if canonical(recorded) != canonical(reference):
            raise SystemExit("recording provider differs from seed canonical artifact")
        if execute(recorded) != 42:
            raise SystemExit("recording provider artifact returned the wrong value")

        integer_cases = (
            ("sub", "#bits<32>", 44, 2, 42),
            ("mul", "#bits<32>", 21, 2, 42),
            ("sdiv", "#bits<32>", 84, 2, 42),
            ("udiv", "#bits<32>", 84, 2, 42),
            ("eq", "#bits<1>", 42, 42, 1),
            ("ne", "#bits<1>", 42, 41, 1),
            ("slt", "#bits<1>", 1, 2, 1),
            ("sle", "#bits<1>", 1, 1, 1),
            ("sgt", "#bits<1>", 2, 1, 1),
            ("sge", "#bits<1>", 1, 1, 1),
            ("ult", "#bits<1>", 1, 2, 1),
            ("ule", "#bits<1>", 2, 2, 1),
            ("ugt", "#bits<1>", 2, 1, 1),
            ("uge", "#bits<1>", 2, 2, 1),
        )
        for operation, result_type, left, right, expected_value in integer_cases:
            case = root / f"integer_{operation}.l1"
            expression = RecordingBuilder.binary(
                operation, str(left), str(right)
            )
            case.write_text(
                f"#proc main() -> {result_type} {{ #return {expression} }}\n",
                encoding="utf-8",
                newline="\n",
            )
            canonical(case)
            if execute(case) != expected_value:
                raise SystemExit(f"{operation} behavior differs from contract")

        memory = root / "memory.l1"
        memory.write_text(
            """#proc main() -> #bits<32> {
  #let %memory: #addr = #alloca(4)
  #let %slot: #addr = #lea(base=%memory, idx=0, scale=1, offset=0)
  #let %answer: #bits<32> = 42
  #store[#bits<32>] %answer, %slot
  #return #load[#bits<32>](%slot)
}
""",
            encoding="utf-8",
            newline="\n",
        )
        canonical(memory)
        if execute(memory) != 42:
            raise SystemExit("memory capability contract returned the wrong value")

        data_builder = RecordingBuilder()
        data_builder.add_data("answer", 1, 1, "*")
        data_builder.procedure(
            "main",
            data_builder.bits_type(32),
            ["#return #zext[#bits<32>](#load[#bits<8>](#data_addr(answer)))"],
        )
        data = root / "data.l1"
        data.write_text(data_builder.finish(), encoding="utf-8", newline="\n")
        canonical(data)
        if execute(data) != 42:
            raise SystemExit("data capability contract returned the wrong value")

        binary_data = root / "binary_data.l1"
        binary_data.write_text(
            """#data bytes(4, 4, "A\\x00B");
#proc main() -> #bits<32> {
  #return #zext[#bits<32>](#load[#bits<8>](#lea(base=#data_addr(bytes), idx=0, scale=1, offset=1)))
}
""",
            encoding="utf-8",
            newline="\n",
        )
        binary_canonical = canonical(binary_data)
        if '#data bytes(4, 4, "A\\x00B");' not in binary_canonical:
            raise SystemExit("binary #data did not survive canonical round trip")
        if execute(binary_data) != 0:
            raise SystemExit("binary #data embedded zero byte was not preserved")

        eval_source = root / "eval.l1"
        eval_source.write_text(
            """#proc main() -> #bits<32> {
  #let %value: #bits<32> = #eval { #return 42 }
  #return %value
}
""",
            encoding="utf-8",
            newline="\n",
        )
        canonical(eval_source)
        if execute(eval_source) != 42:
            raise SystemExit("eval capability contract returned the wrong value")

        for spelling in ("div", "lt", "le", "gt", "ge"):
            invalid = root / f"invalid_{spelling}.l1"
            invalid.write_text(
                f"#proc main() -> #bits<32> "
                f"{{ #return #{spelling}(4, 2) }}\n",
                encoding="utf-8",
                newline="\n",
            )
            rejected = command(PRINT, invalid, "main")
            if rejected.returncode == 0:
                raise SystemExit(f"legacy #{spelling} operation was accepted")

        for spelling in ("i32", "i64", "addr"):
            invalid_type = root / f"invalid_type_{spelling}.l1"
            invalid_type.write_text(
                f"#proc main() -> {spelling} {{ #return 0 }}\n",
                encoding="utf-8",
                newline="\n",
            )
            rejected = command(PRINT, invalid_type, "main")
            if rejected.returncode == 0:
                raise SystemExit(f"legacy {spelling} type was accepted")

    print("PASS seed and recording provider LAINIR API behavior contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
