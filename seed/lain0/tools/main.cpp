
// CLI：lain0 <file.lain> [-o out.l1]
//
// 没有 -o 就把 canonical 文本打到 stdout；失败时诊断打印到 stderr 并返回非 0。
// 编译成功的定义来自 Driver：无诊断错误、verify 通过、文本非空。

#include <fstream>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>

#include "lain0/Driver.hpp"
#include "lain0/Diag.hpp"
#include "lain0/Source.hpp"

namespace {

const char *kUsage =
    "用法: lain0 <file.lain> [-o out.l1] [--dump-tokens] [--dump-ast]\n"
    "      [--round-trip]\n"
    "      lain0 --unit <清单> [-o out.l1] [--round-trip]\n";

// 把文本送到文件或 stdout。写文件失败要报出来并失败退出 ——
// 静默丢掉产物等同于伪造成功。
bool emit(const std::string &text, const std::string &outPath) {
  if (outPath.empty()) {
    std::cout << text;
    std::cout.flush();
    return !std::cout.fail();
  }
  std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "lain0: 无法打开输出文件 " << outPath << "\n";
    return false;
  }
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.flush();
  if (!out) {
    std::cerr << "lain0: 写入输出文件失败 " << outPath << "\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  std::string input{};
  std::string unitPath{};
  std::string outPath{};
  ln::CompileOptions opts{};
  bool roundTrip = false;
  bool help = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a(argv[i]);
    if (a == "-o" || a == "--output") {
      if (i + 1 >= argc) {
        std::cerr << "lain0: -o 缺少参数\n" << kUsage;
        return 2;
      }
      outPath = argv[++i];
    } else if (a.starts_with("-o") && a.size() > 2) {
      outPath = a.substr(2);
    } else if (a == "--unit") {
      if (i + 1 >= argc) {
        std::cerr << "lain0: --unit 缺少参数\n" << kUsage;
        return 2;
      }
      unitPath = argv[++i];
    } else if (a == "--dump-tokens") {
      opts.dumpTokens = true;
    } else if (a == "--dump-ast") {
      opts.dumpAst = true;
    } else if (a == "--round-trip") {
      roundTrip = true;
    } else if (a == "-h" || a == "--help") {
      help = true;
    } else if (a.starts_with("-")) {
      std::cerr << "lain0: 未知选项 " << a << "\n" << kUsage;
      return 2;
    } else if (input.empty()) {
      input = a;
    } else {
      std::cerr << "lain0: 多余的输入文件 " << a << "\n" << kUsage;
      return 2;
    }
  }

  if (help) {
    std::cout << kUsage;
    return 0;
  }
  // §3.7：--unit 与位置参数互斥。两者都给或都不给都是用法错误（退出码 2）。
  const bool hasUnit = !unitPath.empty();
  if (hasUnit != input.empty()) {
    std::cerr << (hasUnit ? "lain0: --unit 与输入文件互斥，只能给一个\n"
                          : "lain0: 缺少输入文件\n")
              << kUsage;
    return 2;
  }

  ln::SourceMgr src;
  ln::DiagSink diags;
  // 清单打不开是用法/IO 错误（退出码 2），不走编译失败的 1。
  const std::string &target = hasUnit ? unitPath : input;
  if (hasUnit) {
    std::ifstream probe(unitPath, std::ios::binary);
    if (!probe) {
      std::cerr << "lain0: 打不开清单 " << unitPath << "\n";
      return 2;
    }
  }
  const ln::CompileResult r = hasUnit
                                  ? ln::compileUnit(unitPath, opts, src, diags)
                                  : ln::compileFile(input, opts, src, diags);

  // 失败路径：诊断一律打印，然后非 0 退出，绝不输出半份产物。
  if (!r.ok) {
    diags.printAll(src, std::cerr);
    // verify 的诊断已经有位置与消息；这里只补一行总结，避免空诊断的沉默失败。
    if (r.verifyCode != 0) {
      std::cerr << "lain0: " << target << ": verify 未通过 (code "
                << r.verifyCode << ")";
      if (!r.verifyMessage.empty()) {
        std::cerr << ": " << r.verifyMessage;
      }
      std::cerr << "\n";
    }
    std::cerr << "lain0: 编译失败：" << target << "\n";
    return 1;
  }

  if (roundTrip) {
    int code = 0;
    std::string msg{};
    if (!ln::roundTripCheck(r.lainir, code, msg)) {
      std::cerr << "lain0: round-trip 失败 (" << code << "): " << msg << "\n";
      return 1;
    }
  }

  if (!emit(r.lainir, outPath)) {
    return 1;
  }
  return 0;
}
