import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
WORK = os.path.join(BUILD, "verify_all")
UNITS = os.path.join(BUILD, "units")
EXAMPLES = os.path.join(ROOT, "examples")
META_ENTRY = os.path.join(ROOT, "src", "meta.lain")


def exe(name):
    path = os.path.join(BUILD, name)
    if os.name == "nt" and os.path.exists(path + ".exe"):
        return path + ".exe"
    return path


LAIN0 = os.path.join(BUILD, "lain0", "lain0.exe") if os.name == "nt" else os.path.join(BUILD, "lain0", "lain0")
LAIN_META = exe("lain-meta")
LAINIR_VERIFY = exe("lainir-verify")
LAINIR_RUN = exe("lainir-run")


# 真值用例：样例 -> [(函数名, 实参, 手算期望值)]。
# 期望值来自对 examples/<样例>.lain 源码的人工核对，负数按有符号读数比较。
RUN_CASES = {
    "arith": [("total", [], 3)],
    "bits": [
        ("band", [14, 6], 6),
        ("bor", [14, 1], 15),
        ("bshl", [3, 2], 12),
        ("bshr", [4294967288, 2], -2),
        ("uband", [14, 6], 6),
        ("ubor", [12, 3], 15),
        ("ubshl", [3, 2], 12),
        ("ubshr", [240, 4], 15),
    ],
    "choose": [
        ("smaller", [2, 5], 3),
        ("smaller", [9, 4], 5),
        ("clamp_zero", [4294967293], 3),
        ("clamp_zero", [4], 5),
    ],
    "convert": [
        ("widen", [4294967294], -2),
        ("narrow", [4294967301], 5),
        ("same", [4294967289], -7),
        ("bare", [4294967294], -2),
    ],
    "field_expr": [("shift", [5, 7], 6), ("scale", [3], 6)],
    "flow": [
        ("pick", [3, 7], 3),
        ("pick", [7, 3], 3),
        ("inc", [41], 42),
        ("bump", [41], 42),
        ("copy_of", [9], 9),
        ("lit", [], 3),
    ],
    "hex": [("mask", [4660], 4608), ("small", [], 42)],
    "if_default": [
        ("keep", [5, 1], 6),
        ("keep", [5, 0], 1),
        ("keep_or_zero", [5, 0], 0),
        ("total", [1, 2, 1], 5),
        ("total", [1, 2, 0], 5),
    ],
    "jumps": [("first", [5], 5), ("early", [5], 6), ("step", [4], 14)],
    "local_type": [("below", [], 1)],
    "locals": [("mk", [10], 10), ("mk2", [7], 7), ("two", [4], 4)],
    "loop": [("countdown", [5], 15), ("upto", [5], 10)],
    "loop_if": [("clip", [6], 13), ("branchy", [4], 36), ("nested", [5], 3)],
    "loop_value": [("first_over", [10, 3], 3), ("third", [10], 3)],
    "modules_lib": [("answer", [], 42), ("twice", [], 42), ("bump", [5], 6)],
    "modules_main": [("total", [], 127), ("nested", [], 43), ("later", [], 44)],
    "modules_twin": [("total", [], 3)],
    "modules_twin_a": [("dup", [7], 7)],
    "modules_twin_b": [("dup", [8], 8)],
    "neg": [("offset", [10], 7), ("below", [3], 4), ("bare", [], -2)],
    "ops": [
        ("mix", [7, 3], 7),
        ("cmp_signed", [5, 3], 1),
        ("cmp_unsigned", [5, 3], 1),
        ("cmp_equal", [5, 5], 0),
        ("mix_unsigned", [7, 3], 2),
        ("cmp_le", [3, 3], 1),
    ],
    "table": [
        ("second", [], 2),
        ("third", [], 30),
        ("count", [], 3),
        ("first_mark", [], 7),
        ("bump", [1, 55], 55),
    ],
    "u8math": [
        ("eq8", [3, 3], 1),
        ("ne8", [3, 3], 0),
        ("lt8", [2, 3], 1),
        ("le8", [3, 3], 1),
        ("gt8", [4, 3], 1),
        ("ge8", [3, 4], 0),
        ("add8", [200, 100], 44),
        ("sub8", [1, 3], 254),
        ("and8", [15, 51], 3),
        ("shr8", [240, 4], 15),
    ],
    "value_if": [
        ("plus", [5, 1], 6),
        ("plus", [5, 0], -4),
        ("pick", [9, 4, 1], 4),
        ("pick", [4, 9, 0], 4),
        ("scale", [3, 1], 9),
        ("sum", [5, 1], 9),
        ("sum", [5, 0], 7),
    ],
    "widths": [
        ("n64", [6, 3], 20),
        ("c64", [6, 3], 1),
        ("ln", [2, 3], 5),
        ("lc", [2, 2], 0),
        ("s64", [3, 5], -2),
        ("bl", [1, 1], 1),
    ],
    "variant_local": [("keep", [9], 9), ("compute", [9], 9)],
    "bytes": [("length", [], 2), ("inline_first", [], 104)],
    "cast": [("local_store", [], 7), ("local_byte", [], 42),
             ("local_round", [], 11)],
    "field_value": [("local_total", [], 7)],
    "loop_local": [("local_sum", [4], 10)],
    "memory": [("local_peek", [], 65)],
    "pointer": [("local_poke", [], 9)],
    "projection": [("local_both", [9], 18)],
    "refs": [("via_ref", [], 42), ("local_set", [], 5)],
    "strinline": [("first_byte", [], 104)],
    "symbol_addr": [("first_byte", [], 1)],
    "types": [("copy", [], 7), ("chain", [], 5), ("project", [9], 18)],
}

