import argparse
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
STAGE1_EXE = os.path.join(BUILD, "lain-stage1.exe" if os.name == "nt" else "lain-stage1")
STAGE2_L1 = os.path.join(BUILD, "stage2.l1")
STAGE2_C = os.path.join(BUILD, "stage2.c")
STAGE2_EXE = os.path.join(BUILD, "lain-stage2.exe" if os.name == "nt" else "lain-stage2")
STAGE3_L1 = os.path.join(BUILD, "stage3.l1")
NATIVE_DRIVER = os.path.join(ROOT, "seed", "native_meta_driver.c")
BOOT_C = os.path.join(ROOT, "seed", "bootstrap", "lainc_boot.c")
KERNEL_INC = os.path.join(ROOT, "seed", "kernel", "include")
SEED_LIB = os.path.join(BUILD, "lib", "lain_seed.lib" if os.name == "nt" else "liblain_seed.a")


def exe(name):
    path = os.path.join(BUILD, name)
    if os.name == "nt" and os.path.exists(path + ".exe"):
        return path + ".exe"
    return path


LAINIR_VERIFY = exe("lainir-verify")
LAINIR_RUN = exe("lainir-run")
LAINIR_C = exe("lainir-c")
LAIN_META = exe("lain-meta-v2")


def run(cmd, timeout=1800):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)


def fail(message):
    print("失败: " + message)
    sys.exit(1)


def build():
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
    for tool in (LAINIR_VERIFY, LAINIR_RUN, LAINIR_C, LAIN_META):
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
    """构建原生 Stage-1 编译器，再用它把 src/entry.lain 编成 build/stage2.l1。

    用 seed/bootstrap/lainc_boot.c（C99 自举快照）冷启动 Stage-1。
    """
    clang_bin = shutil.which("clang")
    if not clang_bin:
        print("失败: 未找到 clang，无法构建 Stage-1")
        return None
    res_cc = run([clang_bin, "-std=c99", "-O2", "-I", KERNEL_INC,
                  NATIVE_DRIVER, BOOT_C, SEED_LIB, "-o", STAGE1_EXE])
    if res_cc.returncode != 0:
        print(res_cc.stdout)
        print(res_cc.stderr)
        return None
    if os.path.exists(STAGE2_L1):
        os.remove(STAGE2_L1)
    res_s2 = run([STAGE1_EXE, META_ENTRY, "--out", STAGE2_L1])
    if res_s2.returncode != 0 or not os.path.exists(STAGE2_L1):
        print(res_s2.stdout)
        print(res_s2.stderr)
        return None
    res_v2 = run([LAINIR_VERIFY, STAGE2_L1])
    if res_v2.returncode != 0:
        print(res_v2.stdout)
        print(res_v2.stderr)
        return None
    return [STAGE2_L1]


def compile_via_native(stem, folder):
    """用原生 Stage-2 编译器编译一个样例。回 (输出文本, 失败文本或 None)。"""
    os.makedirs(META_OUT, exist_ok=True)
    out_path = os.path.join(META_OUT, stem + ".l1")
    if os.path.exists(out_path):
        os.remove(out_path)
    result = run([STAGE2_EXE, os.path.join(folder, stem + ".lain"),
                  "--out", out_path])
    text = result.stdout + result.stderr
    if result.returncode != 0:
        body = text.strip().splitlines()
        return text, body[-1] if body else "失败"
    if not os.path.exists(out_path):
        return text, "编译器没写产物"
    return text, None


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


def program_stdout(text):
    """保留程序自身的标准输出，去掉运行遥测与结果行。"""
    lines = []
    for line in text.replace("\r\n", "\n").split("\n"):
        if line.startswith(("run slice=", "result value=", "result addr=", "result=", "trap ")):
            break
        lines.append(line)
    while lines and lines[-1] == "":
        lines.pop()
    return lines


def run_native(stem, meta_path, entry):
    """把一份 IR 发射成 C99、用 clang 编译并运行。回 (输出, 失败文本或 None)。"""
    os.makedirs(WORK, exist_ok=True)
    clang_bin = shutil.which("clang")
    if not clang_bin:
        return None, "未找到 clang"
    c_path = os.path.join(WORK, stem + ".c")
    res = run([LAINIR_C, "--entry", entry, "--print-result", "-o", c_path, meta_path])
    if res.returncode != 0:
        return None, "C99 发射失败"
    exe_path = os.path.join(WORK, stem + (".exe" if os.name == "nt" else ""))
    cc = run([clang_bin, "-std=c99", "-O2", c_path, "-o", exe_path])
    if cc.returncode != 0:
        return None, "C99 编译失败"
    nat = run([exe_path])
    if nat.returncode != 0:
        return None, "C99 运行失败"
    return nat.stdout + nat.stderr, None


