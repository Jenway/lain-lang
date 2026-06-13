# tests/runner.py
import os
import sys
import glob
import subprocess
import shutil

# ── 1. 配置路径与编译器指令 ──────────────────────────────────────────

WORKSPACE_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
COMPILER_BIN = os.path.join(WORKSPACE_ROOT, "bootstrap", "bootstrap_l1")
LAINC_BIN = os.path.join(WORKSPACE_ROOT, "compiler", "lainc")

# 配置 Chibi 虚拟机的环境变量，保证测试时加载正确
ENV = os.environ.copy()
ENV["LD_LIBRARY_PATH"] = os.path.join(WORKSPACE_ROOT, "bootstrap", "chibi-scheme")
ENV["CHIBI_MODULE_PATH"] = os.path.join(WORKSPACE_ROOT, "bootstrap", "chibi-scheme", "lib")

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

# ── 2. 四大测试管道实现 ──────────────────────────────────────────────

# A. check-pass: 必须编译成功
def run_check_pass():
    print("\n🚀 Running check-pass tests...")
    for path in glob.glob(os.path.join(FIXTURES_DIR, "check-pass", "*.lain")):
        name = os.path.basename(path)
        out_c = "/tmp/lain_test_out.c"

        res = subprocess.run([COMPILER_BIN, path, out_c], env=ENV, capture_output=True, text=True)
        if res.returncode == 0:
            log_success(name)
        else:
            log_failure(name, f"Compilation failed.\n{res.stderr}")

# B. ui: 必须编译失败（测试编译期诊断）
def run_ui_tests():
    print("\n🚀 Running UI (expect fail) tests...")
    for path in glob.glob(os.path.join(FIXTURES_DIR, "ui", "*.lain")):
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
    for path in glob.glob(os.path.join(FIXTURES_DIR, "codegen", "*.lain")):
        name = os.path.basename(path)
        out_l1 = "/tmp/lain_test_out.l1"

        # 1. 提取源码里所有的 // CHECK: 模式
        expected_patterns = []
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                if "// CHECK:" in line:
                    expected_patterns.append(line.split("// CHECK:")[1].strip())

        # 2. 用 lainc --emit-l1 编译
        res = subprocess.run([LAINC_BIN, "--emit-l1", path, out_l1], env=ENV, capture_output=True, text=True)
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

        if failed_pattern:
            log_failure(name, f"Generated L1 IR missing pattern: '{failed_pattern}'")
        else:
            log_success(name)

# D. run-pass: 编译成 C -> 用 gcc 编译 -> 运行并验证退出码
def run_pass_tests():
    print("\n🚀 Running run-pass (execution) tests...")
    for path in glob.glob(os.path.join(FIXTURES_DIR, "run-pass", "*.lain")):
        name = os.path.basename(path)
        out_c = "/tmp/lain_test_out.c"
        out_bin = "/tmp/lain_test_bin"

        # 1. 编译为 C
        res = subprocess.run([COMPILER_BIN, path, out_c], env=ENV, capture_output=True, text=True)
        if res.returncode != 0:
            log_failure(name, f"Lain compilation failed.\n{res.stderr}")
            continue

        # 2. 用系统 gcc 编译这个 C 文件并链接运行时
        # 注意：需要链接我们 runtime 下的 C 文件
        runtime_c = os.path.join(WORKSPACE_ROOT, "runtime", "lain_executor.c")
        gcc_res = subprocess.run(["gcc", out_c, runtime_c, "-lm", "-ldl", "-o", out_bin], capture_output=True, text=True)
        if gcc_res.returncode != 0:
            log_failure(name, f"GCC compilation failed.\n{gcc_res.stderr}")
            continue

        # 3. 运行并验证退出码是否为 0
        run_res = subprocess.run([out_bin], capture_output=True, text=True)
        if run_res.returncode == 0:
            log_success(name)
        else:
            log_failure(name, f"Execution failed with exit code {run_res.returncode}.\n{run_res.stderr}")

# ── 3. 主干控制 ──────────────────────────────────────────────────────

if __name__ == "__main__":
    if not os.path.exists(COMPILER_BIN):
        print(f"Error: Compiler binary not found at {COMPILER_BIN}. Please build it first.")
        sys.exit(1)

    run_check_pass()
    run_ui_tests()
    run_codegen_tests()
    run_pass_tests()

    print("\n📊 Test Suite Summary:")
    print(f"   Passed: \033[92m{passed_tests}\033[0m")
    print(f"   Failed: \033[91m{failed_tests}\033[0m")

    if failed_tests > 0:
        sys.exit(1)
    else:
        sys.exit(0)