# 没有真值用例的样例，以及原因（审计用：这些样例目前只验证降级合法性）。
NO_RUN = {
    "extern": "入口依赖宿主能力表（驱动不注册）",
}


# ---------------------------------------------------------------------------
# v2 进度榜（非阻断）：src_v2 编译器对 Wave 3A 样例的真值对比。
# 失败只打印进度榜与明细，不改变 v1 的退出码。

V2_ENTRY = os.path.join(ROOT, "src_v2", "entry.lain")
V2_UNITS = os.path.join(BUILD, "v2units")
V2_OUT = os.path.join(BUILD, "v2")
LAIN_META_V2 = exe("lain-meta-v2")

# Wave 3A 覆盖的样例。
V2_STEMS = ["arith", "bits", "choose", "convert", "flow", "hex",
            "if_default", "jumps", "loop", "loop_if", "loop_value",
            "neg", "ops", "u8math", "value_if", "widths"]


def v2_abi_check():
    """枚举常量与头文件同步校验。回失败文本或 None。"""
    script = os.path.join(ROOT, "scripts", "gen_v2_abi.py")
    result = run([sys.executable, script, "--check"])
    if result.returncode != 0:
        return (result.stdout + result.stderr).strip() or "gen_v2_abi --check 失败"
    return None


def v2_split():
    """编译 src_v2 自举编译器。回单元路径列表或 None。"""
    if os.path.isdir(V2_UNITS):
        shutil.rmtree(V2_UNITS)
    result = run([LAIN0, "--entry", V2_ENTRY, "--split", V2_UNITS])
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        return None
    return [os.path.join(V2_UNITS, n) for n in sorted(os.listdir(V2_UNITS))
            if n.endswith(".l1")]


def v2_compile_sample(stem, units):
    """用 v2 编译器编译一个样例。回 (输出文本, 失败文本或 None)。"""
    out_path = os.path.join(V2_OUT, stem + ".l1")
    command = [LAIN_META_V2] + units + [os.path.join("examples", stem + ".lain"),
                                         "--out", out_path]
    result = run(command)
    text = result.stdout + result.stderr
    if result.returncode != 0:
        body = text.strip().splitlines()
        return text, body[-1] if body else "失败"
    if not os.path.exists(out_path):
        return text, "v2 没写产物"
    return text, None


def v2_check_runs(stem, out_path):
    """v2 产物真值对比。回 (通过数, 总数, 明细列表)。"""
    cases = RUN_CASES.get(stem) or []
    passed = 0
    details = []
    for func, args, expected in cases:
        result, error, _ = execute([out_path], func, args)
        if error:
            details.append("%s %s%r: %s" % (stem, func, tuple(args), error))
        elif measure(result, expected) != expected:
            details.append("%s %s%r 期望 %d，实得 %d"
                           % (stem, func, tuple(args), expected,
                              measure(result, expected)))
        else:
            passed += 1
    return passed, len(cases), details


