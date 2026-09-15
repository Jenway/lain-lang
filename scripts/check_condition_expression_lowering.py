#!/usr/bin/env python3
"""Verify and execute ordinary conditions through shared expression lowering."""
import sys
import check_local_assignment_types as runner
CASES = {
    "mixed_arithmetic_comparison": """let main = std::func() -> i64 {
      let index: usize = 1; let limit: usize = 3; let byte: i32 = 47;
      if byte == 47 && index + 1 < limit && byte == 47 { return 42; }
      return 0;
    };""",
    "left_associativity": """let main = std::func() -> i64 {
      let value: i64 = 62;
      if value - 10 - 10 == 42 && value == 62 { return 42; } return 0;
    };""",
    "multiplication_precedence": """let main = std::func() -> i64 {
      let value: i64 = 14;
      if value + 15 * 2 - 14 / 7 == 42 { return 42; } return 0;
    };""",
    "logical_precedence": """let main = std::func() -> i64 {
      let value: i64 = 1;
      if value == 1 || value == 0 && value == 0 { return 42; } return 0;
    };""",
    "both_true_or": """let main = std::func() -> i64 {
      let value: i64 = 1;
      if value == 1 || value == 1 { return 42; } return 0;
    };""",
    "right_arithmetic": """let main = std::func() -> i64 {
      let value: i64 = 40;
      if 42 == value + 2 { return 42; } return 0;
    };""",
}
CASES.update({
    "unary_not": """let main = std::func() -> i64 {
      if !(1 == 0) && !(1 == 2) { return 42; } return 0;
    };""",
    "boolean_value": """let main = std::func() -> i64 {
      let flag: bool = 42 == 42 || 42 == 42;
      if flag { return 42; } return 0;
    };""",
    "nested_groups": """let main = std::func() -> i64 {
      if (14 + 15 * 2 - 14 / 7 == 42) && (1 == 0 || 1 == 1) { return 42; }
      return 0;
    };""",
    "while_logical_precedence": """let main = std::func() -> i64 {
      let count: i64 = 0;
      while count < 4 || count < 1 && count < 2 { count = count + 1; }
      if count == 4 { return 42; } return 0;
    };""",
    "while_arithmetic": """let main = std::func() -> i64 {
      let count: i64 = 0;
      while count + 1 < 3 && count < 2 { count = count + 1; }
      if count == 2 { return 42; } return 0;
    };""",
    "repeated_macro_expression": """let inc = macro(x) { x + 1 };
    let main = std::func() -> i64 { return inc(20) + inc(20); };""",
})
if __name__ == "__main__":
    try: raise SystemExit(runner.main(CASES, "condition expression"))
    except (RuntimeError, runner.subprocess.SubprocessError) as error:
        print(error, file=sys.stderr); raise SystemExit(1)
