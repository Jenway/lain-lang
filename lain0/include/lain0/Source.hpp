#ifndef LAIN0_SOURCE_HPP
#define LAIN0_SOURCE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ln {

// 位置以 1 起始的 line/col 表示；Span 以字节偏移表示。
struct Pos {
  uint32_t line = 1;
  uint32_t col = 1;
};

// file == 0 表示无效/空位置。
struct Span {
  uint32_t file = 0;
  uint32_t begin = 0; // 字节偏移，含
  uint32_t end = 0;   // 字节偏移，不含

  bool valid() const { return file != 0; }
};

// 持有源文件缓冲与行首表。diagnostic 的展示全靠它。
class SourceMgr {
public:
  // file id 从 1 起；0 保留为无效。失败返回 0 并填 err。
  uint32_t loadFile(const std::string &path, std::string &err);
  // 已经握有文本的场合（测试用）。
  uint32_t addBuffer(std::string path, std::string text);

  std::string_view text(uint32_t file) const;
  const std::string &path(uint32_t file) const;
  uint32_t size(uint32_t file) const;

  // 字节偏移 -> 行/列（均为 1 起始）
  Pos position(uint32_t file, uint32_t offset) const;
  // 取第 line 行的整行文本（不含换行符），行号 1 起始
  std::string_view lineText(uint32_t file, uint32_t line) const;

private:
  struct Entry {
    std::string path;
    std::string text;
    std::vector<uint32_t> lineStarts; // lineStarts[i] = 第 i+1 行的起始偏移
  };
  const Entry &get(uint32_t file) const;
  std::vector<Entry> files_; // files_[i] 对应 file id = i+1
};

} // namespace ln

#endif