def v2_leaderboard(units):
    """跑 v2 进度榜，打印表格。回失败明细列表。"""
    os.makedirs(V2_OUT, exist_ok=True)
    rows = []
    problems = []
    total_pass = 0
    total_cases = 0
    for stem in V2_STEMS:
        text, error = v2_compile_sample(stem, units)
        row = {"name": stem, "compile": "ok" if not error else "FAIL",
               "verify": "-", "run": "-", "steps": 0}
        if error:
            problems.append("%s 编译: %s" % (stem, error))
            rows.append(row)
            continue
        hit = re.search(r"^steps=(\d+)$", text, re.MULTILINE)
        if hit:
            row["steps"] = int(hit.group(1))
        out_path = os.path.join(V2_OUT, stem + ".l1")
        result = run([LAINIR_VERIFY, out_path])
        if result.returncode != 0:
            row["verify"] = "FAIL"
            body = (result.stdout + result.stderr).strip().splitlines()
            problems.append("%s 校验: %s" % (stem, body[0] if body else "失败"))
            rows.append(row)
            continue
        row["verify"] = "ok"
        passed, count, details = v2_check_runs(stem, out_path)
        total_pass += passed
        total_cases += count
        row["run"] = "%d/%d" % (passed, count) if count else "-"
        rows.append(row)
        for line in details:
            problems.append(line)

    header = ("样例", "编译", "校验", "真值", "steps")
    print()
    print("[v2 进度榜]  src_v2 编译器（Wave 3A 覆盖范围）")
    line = "  ".join(h.ljust(w) for h, w in zip(header, (22, 6, 6, 8, 10)))
    print(line)
    print("-" * len(line))
    for row in rows:
        cells = (row["name"], row["compile"], row["verify"], row["run"],
                 str(row["steps"]))
        print("  ".join(c.ljust(w) for c, w in zip(cells, (22, 6, 6, 8, 10))))
    print("v2 真值用例 %d/%d" % (total_pass, total_cases))
    return problems



# IR 层用例：直接写 LAINIR 文本，测间接 #call 的四条路径。
# value = 入口返回的整数；trap = 引擎陷阱码（此时退出码必须非 0）。
IR_TESTS = os.path.join(ROOT, "seed", "tests")
IR_CASES = [
    {"file": "ir_indirect_call.l1", "entry": "run_zero", "value": 42},
    {"file": "ir_indirect_call.l1", "entry": "run_add", "value": 12},
    {"file": "ir_indirect_call.l1", "entry": "run_tail",
     "args": [41], "value": 42},
    {"file": "ir_indirect_bad_target.l1", "entry": "bad", "trap": 1105},
    {"file": "ir_indirect_bad_arity.l1", "entry": "bad", "trap": 1102},
    {"file": "ir_indirect_bad_type.l1", "entry": "bad", "trap": 1103},
]


def ir_roundtrip(path):
    """canonical 文本往返定点。回失败文本或 None。"""
    first = run([LAINIR_VERIFY, "--print", path])
    if first.returncode != 0:
        body = (first.stdout + first.stderr).strip().splitlines()
        return body[0] if body else "打印失败"
    second_path = path + ".roundtrip.l1"
    with open(second_path, "w", encoding="utf-8") as handle:
        handle.write(first.stdout)
    try:
        second = run([LAINIR_VERIFY, "--print", second_path])
        if second.returncode != 0:
            body = (second.stdout + second.stderr).strip().splitlines()
            return body[0] if body else "二次打印失败"
        if second.stdout != first.stdout:
            return "canonical 文本往返不定点"
    finally:
        if os.path.exists(second_path):
            os.remove(second_path)
    return None


