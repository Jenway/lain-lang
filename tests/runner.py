# tests/runner.py
import os
import sys
import glob
import subprocess
import re

# ── 1. 配置路径与编译器指令 ──────────────────────────────────────────

WORKSPACE_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
COMPILER_BIN = os.path.join(WORKSPACE_ROOT, "src", "compiler", "lainc")

# 配置 Chibi 虚拟机的环境变量，保证测试时加载正确
ENV = os.environ.copy()
ENV["LD_LIBRARY_PATH"] = os.path.join(WORKSPACE_ROOT, "third_party", "chibi-scheme")
ENV["CHIBI_MODULE_PATH"] = os.path.join(WORKSPACE_ROOT, "third_party", "chibi-scheme", "lib")

# 测试套件路径
FIXTURES_DIR = os.path.join(WORKSPACE_ROOT, "tests", "fixtures")

# 统计数据
passed_tests = 0
failed_tests = 0

def log_success(name):
    global passed_tests
    passed_tests += 1
    print(f"  \033[92m[PASS]\033[0m {name}")

def log_failure(name, reason):
    global failed_tests
    failed_tests += 1
    print(f"  \033[91m[FAIL]\033[0m {name}")
    print(f"         \033[93mReason:\033[0m {reason}")


def parse_source_annotations(path):
    """Parse // exit: N, // stdout: ..., and // stderr: ... from source file."""
    result = {"exit": 0, "stdout": None, "stderr": None}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = re.match(r"\s*//\s*exit\s*:\s*(\d+)", line)
            if m:
                result["exit"] = int(m.group(1))
            m = re.match(r"\s*//\s*stdout\s*:\s*(.*)", line)
            if m:
                result["stdout"] = m.group(1).strip()
            m = re.match(r"\s*//\s*stderr\s*:\s*(.*)", line)
            if m:
                result["stderr"] = m.group(1).strip()
    return result


# ── 2. 四大测试管道实现 ──────────────────────────────────────────────

# A. check-pass: 必须编译成功
def run_check_pass():
    print("\n🚀 Running check-pass tests...")
    for path in sorted(glob.glob(os.path.join(FIXTURES_DIR, "check-pass", "*.lain"))):
        name = os.path.basename(path)
        out_c = "/tmp/lain_test_out.c"

        res = subprocess.run([COMPILER_BIN, path, out_c], env=ENV, capture_output=True, text=True)
        if res.returncode == 0:
            log_success(name)
        else:
            log_failure(name, f"Compilation failed.\n{res.stderr}")


def extract_check_patterns(path):
    expected_patterns = []
    forbidden_patterns = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            if "// CHECK:" in line:
                expected_patterns.append(line.split("// CHECK:")[1].strip())
            if "// CHECK-NOT:" in line:
                forbidden_patterns.append(line.split("// CHECK-NOT:")[1].strip())
    return expected_patterns, forbidden_patterns


# B. ui: 必须编译失败（测试编译期诊断）
def run_ui_tests():
    print("\n🚀 Running UI (expect fail) tests...")
    for path in sorted(glob.glob(os.path.join(FIXTURES_DIR, "ui", "*.lain"))):
        name = os.path.basename(path)
        out_c = "/tmp/lain_test_out.c"

        res = subprocess.run([COMPILER_BIN, path, out_c], env=ENV, capture_output=True, text=True)
        if res.returncode != 0:
            log_success(name)
        else:
            log_failure(name, "Expected compilation to fail, but it succeeded.")


# C. codegen: L1 IR "FileCheck" verification
def run_codegen_tests():
    print("\n🚀 Running codegen L1 IR tests...")
    for path in sorted(glob.glob(os.path.join(FIXTURES_DIR, "codegen", "*.lain"))):
        name = os.path.basename(path)
        out_l1 = "/tmp/lain_test_out.l1"

        # 1. 提取源码里所有的 // CHECK: 模式
        expected_patterns, forbidden_patterns = extract_check_patterns(path)

        # 2. 用 lainc --emit-l1 编译
        res = subprocess.run([COMPILER_BIN, "--emit-l1", path, out_l1], env=ENV, capture_output=True, text=True)
        if res.returncode != 0:
            log_failure(name, f"Compilation failed.\n{res.stderr}")
            continue

        # 3. 验证生成的 L1 IR 是否包含这些模式 (FileCheck)
        with open(out_l1, "r", encoding="utf-8") as f:
            generated_l1 = f.read()

        failed_pattern = None
        for pattern in expected_patterns:
            if pattern not in generated_l1:
                failed_pattern = pattern
                break

        forbidden_hit = None
        for pattern in forbidden_patterns:
            if pattern in generated_l1:
                forbidden_hit = pattern
                break

        if failed_pattern:
            log_failure(name, f"Generated L1 IR missing pattern: '{failed_pattern}'")
        elif forbidden_hit:
            log_failure(name, f"Generated L1 IR unexpectedly contained pattern: '{forbidden_hit}'")
        else:
            log_success(name)


