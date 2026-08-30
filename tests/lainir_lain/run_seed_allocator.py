"""Check the capability boundary of ``lainir-seed run``.

The run command owns only the allocator required by generated self-hosting
artifacts.  Source/artifact I/O capabilities must remain unavailable.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SEED = ROOT / "seed" / "zig-out" / "bin" / f"lainir-seed{'.exe' if os.name == 'nt' else ''}"


def run(program: Path, *options: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(SEED), "run", *options, str(program), "main"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="lainir-seed-allocator-") as directory:
        tmp = Path(directory)
        positive = tmp / "positive.l1"
        positive.write_text(
            """#extern #proc bootstrap.allocate-pages(i64 %size) -> addr;
#proc main() -> i32 {
  #let %memory: addr = #call bootstrap.allocate-pages(8)
  #store[#bits<32>] 42, #lea(base=%memory, idx=0, scale=0, offset=0)
  #return #load[#bits<32>](#lea(base=%memory, idx=0, scale=0, offset=0))
}
""",
            encoding="utf-8",
        )
        result = run(positive)
        if result.returncode or result.stdout.strip() != "42":
            raise RuntimeError(
                f"allocator positive case failed: {result.stderr or result.stdout}"
            )

        release = tmp / "release.l1"
        release.write_text(
            """#extern #proc bootstrap.allocate-pages(i64 %size) -> addr;
#extern #proc bootstrap.release-pages(addr %memory) -> #unit;
#proc main() -> i32 {
  #let %memory: addr = #call bootstrap.allocate-pages(8)
  #call bootstrap.release-pages(%memory)
  #return 0
}
""",
            encoding="utf-8",
        )
        result = run(release)
        if result.returncode or result.stdout.strip() != "0":
            raise RuntimeError(
                f"allocator release case failed: {result.stderr or result.stdout}"
            )

        double_release = tmp / "double_release.l1"
        double_release.write_text(
            """#extern #proc bootstrap.allocate-pages(i64 %size) -> addr;
#extern #proc bootstrap.release-pages(addr %memory) -> #unit;
#proc main() -> i32 {
  #let %memory: addr = #call bootstrap.allocate-pages(8)
  #call bootstrap.release-pages(%memory)
  #call bootstrap.release-pages(%memory)
  #return 0
}
""",
            encoding="utf-8",
        )
        result = run(double_release)
        if result.returncode == 0 or "unknown address" not in result.stderr:
            raise RuntimeError(
                "double release was not rejected: "
                f"{result.stdout} {result.stderr}"
            )

        missing = tmp / "missing.l1"
        missing.write_text(
            """#extern #proc bootstrap.source-count() -> i64;
#proc main() -> i32 {
  #return #call bootstrap.source-count()
}
""",
            encoding="utf-8",
        )
        result = run(missing)
        if result.returncode == 0 or "extern capability not found" not in result.stderr:
            raise RuntimeError(
                "run unexpectedly exposed a non-allocator capability: "
                f"{result.stdout} {result.stderr}"
            )

        bad_args = tmp / "bad_args.l1"
        bad_args.write_text(
            """#extern #proc bootstrap.allocate-pages(i64 %size) -> addr;
#proc main() -> i32 {
  #return #call bootstrap.allocate-pages()
}
""",
            encoding="utf-8",
        )
        result = run(bad_args)
        if result.returncode == 0 or "verify error" not in result.stderr:
            raise RuntimeError("allocator argument mismatch was not rejected")

        failed = tmp / "failed.l1"
        failed.write_text(
            """#extern #proc bootstrap.allocate-pages(i64 %size) -> addr;
#proc main() -> i32 {
  #let %memory: addr = #call bootstrap.allocate-pages(8)
  #return 0
}
""",
            encoding="utf-8",
        )
        result = run(failed, "--max-alloc-bytes", "1")
        if result.returncode == 0 or "allocation budget" not in result.stderr:
            raise RuntimeError(
                "allocator failure was not reported: "
                f"{result.stdout} {result.stderr}"
            )

    print("PASS seed run allocator capability boundary")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