def check_ir_cases():
    """跑 IR 层用例。回 (通过数, 总数, 明细列表)。"""
    passed = 0
    details = []
    total = 0
    seen = set()
    for case in IR_CASES:
        total += 1
        path = os.path.join(IR_TESTS, case["file"])
        if not os.path.exists(path):
            details.append("缺用例文件: " + path)
            continue
        if case["file"] not in seen:
            seen.add(case["file"])
            error = ir_roundtrip(path)
            if error:
                details.append("%s 往返: %s" % (case["file"], error))
                continue
        result = run([LAINIR_VERIFY, path])
        if result.returncode != 0:
            body = (result.stdout + result.stderr).strip().splitlines()
            details.append("%s 校验: %s" % (case["file"],
                                           body[0] if body else "失败"))
            continue
        command = [LAINIR_RUN, "--entry", case["entry"]]
        for value in case.get("args", []):
            command += ["--arg", str(value)]
        command.append(path)
        result = run(command)
        text = result.stdout + result.stderr
        if "trap" in case:
            hit = re.search(r"trap kind=\d+ status=(\d+)", text)
            if not hit:
                details.append("%s %s: 没有陷阱输出" %
                               (case["file"], case["entry"]))
            elif int(hit.group(1)) != case["trap"]:
                details.append("%s %s: 期望陷阱 %d，实得 %s" %
                               (case["file"], case["entry"],
                                case["trap"], hit.group(1)))
            elif result.returncode == 0:
                details.append("%s %s: 陷阱后退出码应为非 0" %
                               (case["file"], case["entry"]))
            else:
                passed += 1
            continue
        if result.returncode != 0:
            body = text.strip().splitlines()
            details.append("%s %s: %s" % (case["file"], case["entry"],
                                           body[-1] if body else "无输出"))
            continue
        hit = re.search(r"result value=(\d+) signed=(-?\d+) width=(\d+)", text)
        if not hit:
            details.append("%s %s: 没有整数结果" %
                           (case["file"], case["entry"]))
            continue
        expect = case["value"]
        actual = int(hit.group(1)) if expect >= 0 else int(hit.group(2))
        if actual != expect:
            details.append("%s %s: 期望 %d，实得 %d" %
                           (case["file"], case["entry"], expect, actual))
        else:
            passed += 1
    return passed, total, details


def run(cmd, timeout=1800):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)


def fail(message):
    print("失败: " + message)
    sys.exit(1)


def build():
    cache = os.path.join(BUILD, "CMakeCache.txt")
    if not os.path.exists(cache):
        result = run(["cmake", "-S", "seed", "-B", "build", "-G", "Ninja"])
        if result.returncode != 0:
            print(result.stdout)
            print(result.stderr)
            fail("cmake 配置")
    result = run(["cmake", "--build", "build"])
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        fail("cmake 构建")
    for tool in (LAIN0, LAIN_META, LAINIR_VERIFY, LAINIR_RUN):
        if not os.path.exists(tool):
            fail("没有这个产物: " + tool)


def split_units():
    result = run([LAIN0, "--entry", META_ENTRY, "--split", UNITS])
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        fail("lain0 拆分 src/meta.lain")
    units = sorted(n for n in os.listdir(UNITS) if n.endswith(".l1"))
    if not units:
        fail("拆分目录里没有单元: " + UNITS)
    return len(units)


PATTERNS = {
    "sources": re.compile(r"sources count=(\d+)"),
    "steps": re.compile(r"run slice=\S+ rounds=\S+ steps=(\d+) host_status=(\d+)"),
    "visits": re.compile(r"ast handler_calls=(\d+) visits=(\d+)"),
    "bytes": re.compile(r"output bytes=(\d+)"),
    "trap": re.compile(r"trap (\S+)"),
}


def field(text, name):
    hit = PATTERNS[name].search(text)
    return hit if hit else None


def proc_names(paths):
    names = set()
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if line.startswith("#proc "):
                    names.add(line[len("#proc "):].split("(")[0].strip())
    return names


def execute(paths, name, args):
    """跑一次入口。回 (结果, 失败文本或 None, 原始输出)。"""
    command = [LAINIR_RUN, "--entry", name]
    for value in args:
        command += ["--arg", str(value)]
    command += list(paths)
    result = run(command)
    text = result.stdout + result.stderr
    if result.returncode != 0:
        body = text.strip().splitlines()
        return None, "退出码 %d：%s" % (result.returncode, body[-1] if body else "无输出"), text
    hit = re.search(r"result value=(\d+) signed=(-?\d+) width=(\d+)", text)
    if not hit:
        body = text.strip().splitlines()
        return None, "没有整数结果：" + (body[-1] if body else "无输出"), text
    return {"value": int(hit.group(1)), "signed": int(hit.group(2)),
            "width": int(hit.group(3))}, None, text


def measure(result, expected):
    """按期望值的符号选择读数：非负期望比无符号值，负期望比有符号值。"""
    return result["value"] if expected >= 0 else result["signed"]