def measure(result, expected):
    """按期望值的符号选择读数：非负期望比无符号值，负期望比有符号值。"""
    return result["value"] if expected >= 0 else result["signed"]


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
    # 10 字段结构体（超出聚合旧的内联 8 槽）+ comptime for 展开 + [:expr] 插值：
    # 106556 = 80919（size_of 80*1000 + align_of 8*100 + field_count 10*10 + m+q 19）
    #          + 60（0..4 展开）+ 55（fields_of(Big10) 逐字段求和）
    #          + 582（fields_of(Vec3) 通用拷贝后再读回）+ 100（tail2）
    #          + 24000（Vec3 字段类型尺寸之和 24*1000）+ 24（字段偏移之和）
    #          + 777（deref(d).[:f.name][0] 插值后再写下标）
    #          + 5（src.[:"x"] 字符串字面量插值）+ 34（退栈后同名局部量 k/f 不受污染）。
    # 产物里不能有运行期 #loop：循环已逐轮展开进过程体。
    {"file": "comptime_for_splice.lain", "entry": "s0__main", "value": 106556,
     "folds": [], "procs": ["s0__copy_vec3", "s0__set_first", "s0__tail2"],
     "absent": ["#loop"]},
    # Phase 2A-2：Type 一等值 + TypeBuilder + 类型工厂 make_soa（0 字符串拼接、0 inject）。
    {"file": "soa_typebuilder.lain", "entry": "s0__main", "value": 1753,
     "folds": [], "procs": ["s0__soa_set", "s0__soa_get_sum", "s0__inline_soa_x"],
     "absent": ["#loop", "s0__make_soa"]},
    # Phase 4：Ord 模块签名 + use 自动命中 + ?{} 覆写 + 块内 use 就近优先
    # + 单态化缓存（零 icall 虚表）。
    {"file": "concepts_max.lain", "entry": "s0__main", "value": 124,
     "folds": [], "procs": ["s0__i64_less", "s0__i64_gt", "s0__u64_less"],
     "absent": ["#call [("]},
    # Phase 3A：零 addr 原语（ptr_cast / make_slice / null_ref / is_null）+ Ref/Mut 自动解引用 + 返回类型泛型合一。
    {"file": "modern_no_addr.lain", "entry": "s0__main", "value": 255,
     "folds": [], "procs": ["s0__push_item", "s0__read_item_val"],
     "absent": []},
    # Phase 4：嵌套模块 + 多级路径 + 模块级 open + 块级 use 词法边界。
    # 226 = add(3,4)+(6,7)= (20,27) 的 47 + add(c,c)+(51,70) 的 121
    # + OuterTotal 3 + 块内 total(make(2,3))+2 的 7 + total(c) 47 + make(1,1).x 1。
    {"file": "module_basic.lain", "entry": "s0__main", "value": 226,
     "folds": [], "procs": [], "absent": ["#call [("]},
    # Phase 4：函子 + 同实参记忆化 + 过程内实例化。
    # 64 = R4 环形覆盖后的 15 + R4Again 的 15 + R3 的 17 + 未满追加的 1
    # + 过程内 RingBuf(u64, 2) 的 16。
    {"file": "module_functor.lain", "entry": "s0__main", "value": 64,
     "folds": [], "procs": [], "absent": []},
    # Phase 4：自定义结构体 Eq 见证 + 同类型两份策略 + 显式 ?{ } 覆写 + 块内 use 切换。
    # 1110 = EqVec2 命中的 1000 + 块内 EqMod10 的 10 + 显式 EqMod10 的 100；
    # 两个应判不等的调用各占 500 / 50，命中即为错。
    {"file": "module_concept_self.lain", "entry": "s0__main", "value": 1110,
     "folds": [], "procs": [], "absent": ["#call [("]},
    # Phase 5.1：Expand -> Elaborate -> Lower 管线端到端回归。
    # 42 = comptime for 0..3 展开的 3 + Vec2Add.sum(Vec2{5, 9}) 的 14
    # + 数组 tail 的 10 + 15。
    {"file": "ast_out_expand.lain", "entry": "s0__main", "value": 42,
     "folds": [], "procs": [], "absent": []},
    # Phase 5.2：UFCS 值接收者 + Ref(Self) / Mut(Self) 首参自动借用。
    # 41 = len2(Vec2{3, 4}) 的 25 + add_x(mut_of(v), 5) 的 8 + get_x(ref_of(v)) 的 8。
    {"file": "ufcs_basic.lain", "entry": "s0__main", "value": 41,
     "folds": [], "procs": [], "absent": []},
    # Phase 5.2：f"..." 插值写入栈上字符池并经 std::io 输出，返回确定性整数。
    {"file": "fmt_interp.lain", "entry": "s0__main", "value": 42,
     "folds": [], "procs": [], "absent": []},
    # Phase 5.2：match 返回 i64，Some(x) 绑定载荷。
    # 42 = unwrap_plus_one(Some(41)) 的 42 + unwrap_plus_one(None) 的 0。
    {"file": "match_enum.lain", "entry": "s0__main", "value": 42,
     "folds": [], "procs": [], "absent": []},
    # Phase 5.3：中止性效应的 Value-Error-Stopped 三通道模型。
    # 142 = 值通道 load(21)! 的 42 + 错误通道的 80 + 取消通道的 20。
    {"file": "effect_abortive.lain", "entry": "s0__main", "value": 142,
     "folds": [], "procs": [], "absent": []},
    # Phase 5.3：参数化可恢复效应两次挂起并用 resume 回送加工后的值。
    # 60 = emit(10) 恢复出的 20 + emit(20) 恢复出的 40。
    {"file": "effect_resumable.lain", "entry": "s0__main", "value": 60,
     "folds": [], "procs": [], "absent": []},
    # Phase 5.3：效应行多态让同一高阶过程接受纯回调与带 Cancel 的回调。
    # 55 = 纯实例化 7*2+1 的 15 + 效应实例化 20*2 的 40。
    {"file": "effect_row_poly.lain", "entry": "s0__main", "value": 55,
     "folds": [], "procs": [], "absent": []},
    # Phase 6.1：纯 Lain 的 LAINIR 校验器与 C99 后端。内存里构造的合法模块必须
    # 被 verify_module 判为 0，四类畸形模块必须各自被拒；前导 + 过程发射出的
    # C99 文本必须含 switch / lain_load_bits / return 且非空。
    {"file": "phase6_ir_c99.lain", "entry": "s0__main", "value": 61,
     "folds": [], "procs": [], "absent": []},
    # Phase 6.2：纯 Lain 的 Reader 与 AstOut。多行源码切出的 RawAst 节点数、孩子表、
    # 记号文本与 1 基行列号必须对得上；括号错配与未闭合字符串必须给出确定错误码；
    # AstOut 回滚复位水位，第二次事务提交 sum(1, 2) 子树后结构完整。
    {"file": "phase6_reader_astout.lain", "entry": "s0__main", "value": 62,
     "folds": [], "procs": [], "absent": []},
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
    {"file": "splice_runtime_name.lain", "why": "[:expr] 的成员名来自运行期形参"},
    {"file": "splice_missing_field.lain", "why": "[:expr] 插值出结构体里不存在的字段"},
    {"file": "type_runtime_err.lain", "why": "Type 不能作为运行期变量类型"},
    {"file": "record_empty_err.lain", "why": "record_end 不能构造空结构体"},
    {"file": "record_dup_err.lain", "why": "record_add 不能添加重复字段名"},
    {"file": "concept_conflict.lain", "why": "泛型实参类型推导冲突"},
    {"file": "concept_missing_given.lain", "why": "缺少对应类型的模块见证"},
    {"file": "concept_ambig_given.lain", "why": "同层多个模块见证歧义"},
    {"file": "concept_rec_limit.lain", "why": "泛型递归实例化超过深度上限"},
    {"file": "legacy_top_func_err.lain", "why": "顶层 func 已移除，必须改用 let = func"},
    {"file": "legacy_top_struct_err.lain", "why": "顶层 struct 已移除，必须改用 let = struct"},
    {"file": "legacy_given_err.lain", "why": "given 关键字已退场，必须改用 sig + module + use"},
    {"file": "mod_private_err.lain", "why": "模块私有成员不能被模块外访问"},
    {"file": "sig_mismatch_err.lain", "why": "实现模块缺少签名要求的公开成员"},
    {"file": "ufcs_ambig_err.lain", "why": "两个活跃模块为同一类型导出同名 UFCS 方法，必须报歧义"},
    {"file": "match_nonexhaustive_err.lain", "why": "枚举变体未覆盖且没有通配分支，必须报穷尽缺失"},
    {"file": "effect_unhandled_err.lain", "why": "纯过程发起未捕获的中止性效应，必须被纯度检查拒绝"},
    {"file": "effect_missing_bang_err.lain", "why": "调用带效应的子过程漏掉尾随的 ! 传播操作符，必须报错"},
    {"file": "effect_multishot_err.lain", "why": "处理器分支两次调用 resume，必须违反单次恢复契约"},
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


