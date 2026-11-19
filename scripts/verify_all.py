import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
WORK = os.path.join(BUILD, "verify_all")
EXAMPLES = os.path.join(ROOT, "examples")
META_ENTRY = os.path.join(ROOT, "src", "entry.lain")
META_UNITS = os.path.join(BUILD, "metaunits")
META_OUT = os.path.join(BUILD, "meta")


def exe(name):
    path = os.path.join(BUILD, name)
    if os.name == "nt" and os.path.exists(path + ".exe"):
        return path + ".exe"
    return path


LAIN0 = os.path.join(BUILD, "lain0", "lain0.exe") if os.name == "nt" else os.path.join(BUILD, "lain0", "lain0")
LAINIR_VERIFY = exe("lainir-verify")
LAINIR_RUN = exe("lainir-run")
LAIN_META = exe("lain-meta-v2")


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

# 没有真值用例的样例，以及原因。
NO_RUN = {
    "extern": "入口依赖宿主能力表（驱动不注册）",
}


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
    for tool in (LAIN0, LAINIR_VERIFY, LAINIR_RUN, LAIN_META):
        if not os.path.exists(tool):
            fail("没有这个产物: " + tool)


def abi_check():
    """枚举常量与头文件同步校验。回失败文本或 None。"""
    script = os.path.join(ROOT, "scripts", "gen_v2_abi.py")
    result = run([sys.executable, script, "--check"])
    if result.returncode != 0:
        return (result.stdout + result.stderr).strip() or "gen_v2_abi --check 失败"
    return None


def split_meta():
    """把 src 里的 Meta 拆成逐模块单元。回单元路径列表或 None。"""
    if os.path.isdir(META_UNITS):
        shutil.rmtree(META_UNITS)
    result = run([LAIN0, "--entry", META_ENTRY, "--split", META_UNITS])
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        return None
    return [os.path.join(META_UNITS, n) for n in sorted(os.listdir(META_UNITS))
            if n.endswith(".l1")]


def compile_sample(stem, units):
    """用 Meta 编译一个样例。回 (输出文本, 失败文本或 None)。"""
    os.makedirs(META_OUT, exist_ok=True)
    out_path = os.path.join(META_OUT, stem + ".l1")
    command = [LAIN_META] + units + [os.path.join("examples", stem + ".lain"),
                                      "--out", out_path]
    result = run(command)
    text = result.stdout + result.stderr
    if result.returncode != 0:
        body = text.strip().splitlines()
        return text, body[-1] if body else "失败"
    if not os.path.exists(out_path):
        return text, "Meta 没写产物"
    return text, None


def lain0_compile_sample(stem):
    """用 lain0 编译同一个样例，作为差分基准。回产物路径或 None。"""
    out_dir = os.path.join(WORK, stem)
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "lain0.l1")
    result = run([LAIN0, os.path.join("examples", stem + ".lain"),
                  "-o", out_path, "--round-trip"])
    if result.returncode != 0 or not os.path.exists(out_path):
        return None
    return out_path


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


def check_runs(stem, meta_path, l0_path):
    """跑该样例的全部真值用例，比对 Meta 与 lain0。回 (通过数, 总数, 明细列表)。"""
    cases = RUN_CASES.get(stem)
    if not cases:
        return 0, 0, []
    meta_names = proc_names([meta_path])
    l0_names = proc_names([l0_path]) if l0_path else set()
    passed = 0
    details = []
    for func, args, expected in cases:
        meta_entry = "s0__%s" % func
        l0_entry = "%s__%s" % (stem, func)
        if meta_entry not in meta_names:
            details.append("%s：Meta 没有入口 %s" % (stem, meta_entry))
            continue
        result, error, _ = execute([meta_path], meta_entry, args)
        if error:
            details.append("%s %s%r: %s" % (stem, func, tuple(args), error))
            continue
        got = measure(result, expected)
        if got != expected:
            details.append("%s %s%r 期望 %d，实得 %d"
                           % (stem, func, tuple(args), expected, got))
            continue
        if l0_path:
            if l0_entry not in l0_names:
                details.append("%s：lain0 没有入口 %s" % (stem, l0_entry))
                continue
            ref, ref_error, _ = execute([l0_path], l0_entry, args)
            if ref_error:
                details.append("%s lain0 %s%r: %s" % (stem, func, tuple(args), ref_error))
                continue
            if measure(ref, expected) != measure(result, expected):
                details.append("%s %s%r 与 lain0 不一致：lain0 %d，Meta %d"
                               % (stem, func, tuple(args), measure(ref, expected),
                                  measure(result, expected)))
                continue
        passed += 1
    return passed, len(cases), details


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
            return body[0] if body else "往返校验失败"
        if second.stdout != first.stdout:
            return "往返文本不一致"
    finally:
        if os.path.exists(second_path):
            os.remove(second_path)
    return None