def check_runs(stem, l0_path, meta_paths):
    """跑该样例的全部真值用例，回 (状态, 差分状态, 明细列表)。"""
    cases = RUN_CASES.get(stem)
    if not cases:
        return "-", "-", []
    if not os.path.exists(l0_path) or not meta_paths:
        return "-", "-", []
    l0_names = proc_names([l0_path])
    meta_names = proc_names(meta_paths)
    l0_state, meta_state, diff_state = "ok", "ok", "ok"
    details = []
    for func, args, expected in cases:
        l0_entry = "%s__%s" % (stem, func)
        meta_entry = "examples_%s__%s" % (stem, func)
        if l0_entry not in l0_names:
            l0_state = "FAIL"
            details.append("lain0 没有入口 %s" % l0_entry)
            continue
        if meta_entry not in meta_names:
            meta_state = "FAIL"
            details.append("Meta 没有入口 %s" % meta_entry)
            continue
        l0_result, l0_error, _ = execute([l0_path], l0_entry, args)
        meta_result, meta_error, _ = execute(meta_paths, meta_entry, args)
        if l0_error:
            l0_state = "FAIL"
            details.append("lain0 %s%r: %s" % (func, tuple(args), l0_error))
        elif measure(l0_result, expected) != expected:
            l0_state = "FAIL"
            details.append("lain0 %s%r 期望 %d，实得 %d"
                           % (func, tuple(args), expected, measure(l0_result, expected)))
        if meta_error:
            meta_state = "FAIL"
            details.append("Meta %s%r: %s" % (func, tuple(args), meta_error))
        elif measure(meta_result, expected) != expected:
            meta_state = "FAIL"
            details.append("Meta %s%r 期望 %d，实得 %d"
                           % (func, tuple(args), expected, measure(meta_result, expected)))
        if l0_result and meta_result:
            l0_shape = (l0_result["value"], l0_result["width"])
            meta_shape = (meta_result["value"], meta_result["width"])
            if l0_shape != meta_shape:
                diff_state = "FAIL"
                details.append("差分 %s%r: lain0=%r Meta=%r"
                               % (func, tuple(args), l0_shape, meta_shape))
    if l0_state == "ok":
        l0_state = "%d/%d" % (len(cases), len(cases))
    if meta_state == "ok":
        meta_state = "%d/%d" % (len(cases), len(cases))
    return l0_state, diff_state, details


def check_sample(name):
    """跑一个样例，回 (结果行, 失败原因列表)。"""
    source = os.path.join("examples", name)
    stem = name[: -len(".lain")]
    out_dir = os.path.join(WORK, stem)
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)

    row = {"name": stem, "sources": 0, "lain0": "ok", "round": "ok",
           "meta": "ok", "verify": "ok", "run0": "-", "diff": "-",
           "steps": 0, "visits": 0, "bytes": 0}
    problems = []

    l0_out = os.path.join(out_dir, "lain0.l1")
    result = run([LAIN0, source, "-o", l0_out, "--round-trip"])
    if result.returncode != 0:
        row["lain0"] = "FAIL"
        row["round"] = "-"
        body = (result.stdout + result.stderr).strip().splitlines()
        problems.append("lain0: " + (body[-1] if body else "失败"))
    elif not os.path.exists(l0_out):
        row["lain0"] = "FAIL"
        row["round"] = "-"
        problems.append("lain0 没写产物")

    unit_files = []
    first_out = os.path.join(out_dir, "meta0.l1")
    result = run([LAIN_META, "--root", ".", "--units-dir", UNITS,
                  "--lower", "0", "--out", first_out, source])
    text = result.stdout + result.stderr
    if result.returncode != 0:
        row["meta"] = "FAIL"
        problems.append("lain-meta: " + (text.strip().splitlines()[-1] if text.strip() else "失败"))
    sources_hit = field(text, "sources")
    steps_hit = field(text, "steps")
    visits_hit = field(text, "visits")
    bytes_hit = field(text, "bytes")
    trap_hit = field(text, "trap")
    if sources_hit:
        row["sources"] = int(sources_hit.group(1))
    if steps_hit:
        row["steps"] = int(steps_hit.group(1))
        if steps_hit.group(2) != "0":
            row["meta"] = "FAIL"
            problems.append("lain-meta 状态 " + steps_hit.group(2))
    if visits_hit:
        row["visits"] = int(visits_hit.group(2))
    if bytes_hit:
        row["bytes"] = int(bytes_hit.group(1))
    if trap_hit and trap_hit.group(1) != "none":
        row["meta"] = "FAIL"
        problems.append("引擎 trap: " + trap_hit.group(1))
    if not sources_hit:
        row["sources"] = 0
    if row["sources"] == 0:
        row["verify"] = "-"
        return row, problems

    if os.path.exists(first_out):
        unit_files.append(first_out)
    for index in range(1, row["sources"]):
        out_path = os.path.join(out_dir, "meta%d.l1" % index)
        result = run([LAIN_META, "--root", ".", "--units-dir", UNITS, "-q",
                      "--lower", str(index), "--out", out_path, source])
        if result.returncode != 0:
            row["meta"] = "FAIL"
            body = (result.stdout + result.stderr).strip().splitlines()
            problems.append("lain-meta 单元 %d: %s" % (index, body[-1] if body else "失败"))
            continue
        if os.path.exists(out_path):
            unit_files.append(out_path)

    if len(unit_files) != row["sources"]:
        row["verify"] = "FAIL"
        problems.append("产物单元数 %d，应得 %d" % (len(unit_files), row["sources"]))
        return row, problems

    ordered = [os.path.join(out_dir, "meta%d.l1" % i) for i in range(row["sources"])]
    result = run([LAINIR_VERIFY] + ordered)
    if result.returncode != 0:
        row["verify"] = "FAIL"
        body = (result.stdout + result.stderr).strip().splitlines()
        problems.append("lainir-verify: " + (body[0] if body else "失败"))

    run0, diff, details = check_runs(stem, l0_out, ordered)
    row["run0"] = run0
    row["diff"] = diff
    for line in details:
        problems.append(line)
    if "FAIL" in (run0, diff):
        row["verify"] = "FAIL" if row["verify"] == "ok" else row["verify"]
    return row, problems


