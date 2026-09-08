#!/usr/bin/env python3
"""Verify EvalResult VSpace identity and generation metadata."""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-seed.exe" if os.name == "nt" else "lainir-seed"
)
EVAL = ROOT / "src" / "lainir" / "lain" / "eval_result.l1"

TEST = r'''
#proc meta_value_kind(#addr %value) -> #bits<32> { #return 4 }

#proc main() -> #bits<32> {
  #let %owner: #addr = #call bootstrap.allocate-pages(8)
  #let %vspace: #addr = #call bootstrap.allocate-pages(8)
  #let %result: #addr = #call eval_result_new_owned_vspace(
    0, 42, %owner, %vspace, 7
  )
  #if #ne(#ptr2int(#call eval_result_vspace(%result)), #ptr2int(%vspace)) {
    #return 101
  }
  #if #ne(#call eval_result_generation(%result), 7) { #return 102 }
  #if #eq(#call eval_result_matches_vspace(%result, %vspace, 7), 0) {
    #return 108
  }
  #if #eq(#call eval_result_matches_vspace(%result, %vspace, 8), 1) {
    #return 109
  }
  #let %copy: #addr = #call eval_result_clone(%result)
  #if #ne(#ptr2int(#call eval_result_vspace(%copy)), #ptr2int(%vspace)) {
    #return 103
  }
  #if #ne(#call eval_result_generation(%copy), 7) { #return 104 }
  #let %object: #addr = #call bootstrap.allocate-pages(8)
  #let %object_result: #addr = #call eval_result_object_owned_vspace(
    0, %object, %owner, %vspace, 9
  )
  #if #eq(#call eval_result_is_object(%object_result), 0) { #return 105 }
  #if #ne(#ptr2int(#call eval_result_vspace(%object_result)), #ptr2int(%vspace)) {
    #return 106
  }
  #if #ne(#call eval_result_generation(%object_result), 9) { #return 107 }
  #return 0
}
'''


def main() -> int:
    eval_source = EVAL.read_text(encoding="utf-8")
    eval_source = eval_source.replace(
        "#extern #proc meta_value_kind(#addr %value) -> #bits<32>;\n",
        "",
        1,
    )
    with tempfile.TemporaryDirectory(prefix="lain-eval-vspace-") as directory:
        source = Path(directory) / "eval_vspace_test.l1"
        source.write_text(eval_source + "\n" + TEST, encoding="utf-8", newline="\n")
        result = subprocess.run(
            [os.fspath(SEED), "run", os.fspath(source), "main"],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
    if result.returncode != 0 or result.stdout.strip() != "0":
        raise SystemExit(
            "EvalResult VSpace check failed: "
            + (result.stderr.strip() or result.stdout.strip())
        )
    print("PASS EvalResult VSpace identity/generation contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