def check_ir_cases():
    """跑 IR 层用例。回 (通过数, 总数, 明细列表)。"""
    passed = 0
    details = []
    for case in IR_CASES:
        path = os.path.join(IR_TESTS, case["file"])
        if not os.path.exists(path):
            details.append("缺用例文件: " + path)
            continue
        error = ir_roundtrip(path)
        if error:
            details.append("%s 往返: %s" % (case["file"], error))
            continue
        command = [LAINIR_RUN, "--entry", case["entry"]]
        for value in case.get("args", []):
            command += ["--arg", str(value)]
        command.append(path)
        result = run(command)
        text = result.stdout + result.stderr
        if "trap" in case:
            hit = re.search(r"trap kind=\d+ status=(\d+)", text, re.IGNORECASE)
            if result.returncode == 0 or not hit:
                details.append("%s %s 期望陷阱 %d"
                               % (case["file"], case["entry"], case["trap"]))
                continue
            if int(hit.group(1)) != case["trap"]:
                details.append("%s %s 陷阱码 %d，应为 %d"
                               % (case["file"], case["entry"], int(hit.group(1)),
                                  case["trap"]))
                continue
            passed += 1
            continue
        if result.returncode != 0:
            body = text.strip().splitlines()
            details.append("%s %s: %s" % (case["file"], case["entry"],
                                          body[-1] if body else "无输出"))
            continue
        hit = re.search(r"result value=(\d+)", text)
        if not hit or int(hit.group(1)) != case["value"]:
            details.append("%s %s 应为 %d"
                           % (case["file"], case["entry"], case["value"]))
            continue
        passed += 1
    return passed, len(IR_CASES), details


