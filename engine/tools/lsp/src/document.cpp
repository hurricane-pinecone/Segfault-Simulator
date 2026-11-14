#include "document.h"

#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-cpp.h>

#include <algorithm>

namespace sfs::lsp
{

void TreeDeleter::operator()(TSTree* tree) const { ts_tree_delete(tree); }

void ParserDeleter::operator()(TSParser* parser) const
{
  ts_parser_delete(parser);
}

ParserPtr makeCppParser()
{
  ParserPtr parser(ts_parser_new());
  ts_parser_set_language(parser.get(), tree_sitter_cpp());
  return parser;
}

std::vector<uint32_t> countLines(std::string_view text)
{
  std::vector<uint32_t> starts;
  starts.reserve(text.size() / 32 + 1);
  starts.push_back(0);
  for (std::size_t i = 0; i < text.size(); ++i)
    if (text[i] == '\n')
      starts.push_back(static_cast<uint32_t>(i + 1));
  return starts;
}

void setText(Document& document, std::string text)
{
  document.text = std::move(text);
  document.text.reserve(document.text.size() + document.text.size() / 4);
  document.lineStarts = countLines(document.text);
  document.tree.reset();
  document.stale = true;
  document.dirty = true;
  document.lastChange = std::chrono::steady_clock::now();
}

std::string_view lineText(const Document& document, uint32_t line)
{
  if (line >= document.lineStarts.size())
    return {};
  const uint32_t start = document.lineStarts[line];
  uint32_t end = line + 1 < document.lineStarts.size()
                     ? document.lineStarts[line + 1] - 1
                     : static_cast<uint32_t>(document.text.size());
  if (end > start && document.text[end - 1] == '\r')
    --end;
  return std::string_view(document.text).substr(start, end - start);
}

uint32_t byteOffset(const Document& document,
                    Position position,
                    PositionEncoding encoding)
{
  if (position.line >= document.lineStarts.size())
    return static_cast<uint32_t>(document.text.size());
  return document.lineStarts[position.line] +
         characterToByte(
             lineText(document, position.line), position.character, encoding);
}

Position positionOf(const Document& document,
                    uint32_t row,
                    uint32_t byteColumn,
                    PositionEncoding encoding)
{
  return {row, byteToCharacter(lineText(document, row), byteColumn, encoding)};
}

std::pair<uint32_t, uint32_t> pointOf(const Document& document, uint32_t byte)
{
  const auto after = std::upper_bound(
      document.lineStarts.begin(), document.lineStarts.end(), byte);
  const auto row =
      static_cast<uint32_t>(after - document.lineStarts.begin() - 1);
  return {row, byte - document.lineStarts[row]};
}

void applyEdit(Document& document,
               Position start,
               Position end,
               std::string_view text,
               PositionEncoding encoding)
{
  const uint32_t from = byteOffset(document, start, encoding);
  const uint32_t to = std::max(from, byteOffset(document, end, encoding));
  const auto [startRow, startColumn] = pointOf(document, from);
  const auto [oldEndRow, oldEndColumn] = pointOf(document, to);

  const std::size_t size = document.text.size() - (to - from) + text.size();
  if (size > document.text.capacity())
    document.text.reserve(size + size / 4);
  document.text.replace(from, to - from, text);

  // Lines that started inside the replaced text go; the new text's come in;
  // later ones move by the change in length.
  std::vector<uint32_t>& starts = document.lineStarts;
  const auto first = std::upper_bound(starts.begin(), starts.end(), from);
  const auto last = std::upper_bound(first, starts.end(), to);
  const int64_t delta =
      static_cast<int64_t>(text.size()) - static_cast<int64_t>(to - from);
  for (auto it = last; it != starts.end(); ++it)
    *it = static_cast<uint32_t>(*it + delta);
  const std::size_t at = static_cast<std::size_t>(first - starts.begin());
  starts.erase(first, last);
  std::size_t inserted = 0;
  for (std::size_t i = 0; i < text.size(); ++i)
    if (text[i] == '\n')
      ++inserted;
  if (inserted > 0)
  {
    starts.insert(
        starts.begin() + static_cast<std::ptrdiff_t>(at), inserted, 0);
    std::size_t next = at;
    for (std::size_t i = 0; i < text.size(); ++i)
      if (text[i] == '\n')
        starts[next++] = static_cast<uint32_t>(from + i + 1);
  }

  if (document.tree)
  {
    const auto [newEndRow, newEndColumn] =
        pointOf(document, from + static_cast<uint32_t>(text.size()));
    TSInputEdit edit;
    edit.start_byte = from;
    edit.old_end_byte = to;
    edit.new_end_byte = from + static_cast<uint32_t>(text.size());
    edit.start_point = {startRow, startColumn};
    edit.old_end_point = {oldEndRow, oldEndColumn};
    edit.new_end_point = {newEndRow, newEndColumn};
    ts_tree_edit(document.tree.get(), &edit);
  }
  document.stale = true;
  document.dirty = true;
  document.lastChange = std::chrono::steady_clock::now();
}

void reparse(Document& document, TSParser* parser)
{
  if (!document.stale && document.tree)
    return;
  TSTree* tree =
      ts_parser_parse_string(parser,
                             document.tree.get(),
                             document.text.data(),
                             static_cast<uint32_t>(document.text.size()));
  document.tree.reset(tree);
  document.stale = false;
}

nlohmann::json rangeJson(const Document& document,
                         const codegen::SourceRange& range,
                         PositionEncoding encoding)
{
  const Position start =
      positionOf(document, range.startRow, range.startColumn, encoding);
  const Position end =
      positionOf(document, range.endRow, range.endColumn, encoding);
  return {{"start", {{"line", start.line}, {"character", start.character}}},
          {"end", {{"line", end.line}, {"character", end.character}}}};
}

Position positionFrom(const nlohmann::json& position)
{
  if (!position.is_object())
    return {};
  return {position.value("line", 0u), position.value("character", 0u)};
}

bool isHeader(const std::filesystem::path& path)
{
  const std::string extension = path.extension().string();
  return extension == ".h" || extension == ".hh" || extension == ".hpp" ||
         extension == ".hxx";
}

} // namespace sfs::lsp
