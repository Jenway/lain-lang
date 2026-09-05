#!/usr/bin/env python3
"""Exercise CompileContextV1 and MetaPassResultV1 with the seed runtime."""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEED = ROOT / "seed" / "zig-out" / "bin" / (
    "lainir-seed.exe" if os.name == "nt" else "lainir-seed"
)
CONTEXT = ROOT / "src" / "lainir" / "lain" / "compiler_context.l1"
EVAL = ROOT / "src" / "lainir" / "lain" / "eval_result.l1"

TEST = r'''
#proc meta_value_nil() -> addr {
  #let %value: addr = #call bootstrap.allocate-pages(8)
  #store 0, #lea(base=%value, idx=0, scale=0, offset=0)
  #return %value
}

#proc meta_value_kind(addr %value) -> i32 {
  #return #load[i32](#lea(base=%value, idx=0, scale=0, offset=0))
}

#proc main() -> #bits<32> {
  #let %owner_a: addr = #int2ptr(1)
  #let %owner_b: addr = #int2ptr(2)
  #let %context: addr = #call lain_compile_context_v1_new(
    %owner_a, 2, 0, 2, 1
  )
  #if #eq(#call lain_compile_context_v1_consume_step(%context), 0) {
    #return 101
  }
  #if #eq(#call lain_compile_context_v1_consume_step(%context), 0) {
    #return 102
  }
  #if #eq(#call lain_compile_context_v1_consume_step(%context), 1) {
    #return 103
  }
  #if #eq(#call lain_compile_context_v1_has_capability(%context, 1), 0) {
    #return 104
  }
  #if #eq(#call lain_compile_context_v1_has_capability(%context, 2), 1) {
    #return 105
  }
  #if #eq(#call lain_compile_context_v1_enter_recursion(%context), 0) {
    #return 111
  }
  #if #eq(#call lain_compile_context_v1_enter_recursion(%context), 0) {
    #return 112
  }
  #if #eq(#call lain_compile_context_v1_enter_recursion(%context), 1) {
    #return 113
  }
  #call lain_compile_context_v1_leave_recursion(%context)
  #if #eq(#call lain_compile_context_v1_enter_recursion(%context), 0) {
    #return 114
  }
  #call lain_compile_context_v1_leave_recursion(%context)
  #call lain_compile_context_v1_leave_recursion(%context)
  #let %unlimited: addr = #call lain_compile_context_v1_new(
    %owner_a, 0, 0, 0, 0
  )
  #if #eq(#call lain_compile_context_v1_consume_step(%unlimited), 0) {
    #return 106
  }
  #let %result: addr = #call lain_meta_pass_result_v1_new(
    0, 0, %owner_a, %owner_a, %owner_b, %owner_a
  )
  #if #eq(#call lain_meta_pass_result_v1_owner_matches(%result, %context), 0) {
    #return 107
  }
  #let %other_context: addr = #call lain_compile_context_v1_new(
    %owner_b, 0, 0, 0, 0
  )
  #if #eq(#call lain_meta_pass_result_v1_transfer_owner(
    %result, %owner_a, %other_context
  ), 0) {
    #return 108
  }
  #if #eq(#call lain_meta_pass_result_v1_owner_matches(%result, %other_context), 0) {
    #return 109
  }
  #if #eq(#call lain_meta_pass_result_v1_transfer_owner(
    %result, %owner_a, %context
  ), 1) {
    #return 110
  }
  #let %eval_result: addr = #call eval_result_new_owned(
    0, 42, %owner_a
  )
  #if #eq(#call eval_result_owner_matches_owner(
    %eval_result, %owner_a
  ), 0) {
    #return 120
  }
  #if #eq(#call eval_result_transfer_owner(
    %eval_result, %owner_a, %owner_b
  ), 0) {
    #return 121
  }
  #if #eq(#call eval_result_owner_matches_owner(
    %eval_result, %owner_b
  ), 0) {
    #return 122
  }
  #if #eq(#call eval_result_transfer_owner(
    %eval_result, %owner_a, %owner_a
  ), 1) {
    #return 123
  }
  #let %nil_meta: addr = #call meta_value_nil()
  #let %nil_object_result: addr = #call eval_result_object(
    0, %nil_meta
  )
  #if #ne(#call eval_status(%nil_object_result), 5108) {
    #return 124
  }
  #return 0
}
'''


def main() -> int:
    if not SEED.is_file() or not CONTEXT.is_file() or not EVAL.is_file():
        raise SystemExit("compile context check: seed, context, or eval source missing")
    with tempfile.TemporaryDirectory(prefix="lain-context-contract-") as directory:
        source = Path(directory) / "context_test.l1"
        eval_source = EVAL.read_text(encoding="utf-8").replace(
            "#extern #proc bootstrap.allocate-pages(i64 %size) -> addr;\n",
            "",
            1,
        )
        eval_source = eval_source.replace(
            "#extern #proc meta_value_kind(addr %value) -> i32;\n",
            "",
            1,
        )
        eval_source = eval_source.replace(
            "#extern #proc bootstrap.release-pages(addr %memory) -> #unit;\n",
            "",
            1,
        )
        source.write_text(
            CONTEXT.read_text(encoding="utf-8")
            + "\n"
            + eval_source
            + "\n"
            + TEST,
            encoding="utf-8",
            newline="\n",
        )
        result = subprocess.run(
            [os.fspath(SEED), "run", os.fspath(source), "main"],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if result.returncode != 0:
            raise SystemExit(
                "compile context check failed: "
                + (result.stderr.strip() or result.stdout.strip())
            )
        if result.stdout.strip() != "0":
            raise SystemExit(
                "compile context check returned " + result.stdout.strip()
            )
    print("PASS CompileContextV1 and MetaPassResultV1 contracts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