# Meta 扩展测试：编译期求值（eval）、编译期代码注入（inject）与间接调用（icall）都必须
# 留下可执行产物，且产物里不留求值/注入入口；负测试必须给出编译错误而不是 VM 崩溃。
META_TESTS = os.path.join(ROOT, "tests", "meta")
META_TEST_OUT = os.path.join(WORK, "meta_tests")
META_CASES = [
    {"file": "eval_comptime.lain", "entry": "s0__main", "value": 5112,
     "folds": [("s0__folded", "55"), ("s0__folded_plus", "55")], "procs": []},
    # 静态反射现场生成 Vec3_SoA 与 soa_pack / soa_x / soa_y / soa_z：
    # 61 = 10 + 20 + 30 + soa_pack 的 1 + 反射折叠偏差 0。
    {"file": "soa_reflect.lain", "entry": "s0__main", "value": 61, "folds": [],
     "procs": ["s0__soa_pack", "s0__soa_x", "s0__soa_y", "s0__soa_z"]},
    # 接口与 VTable：inject 生成两张表与两个多态过程，多态函数从 VTable 取出
    # 函数地址后做间接调用。67 = Circle.area(7) + Rect.scale(3*4, 5)。
    {"file": "interface_vtable.lain", "entry": "s0__main", "value": 67, "folds": [],
     "procs": ["s0__shape_area", "s0__shape_scale", "s0__circle_vtable",
               "s0__rect_vtable", "s0__as_shape_Circle", "s0__as_shape_Rect"],
     "contains": ["#call [(#addr) -> #bits<64>](",
                  "#call [(#addr, #bits<64>) -> #bits<64>]("]},
    # 复合类型基础设施：窄标量的 ref_of/mut_of/deref 按真实位宽读（u8），
    # 聚合引用 deref 之后的字段与嵌套字段链都能定位。
    # 925 = u8 的 200 + mut 的 3 + deref(rf).pos.x 的 7 + frame_score 的 704
    # + Mut(i64) 隐式转 Ref(i64) 后解引用的 11。
    {"file": "refs_deref.lain", "entry": "s0__main", "value": 925, "folds": [],
     "procs": ["s0__frame_score"]},
    # Fn 值走间接调用完成动态多态：s.vtable.scale(s.data, k) 按签名校验实参；
    # Fn 还能做形参并在标注位置直接收过程名。
    # 56 = area_of(Circle{r:7}) 的 7 + scaled(..., 5) 的 35 + f(1) 的 2 + twice 的 12。
    {"file": "fn_vtable.lain", "entry": "s0__main", "value": 56, "folds": [],
     "procs": ["s0__area_of", "s0__scaled", "s0__circle_area",
               "s0__circle_scale", "s0__inc", "s0__twice"],
     "contains": ["#call [(#addr) -> #bits<64>](",
                  "#call [(#addr, #bits<64>) -> #bits<64>](",
                  "#call [(#bits<64>) -> #bits<64>]("]},
    # Array(u64, 4) 局部字面量与结构体字段、数组形参、Slice/MutSlice 视图：
    # 4162 = 4000（len）+ 100（四元素求和）+ 50（形参数组的 s[0]+s[3]）
    # + 10（字段数组的 s[1]+s[2] 加 tag=5）+ 2（slice_of(Ref,2) 的 len）。
    {"file": "array_slice.lain", "entry": "s0__main", "value": 4162, "folds": [],
     "procs": ["s0__sum_slice", "s0__head_tail", "s0__row_mid"]},
    # 字符串反转义后作为 Slice(u8) 使用：a、换行、b、双引号、c、反斜杠共 6 字节，
    # 和 430，6430 = 6*1000 + 430。
    {"file": "str_escape.lain", "entry": "s0__main", "value": 6430, "folds": [],
     "procs": ["s0__sum_bytes"],
     "contains": ["data s0__strlit_0 ro"]},
    # 通用左值 + sret 聚合返回 + let = func 与无捕获匿名函数：
    # 167 = bump 105 + m.y 9 + bx.pos.x 7 + c 11 + 数组和 12 + apply 21
    #       + is_odd 1 + is_even 1。
    {"file": "lval_sret_func.lain", "entry": "s0__main", "value": 167, "folds": [],
     "procs": ["s0__mk", "s0__bump", "s0__set_y", "s0__set_pos_x", "s0__set_slot",
               "s0__fill", "s0__sum", "s0__is_odd", "s0__is_even", "s0__apply",
               "s0__anon0"],
     "contains": ["#call @s0__bump(%_t149, %_t148)"]},
    # 干净版列式 SoA：注入的字段是真的 Array(i64, 4) 列，生成器用 Slice(u8)
    # 与转义写入器拼源码。81 = 四组 (x+y+z) 的和 78 + soa_quote 的 3。
    {"file": "soa_clean.lain", "entry": "s0__main", "value": 81, "folds": [],
     "procs": ["s0__soa_set", "s0__soa_at", "s0__soa_quote"]},
    # 干净版接口分发：Ref(T) 显式解引用、Ref(void) 承载数据、Fn 直接从成员调用。
    # 49 = circle_area(3) 的 9 + rect_scale(4x5, 2) 的 40。
    {"file": "interface_clean.lain", "entry": "s0__main", "value": 49, "folds": [],
     "procs": ["s0__area_of", "s0__scale_of", "s0__circle_area", "s0__rect_scale"],
     "contains": ["#call [(#addr) -> #bits<64>](",
                  "#call [(#addr, #bits<64>) -> #bits<64>]("]},
    {"file": "polish_syntax.lain", "entry": "s0__main", "value": 150, "folds": [],
     "procs": ["s0__triple", "s0__inc", "s0__apply", "s0__set_y"],
     "contains": ["data s0__strlit_0 ro", "#call [(#bits<64>) -> #bits<64>]("]},
]
META_NEGATIVE = [
    {"file": "eval_runtime_ref.lain", "why": "eval 内引用了运行期形参"},
    {"file": "inject_missing.lain", "why": "inject 指向不存在的生成器"},
    {"file": "inject_bad_snippet.lain", "why": "注入片段无法解析"},
    {"file": "reflect_bad_type.lain", "why": "反射查询用了越界的字面量 TypeId"},
    {"file": "eval_trap.lain", "why": "被求值的表达式在隔离执行里触发陷阱"},
    {"file": "icall_bad_sig.lain", "why": "间接调用的目标不是 addr"},
    {"file": "icall_bad_args.lain", "why": "间接调用的实参个数与签名不符"},
    {"file": "deref_nonref.lain", "why": "对非引用值做解引用"},
    {"file": "deref_void.lain", "why": "对 Ref(void) 直接解引用"},
    {"file": "fn_bad_args.lain", "why": "Fn 值的实参个数与签名不符"},
    {"file": "ref_to_mut.lain", "why": "把 Ref(T) 当成 Mut(T) 传递"},
    {"file": "slice_to_mutslice.lain", "why": "把 Slice(T) 当成 MutSlice(T) 传递"},
    {"file": "fn_bad_type.lain", "why": "Fn 值的实参类型与签名不符"},
    {"file": "array_bad_len.lain", "why": "数组字面量元素个数与 Array(T, N) 不符"},
    {"file": "lval_ref_ro.lain", "why": "通过只读 Ref 写字段必须失败"},
    {"file": "lval_slice_ro.lain", "why": "通过只读 Slice 写元素必须失败"},
    {"file": "anon_capture.lain", "why": "无名函数捕获外层局部量必须失败"},
    {"file": "lval_bad.lain", "why": "非法左值必须报错"},
    {"file": "infer_missing.lain", "why": "let 右值未绑定，无法推导类型"},
    {"file": "fn_value_bad_type.lain", "why": "过程值与目标 Fn 签名不符"},
    {"file": "void_value.lain", "why": "void 调用没有可绑定的返回值"},
]