def main():
    started = time.time()
    os.makedirs(WORK, exist_ok=True)
    print("构建中 …")
    build()
    units = split_units()
    print("lain0 拆分 src/meta.lain -> %d 份单元" % units)

    v2_units = None
    v2_problems = []
    abi_error = v2_abi_check()
    if abi_error:
        v2_problems.append("枚举同步: " + abi_error)
    else:
        v2_units = v2_split()
        if not v2_units:
            v2_problems.append("v2 自举: lain0 --split src_v2/entry.lain 失败")

    names = sorted(n for n in os.listdir(EXAMPLES) if n.endswith(".lain"))
    rows = []
    problems = []
    for name in names:
        row, bad = check_sample(name)
        rows.append(row)
        for line in bad:
            problems.append(row["name"] + ": " + line)
        print(".", end="", flush=True)
    print()

    header = ("样例", "单元", "lain0", "往返", "Meta", "校验", "真值",
              "差分", "steps", "ast visits", "output bytes")
    widths = (22, 6, 7, 6, 6, 6, 6, 6, 10, 11, 13)
    line = "  ".join(h.ljust(w) for h, w in zip(header, widths))
    print()
    print(line)
    print("-" * len(line))
    for row in rows:
        cells = (row["name"], str(row["sources"]), row["lain0"], row["round"],
                 row["meta"], row["verify"], row["run0"], row["diff"],
                 str(row["steps"]), str(row["visits"]), str(row["bytes"]))
        print("  ".join(c.ljust(w) for c, w in zip(cells, widths)))

    failed = [r["name"] for r in rows
              if "FAIL" in (r["lain0"], r["round"], r["meta"], r["verify"],
                             r["run0"], r["diff"])]
    cases = sum(len(v) for v in RUN_CASES.values())
    executed = [r for r in rows if r["run0"] != "-"]
    print()
    print("通过 %d/%d；真值用例 %d 条，覆盖 %d 个样例"
          % (len(rows) - len(failed), len(rows), cases, len(executed)))
    missing = [r["name"] for r in rows if r["run0"] == "-"]
    if missing:
        print()
        print("没有真值用例的样例（只验证降级合法性）:")
        for name in missing:
            print("  %-20s %s" % (name, NO_RUN.get(name, "未登记原因")))
    if problems:
        print()
        print("失败明细:")
        for item in problems:
            print("  " + item)
    if v2_units:
        v2_problems += v2_leaderboard(v2_units)
    elif v2_problems:
        print()
        print("[v2 进度榜] 跳过：")
        for item in v2_problems:
            print("  " + item)
    if v2_problems:
        print()
        print("v2 失败明细（不阻断 v1）:")
        for item in v2_problems:
            print("  " + item)
    print()
    print("[IR 层用例]  间接 #call（取值 / 尾调用 / 非法目标 / 签名不符）")
    ir_passed, ir_total, ir_details = check_ir_cases()
    print("IR 用例 %d/%d" % (ir_passed, ir_total))
    if ir_details:
        print("IR 失败明细:")
        for item in ir_details:
            print("  " + item)
    print("耗时 %.1f 秒" % (time.time() - started))
    return 0 if not failed and not ir_details else 1


if __name__ == "__main__":
    sys.exit(main())
