#include "lain0/Source.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

namespace ln {
namespace {

// 行首表登记每一行的起始偏移；末尾的换行后面仍算一行（EOF 的位置落在那一行），
// 这样 position(size) 不会退化成越界。
void buildLineStarts(std::string_view text, std::vector<uint32_t> &out) {
  out.clear();
  out.push_back(0);
  for (uint32_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') {
      out.push_back(i + 1);
    }
  }
}

} // namespace

const SourceMgr::Entry &SourceMgr::get(uint32_t file) const {
  static const Entry empty{};
  if (file == 0 || file > files_.size()) {
    return empty;
  }
  return files_[file - 1];
}

uint32_t SourceMgr::loadFile(const std::string &path, std::string &err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "无法打开源文件 " + path;
    return 0;
  }
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  if (in.bad()) {
    err = "读取源文件失败 " + path;
    return 0;
  }
  return addBuffer(path, std::move(text));
}

uint32_t SourceMgr::addBuffer(std::string path, std::string text) {
  Entry e;
  e.path = std::move(path);
  e.text = std::move(text);
  buildLineStarts(e.text, e.lineStarts);
  files_.push_back(std::move(e));
  return static_cast<uint32_t>(files_.size());
}

std::string_view SourceMgr::text(uint32_t file) const {
  return get(file).text;
}

const std::string &SourceMgr::path(uint32_t file) const {
  return get(file).path;
}

uint32_t SourceMgr::size(uint32_t file) const {
  return static_cast<uint32_t>(get(file).text.size());
}

Pos SourceMgr::position(uint32_t file, uint32_t offset) const {
  const Entry &e = get(file);
  if (e.lineStarts.empty()) {
    return Pos{};
  }
  // 超出文本的偏移夹到 EOF，避免诊断在文件末尾处拿到垃圾行号。
  const uint32_t off = std::min<uint32_t>(
      offset, static_cast<uint32_t>(e.text.size()));
  auto it = std::upper_bound(e.lineStarts.begin(), e.lineStarts.end(), off);
  if (it == e.lineStarts.begin()) {
    return Pos{};
  }
  --it;
  Pos p;
  p.line = static_cast<uint32_t>(it - e.lineStarts.begin()) + 1;
  p.col = off - *it + 1;
  return p;
}

std::string_view SourceMgr::lineText(uint32_t file, uint32_t line) const {
  const Entry &e = get(file);
  if (line == 0 || line > e.lineStarts.size()) {
    return {};
  }
  const uint32_t begin = e.lineStarts[line - 1];
  uint32_t end = (line < e.lineStarts.size())
                     ? e.lineStarts[line]
                     : static_cast<uint32_t>(e.text.size());
  // 换行符不属于行文本；CRLF 里的 \r 同样要去掉，否则插入符列号对不上显示。
  while (end > begin &&
         (e.text[end - 1] == '\n' || e.text[end - 1] == '\r')) {
    --end;
  }
  return std::string_view(e.text).substr(begin, end - begin);
}

} // namespace ln