def proc_block(text, name):
    """取 “#proc <name>(” 到该过程收尾 “}” 之间的正文。"""
    start = text.find("#proc %s(" % name)
    if start < 0:
        return None
    end = text.find("\n}\n", start)
    if end < 0:
        return None
    return text[start:end]


def compile_meta_test(stem, units):
    """编译 tests/meta/<stem>.lain。回 (产物路径, 驱动输出, 驱动退出码)。"""
    out_path = os.path.join(META_TEST_OUT, stem + ".l1")
    if os.path.exists(out_path):
        os.remove(out_path)
    command = [LAIN_META] + units + [os.path.join("tests", "meta", stem + ".lain"),
                                     "--out", out_path]
    result = run(command)
    return out_path, result.stdout + result.stderr, result.returncode


def check_meta_cases(units):
    """跑 Meta 扩展测试。回 (通过数, 总数, 明细列表)。"""
    passed = 0
    details = []
    for case in META_CASES:
        stem = case["file"][:-len(".lain")]
        out_path, text, code = compile_meta_test(stem, units)
        if code != 0 or "result=0" not in text:
            body = text.strip().splitlines()
            details.append("%s 编译: %s" % (case["file"], body[-1] if body else "失败"))
            continue
        keep = run([LAINIR_VERIFY, out_path])
        if keep.returncode != 0:
            body = (keep.stdout + keep.stderr).strip().splitlines()
            details.append("%s 校验: %s" % (case["file"], body[0] if body else "失败"))
            continue
        with open(out_path, "r", encoding="utf-8") as handle:
            ir_text = handle.read()
        if "@__meta_eval" in ir_text:
            details.append("%s 产物里残留求值入口 @__meta_eval" % case["file"])
            continue
        missing = [p for p in case.get("procs", []) if ("#proc %s(" % p) not in ir_text]
        if missing:
            details.append("%s 缺少生成出来的过程 %s" % (case["file"], " ".join(missing)))
            continue
        lacking = [s for s in case.get("contains", []) if s not in ir_text]
        if lacking:
            details.append("%s 产物里缺少 %s" % (case["file"], " ".join(lacking)))
            continue
        broken = False
        for proc, literal in case["folds"]:
            block = proc_block(ir_text, proc)
            if block is None:
                details.append("%s 缺少过程 %s" % (case["file"], proc))
                broken = True
                break
            if "#call @" in block:
                details.append("%s 的 %s 仍在调用子过程" % (case["file"], proc))
                broken = True
                break
            if literal not in block:
                details.append("%s 的 %s 没有折叠出常量 %s"
                               % (case["file"], proc, literal))
                broken = True
                break
        if broken:
            continue
        got, error, _ = execute([out_path], case["entry"], [])
        if error or not got or measure(got, case["value"]) != case["value"]:
            details.append("%s 执行 %s 应为 %d"
                           % (case["file"], case["entry"], case["value"]))
            continue
        passed += 1
    # 负测试：必须给出编译错误诊断，且不留下可执行产物。
    for negative in META_NEGATIVE:
        stem = negative["file"][:-len(".lain")]
        out_path, text, code = compile_meta_test(stem, units)
        if "result=0" in text:
            details.append("%s 编译成功，但%s必须失败" % (negative["file"], negative["why"]))
            continue
        if "compile failed" not in text:
            body = text.strip().splitlines()
            details.append("%s 没有编译失败诊断: %s"
                           % (negative["file"], body[-1] if body else "无输出"))
            continue
        written = os.path.getsize(out_path) if os.path.exists(out_path) else 0
        if written != 0:
            details.append("%s 编译失败却写出了 %d 字节产物" % (negative["file"], written))
            continue
        passed += 1
    return passed, len(META_CASES) + len(META_NEGATIVE), details