def compile_meta_test(stem):
    """用原生 Stage-2 编译器编译 tests/meta/<stem>.lain。回 (产物路径, 驱动输出, 驱动退出码)。"""
    out_path = os.path.join(META_TEST_OUT, stem + ".l1")
    if os.path.exists(out_path):
        os.remove(out_path)
    command = [STAGE2_EXE, os.path.join("tests", "meta", stem + ".lain"),
               "--out", out_path]
    result = run(command)
    return out_path, result.stdout + result.stderr, result.returncode


def check_meta_cases():
    """跑 Meta 扩展测试。回 (通过数, 总数, 明细列表)。"""
    passed = 0
    details = []
    for case in META_CASES:
        stem = case["file"][:-len(".lain")]
        out_path, text, code = compile_meta_test(stem)
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
        present = [s for s in case.get("absent", []) if s in ir_text]
        if present:
            details.append("%s 产物里出现了 %s" % (case["file"], " ".join(present)))
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
        out_path, text, code = compile_meta_test(stem)
        if "result=0" in text:
            details.append("%s 编译成功，但%s必须失败" % (negative["file"], negative["why"]))
            continue
        if "diag " not in text or code == 0:
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
    parser = argparse.ArgumentParser(description="Lain 全量验证脚本")
    parser.add_argument("--update-bootstrap", action="store_true",
                        help="全部检查与固定点通过后，把 build/stage2.c 刷新为 seed/bootstrap/lainc_boot.c")
    args = parser.parse_args()
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

    clang_bin = shutil.which("clang")

    print()
    print("[C99 自举验证]  Stage-1 -> Stage-2 -> Stage-3 固定点一致")
    bootstrap_ok = False
    stage2_ready = False
    if units and clang_bin:
        self_res = run([LAINIR_C, "-o", STAGE2_C] + units)
        if self_res.returncode != 0:
            problems.append("C99 自举: lainir-c 发射 stage2.c 失败")
            print("C99 自举 失败")
        else:
            cc_self = run([clang_bin, "-std=c99", "-O2", "-I", KERNEL_INC,
                           NATIVE_DRIVER, STAGE2_C, SEED_LIB, "-o", STAGE2_EXE])
            if cc_self.returncode != 0:
                problems.append("C99 自举: clang 编译 stage2.exe 失败")
                print("C99 自举 失败")
            else:
                stage2_ready = True
                if os.path.exists(STAGE3_L1):
                    os.remove(STAGE3_L1)
                res_s3 = run([STAGE2_EXE, META_ENTRY, "--out", STAGE3_L1])
                if res_s3.returncode != 0 or not os.path.exists(STAGE3_L1):
                    problems.append("C99 自举: Stage-2 编译 stage3.l1 失败")
                    print("C99 自举 失败")
                else:
                    res_v3 = run([LAINIR_VERIFY, STAGE3_L1])
                    if res_v3.returncode != 0:
                        problems.append("C99 自举: stage3.l1 校验失败")
                        print("C99 自举 失败")
                    else:
                        with open(STAGE2_L1, "rb") as f2, open(STAGE3_L1, "rb") as f3:
                            same = (f2.read() == f3.read())
                        if not same:
                            problems.append("C99 自举: stage2.l1 与 stage3.l1 不一致")
                            print("C99 自举 失败（固定点不一致）")
                        else:
                            bootstrap_ok = True
                            print("C99 自举 通过（Stage-1 -> Stage-2 -> Stage-3 固定点一致，%d KB IR / %d KB C99）"
                                  % (os.path.getsize(STAGE2_L1) // 1024,
                                     os.path.getsize(STAGE2_C) // 1024))
    elif units:
        print("C99 自举 跳过（未找到 clang）")
    else:
        print("C99 自举 跳过（自举单元缺失）")

    print()
    print("[IR 层用例]  间接 #call（取值 / 尾调用 / 非法目标 / 签名不符）")
    ir_passed, ir_total, ir_details = check_ir_cases()
    print("IR 用例 %d/%d" % (ir_passed, ir_total))
    problems += ir_details

    print()
    print("[Meta 扩展测试]  编译期求值 eval（常量折叠 / 隔离执行）、代码注入 inject（静态反射 / SoA 与 VTable）与间接调用 icall")
    if stage2_ready:
        meta_passed, meta_total, meta_details = check_meta_cases()
        print("Meta 用例 %d/%d" % (meta_passed, meta_total))
        problems += meta_details
    else:
        print("Meta 用例 跳过（Stage-2 编译器缺失）")

    print()
    print("[现代样例]  examples/*.lain 编译、校验、VM 与 C99 原生差分")
    if stage2_ready and os.path.isdir(EXAMPLES):
        ex_names = sorted(n for n in os.listdir(EXAMPLES) if n.endswith(".lain"))
        ex_pass = 0
        for ex_name in ex_names:
            ex_stem = ex_name[:-len(".lain")]
            _, ex_err = compile_via_native(ex_stem, EXAMPLES)
            if ex_err:
                problems.append("examples/%s 编译: %s" % (ex_name, ex_err))
                continue
            ex_out = os.path.join(META_OUT, ex_stem + ".l1")
            ex_ver = run([LAINIR_VERIFY, ex_out])
            if ex_ver.returncode != 0:
                problems.append("examples/%s 校验失败" % ex_name)
                continue
            procs = proc_names([ex_out])
            entry = "s0__main" if "s0__main" in procs else ("main" if "main" in procs else None)
            if entry:
                got, run_err, vm_text = execute([ex_out], entry, [])
                if run_err:
                    problems.append("examples/%s VM 运行失败: %s" % (ex_name, run_err))
                    continue
                nat_text, nat_err = run_native(ex_stem, ex_out, entry)
                if nat_err:
                    problems.append("examples/%s %s" % (ex_name, nat_err))
                    continue
                if program_stdout(vm_text) != program_stdout(nat_text):
                    problems.append("examples/%s C99 与 VM 标准输出不一致" % ex_name)
                    continue
                m = re.search(r"result=(-?\d+)", nat_text)
                if not m or int(m.group(1)) != got["signed"]:
                    problems.append("examples/%s C99 与 VM 结果不一致: %s vs %s"
                                    % (ex_name, nat_text.strip(), got["signed"]))
                    continue
            ex_pass += 1
        print("现代样例 %d/%d" % (ex_pass, len(ex_names)))
    else:
        print("现代样例 跳过")

    print()
    print("[标准库]  std::test / std::bench 双后端；lain_io_write 越界访问必须触发 VM 陷阱")
    std_passed = 0
    std_cases = [
        ("std_test_self", 0, ["[PASS] self (5/5)"]),
        ("std_test_fail", 2, ["[FAIL] failing :: false-check",
                              "[FAIL] failing :: wrong-i64: expected 2, got 1",
                              "[FAIL] failing (2 failed)"]),
        ("std_bench_smoke", 21, None),
    ]
    if units and stage2_ready:
        for std_stem, std_value, std_lines in std_cases:
            _, std_err = compile_via_native(std_stem, META_TESTS)
            if std_err:
                problems.append("tests/meta/%s.lain 编译: %s" % (std_stem, std_err))
                continue
            std_out = os.path.join(META_OUT, std_stem + ".l1")
            if run([LAINIR_VERIFY, std_out]).returncode != 0:
                problems.append("tests/meta/%s.lain 校验失败" % std_stem)
                continue
            got, run_err, vm_text = execute([std_out], "s0__main", [])
            if run_err:
                problems.append("tests/meta/%s.lain VM 运行失败: %s" % (std_stem, run_err))
                continue
            if got["signed"] != std_value:
                problems.append("tests/meta/%s.lain VM 结果 %d，期望 %d"
                                % (std_stem, got["signed"], std_value))
                continue
            vm_lines = program_stdout(vm_text)
            if std_lines is not None and vm_lines != std_lines:
                problems.append("tests/meta/%s.lain VM 标准输出不符: %s"
                                % (std_stem, vm_lines))
                continue
            if std_lines is None and not any(
                    line.startswith("[bench] work steps=") for line in vm_lines):
                problems.append("tests/meta/%s.lain VM 没有输出 bench 遥测" % std_stem)
                continue
            nat_text, nat_err = run_native(std_stem, std_out, "s0__main")
            if nat_err:
                problems.append("tests/meta/%s.lain %s" % (std_stem, nat_err))
                continue
            if std_lines is not None and program_stdout(nat_text) != std_lines:
                problems.append("tests/meta/%s.lain C99 标准输出不符" % std_stem)
                continue
            if std_lines is None and "[bench] work steps=" not in nat_text:
                problems.append("tests/meta/%s.lain C99 没有输出 bench 遥测" % std_stem)
                continue
            nat_hit = re.search(r"result=(-?\d+)", nat_text)
            if not nat_hit or int(nat_hit.group(1)) != std_value:
                problems.append("tests/meta/%s.lain C99 结果不符: %s"
                                % (std_stem, nat_text.strip()))
                continue
            std_passed += 1
        _, oob_err = compile_via_native("std_io_oob", META_TESTS)
        if oob_err:
            problems.append("tests/meta/std_io_oob.lain 编译: %s" % oob_err)
        else:
            oob_out = os.path.join(META_OUT, "std_io_oob.l1")
            _, oob_run_err, oob_text = execute([oob_out], "s0__main", [])
            if not oob_run_err or "trap kind=2 status=1004" not in oob_text:
                problems.append("tests/meta/std_io_oob.lain 没有触发 lain_io_write 越界陷阱")
            else:
                std_passed += 1
        print("标准库 %d/%d" % (std_passed, len(std_cases) + 1))
    else:
        print("标准库 跳过")

    if args.update_bootstrap and bootstrap_ok and not problems:
        os.makedirs(os.path.dirname(BOOT_C), exist_ok=True)
        shutil.copyfile(STAGE2_C, BOOT_C)
        print("自举快照已刷新: %s" % os.path.relpath(BOOT_C, ROOT))
    elif args.update_bootstrap:
        print("自举快照 未刷新（固定点未通过或检查被跳过）")

    # 现代源码零 addr / 零 given 审查：tests/meta 里的现代样例与 legacy/std 负测试
    # 也必须零 addr；given 关键字已退场，只有 legacy_given_err.lain 故意用它测诊断。
    meta_zero_names = ["modern_no_addr.lain", "legacy_top_func_err.lain",
                       "legacy_top_struct_err.lain", "std_test_self.lain",
                       "std_test_fail.lain", "std_bench_smoke.lain", "std_io_oob.lain",
                       "module_basic.lain", "module_functor.lain", "module_concept_self.lain",
                       "concepts_max.lain", "concept_ambig_given.lain",
                       "concept_conflict.lain", "concept_missing_given.lain",
                       "legacy_given_err.lain", "mod_private_err.lain",
                       "sig_mismatch_err.lain"]
    modern_targets = [os.path.join(META_TESTS, n) for n in meta_zero_names]
    if os.path.isdir(EXAMPLES):
        modern_targets += [os.path.join(EXAMPLES, n)
                           for n in sorted(os.listdir(EXAMPLES)) if n.endswith(".lain")]
    std_dir = os.path.join(ROOT, "std")
    if os.path.isdir(std_dir):
        modern_targets += [os.path.join(std_dir, n)
                           for n in sorted(os.listdir(std_dir)) if n.endswith(".lain")]
    src_dir = os.path.join(ROOT, "src")
    if os.path.isdir(src_dir):
        for dirpath, dirnames, filenames in os.walk(src_dir):
            dirnames.sort()
            modern_targets += [os.path.join(dirpath, n)
                               for n in sorted(filenames) if n.endswith(".lain")]
    given_exempt = {os.path.join(META_TESTS, "legacy_given_err.lain")}
    for zpath in modern_targets:
        with open(zpath, "r", encoding="utf-8") as zh:
            zraw = zh.read()
        zsrc = re.sub(r"//.*", "", zraw)
        zcode = re.sub(r'"(?:\\.|[^"\\])*"', '""', zsrc)
        zrel = os.path.relpath(zpath, ROOT)
        if re.search(r"\baddr\b", zsrc):
            problems.append("零 addr 审查失败: %s 仍含裸 addr 类型" % zrel)
        if zpath not in given_exempt and re.search(r"\bgiven\b", zcode):
            problems.append("零 given 审查失败: %s 仍含旧式 given 关键字" % zrel)
    print("零 addr / 零 given 现代文件检查 %d 份通过" % len(modern_targets))

    if problems:
        print()
        print("失败明细:")
        for item in problems:
            print("  " + item)
    print("耗时 %.1f 秒" % (time.time() - started))
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