def run_interface_tests():
    print("\n🚀 Running interface emission tests...")
    for path in sorted(glob.glob(os.path.join(FIXTURES_DIR, "interface", "*.lain"))):
        name = os.path.basename(path)
        out_lci = "/tmp/lain_test_out.lci"
        expected_patterns, forbidden_patterns = extract_check_patterns(path)

        res = subprocess.run([COMPILER_BIN, "--emit-interface", path, out_lci], env=ENV, capture_output=True, text=True)
        if res.returncode != 0:
            log_failure(name, f"Interface emission failed.\n{res.stderr}")
            continue

        with open(out_lci, "r", encoding="utf-8") as f:
            generated_lci = f.read()

        failed_pattern = None
        for pattern in expected_patterns:
            if pattern not in generated_lci:
                failed_pattern = pattern
                break

        forbidden_hit = None
        for pattern in forbidden_patterns:
            if pattern in generated_lci:
                forbidden_hit = pattern
                break

        if failed_pattern:
            log_failure(name, f"Generated interface missing pattern: '{failed_pattern}'")
        elif forbidden_hit:
            log_failure(name, f"Generated interface unexpectedly contained pattern: '{forbidden_hit}'")
        else:
            log_success(name)


def run_import_via_interface_tests():
    print("\n🚀 Running import-via-interface tests...")
    for path in sorted(glob.glob(os.path.join(FIXTURES_DIR, "import_via_interface", "*", "consumer.lain"))):
        name = os.path.basename(os.path.dirname(path))
        case_dir = os.path.dirname(path)
        out_c = "/tmp/lain_test_import_interface.c"
        interface_files = []

        try:
            deps = sorted(
                p for p in glob.glob(os.path.join(case_dir, "*.lain"))
                if os.path.basename(p) != "consumer.lain"
            )
            for dep in deps:
                dep_lci = dep + ".lci"
                interface_files.append(dep_lci)
                emit_res = subprocess.run([COMPILER_BIN, "--emit-interface", dep, dep_lci], env=ENV, capture_output=True, text=True)
                if emit_res.returncode != 0:
                    log_failure(name, f"Interface emission failed for {os.path.basename(dep)}.\n{emit_res.stderr}")
                    break
            else:
                compile_res = subprocess.run([COMPILER_BIN, path, out_c], env=ENV, capture_output=True, text=True)
                if compile_res.returncode != 0:
                    log_failure(name, f"Consumer compilation failed.\n{compile_res.stderr}")
                else:
                    log_success(name)

        finally:
            for interface_file in interface_files:
                if os.path.exists(interface_file):
                    os.remove(interface_file)


def run_build_tests():
    print("\nSkipping legacy C-side build driver tests.")
    print("Build orchestration belongs in build.lain / meta libraries, not the C compiler driver.")


# D. run-pass: 编译成 C -> 用 gcc 编译 -> 运行并验证退出码
def run_pass_tests():
    print("\n🚀 Running run-pass (execution) tests...")
    for path in sorted(glob.glob(os.path.join(FIXTURES_DIR, "run-pass", "*.lain"))):
        name = os.path.basename(path)
        out_c = "/tmp/lain_test_out.c"
        out_bin = "/tmp/lain_test_bin"

        # 0. 读取源码注释中的预期值
        expected = parse_source_annotations(path)

        # 1. 编译为 C
        res = subprocess.run([COMPILER_BIN, path, out_c], env=ENV, capture_output=True, text=True)
        if res.returncode != 0:
            log_failure(name, f"Lain compilation failed.\n{res.stderr}")
            continue

        # 2. 用系统 gcc 编译这个 C 文件并链接运行时
        runtime_c = os.path.join(WORKSPACE_ROOT, "runtime", "lain_executor.c")
        gcc_res = subprocess.run(
            ["gcc", out_c, runtime_c, "-lm", "-ldl", "-o", out_bin],
            capture_output=True, text=True
        )
        if gcc_res.returncode != 0:
            log_failure(name, f"GCC compilation failed.\n{gcc_res.stderr}")
            continue

        # 3. 运行并验证 (capture as bytes to handle potential binary output)
        run_res = subprocess.run([out_bin], capture_output=True)
        stdout_text = run_res.stdout.decode('utf-8', errors='replace')
        stderr_text = run_res.stderr.decode('utf-8', errors='replace')

        failures = []
        if run_res.returncode != expected["exit"]:
            failures.append(f"exit code: expected {expected['exit']}, got {run_res.returncode}")
        if expected["stdout"] is not None and expected["stdout"] not in stdout_text:
            failures.append(f"stdout missing: '{expected['stdout']}'")
        if expected["stderr"] is not None and expected["stderr"] not in stderr_text:
            failures.append(f"stderr missing: '{expected['stderr']}'")

        if failures:
            log_failure(name, "; ".join(failures))
        else:
            log_success(name)


# ── 3. 主干控制 ──────────────────────────────────────────────────────

if __name__ == "__main__":
    if not os.path.exists(COMPILER_BIN):
        print(f"Error: Compiler binary not found at {COMPILER_BIN}. Please build it first.")
        sys.exit(1)

    run_check_pass()
    run_ui_tests()
    run_codegen_tests()
    run_interface_tests()
    run_import_via_interface_tests()
    run_build_tests()
    run_pass_tests()

    print(f"\n📊 Test Suite Summary:")
    print(f"   Passed: \033[92m{passed_tests}\033[0m")
    print(f"   Failed: \033[91m{failed_tests}\033[0m")

    if failed_tests > 0:
        sys.exit(1)
    else:
        sys.exit(0)