def main():
    started = time.time()
    os.makedirs(WORK, exist_ok=True)
    os.makedirs(META_TEST_OUT, exist_ok=True)
    print("构建中 …")
    build()

    problems = []
    abi_error = abi_check()
    if abi_error:
        problems.append("枚举同步: " + abi_error)
        units = None
    else:
        units = split_meta()
        if not units:
            problems.append("Meta 自举: lain0 --split src/entry.lain 失败")

    names = sorted(n for n in os.listdir(EXAMPLES) if n.endswith(".lain"))
    rows = []
    total_pass = 0
    total_cases = 0
    if units:
        for name in names:
            stem = name[:-len(".lain")]
            text, error = compile_sample(stem, units)
            row = {"name": stem, "compile": "ok" if not error else "FAIL",
                   "verify": "-", "run": "-", "diff": "-", "steps": 0}
            if error:
                problems.append("%s 编译: %s" % (stem, error))
                rows.append(row)
                continue
            hit = re.search(r"^steps=(\d+)$", text, re.MULTILINE)
            if hit:
                row["steps"] = int(hit.group(1))
            out_path = os.path.join(META_OUT, stem + ".l1")
            result = run([LAINIR_VERIFY, out_path])
            if result.returncode != 0:
                row["verify"] = "FAIL"
                body = (result.stdout + result.stderr).strip().splitlines()
                problems.append("%s 校验: %s" % (stem, body[0] if body else "失败"))
                rows.append(row)
                continue
            row["verify"] = "ok"
            l0_path = lain0_compile_sample(stem)
            passed, count, details = check_runs(stem, out_path, l0_path)
            total_pass += passed
            total_cases += count
            row["run"] = "%d/%d" % (passed, count) if count else "-"
            row["diff"] = "ok" if l0_path else "-"
            rows.append(row)
            for line in details:
                problems.append(line)
            print(".", end="", flush=True)
        print()

    header = ("样例", "编译", "校验", "真值", "差分", "steps")
    widths = (24, 6, 6, 8, 6, 10)
    line = "  ".join(h.ljust(w) for h, w in zip(header, widths))
    print()
    print(line)
    print("-" * len(line))
    for row in rows:
        cells = (row["name"], row["compile"], row["verify"], row["run"],
                 row["diff"], str(row["steps"]))
        print("  ".join(c.ljust(w) for c, w in zip(cells, widths)))

    failed = [r["name"] for r in rows if "FAIL" in (r["compile"], r["verify"])]
    print()
    print("样例 %d/%d 通过；真值用例 %d/%d"
          % (len(rows) - len(failed), len(rows), total_pass, total_cases))
    missing = [r["name"] for r in rows if r["run"] == "-"]
    if missing:
        print()
        print("没有真值用例的样例（只验证编译与校验）:")
        for name in missing:
            print("  %-24s %s" % (name, NO_RUN.get(name, "未登记原因")))

    print()
    print("[IR 层用例]  间接 #call（取值 / 尾调用 / 非法目标 / 签名不符）")
    ir_passed, ir_total, ir_details = check_ir_cases()
    print("IR 用例 %d/%d" % (ir_passed, ir_total))
    problems += ir_details

    print()
    print("[Meta 扩展测试]  编译期求值 eval（常量折叠 / 隔离执行）、代码注入 inject（静态反射 / SoA 与 VTable）与间接调用 icall")
    if units:
        meta_passed, meta_total, meta_details = check_meta_cases(units)
        print("Meta 用例 %d/%d" % (meta_passed, meta_total))
        problems += meta_details
    else:
        print("Meta 用例 跳过（自举单元缺失）")

    if problems:
        print()
        print("失败明细:")
        for item in problems:
            print("  " + item)
    print("耗时 %.1f 秒" % (time.time() - started))
    return 0 if not problems and not failed else 1


if __name__ == "__main__":
    sys.exit(main())
