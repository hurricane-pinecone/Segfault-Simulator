#include "formatLines.h"

#include <tree_sitter/api.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace sfs::lsp::format
{

LineIndex::LineIndex(std::string_view text) : m_text(text)
{
  m_starts.push_back(0);

  for (uint32_t i = 0; i < text.size(); ++i)
    if (text[i] == '\n')
      m_starts.push_back(i + 1);

  if (m_starts.back() != text.size())
    m_starts.push_back(static_cast<uint32_t>(text.size()));
}

bool LineIndex::blank(uint32_t row) const
{
  for (const char c : line(row))
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
      return false;

  return true;
}

std::string_view LineIndex::indent(uint32_t row) const
{
  const std::string_view text = line(row);
  std::size_t end = 0;

  while (end < text.size() && (text[end] == ' ' || text[end] == '\t'))
    ++end;

  return text.substr(0, end);
}

std::string_view LineIndex::line(uint32_t row) const
{
  if (row + 1 >= m_starts.size())
    return {};

  return m_text.substr(m_starts[row], m_starts[row + 1] - m_starts[row]);
}

bool LineIndex::startsAt(uint32_t row, uint32_t column) const
{
  const std::string_view text = line(row);

  for (uint32_t i = 0; i < column && i < text.size(); ++i)
    if (text[i] != ' ' && text[i] != '\t')
      return false;

  return true;
}

uint32_t lastRow(TSNode node)
{
  const TSPoint start = ts_node_start_point(node);
  const TSPoint end = ts_node_end_point(node);
  return end.column == 0 && end.row > start.row ? end.row - 1 : end.row;
}

std::string_view nodeText(TSNode node, std::string_view text)
{
  if (ts_node_is_null(node))
    return {};

  const uint32_t start = ts_node_start_byte(node);
  const uint32_t end = ts_node_end_byte(node);

  if (start >= text.size() || end < start)
    return {};

  return text.substr(start, end - start);
}

std::string_view lineBreakOf(std::string_view text)
{
  const std::size_t at = text.find('\n');
  return at != std::string_view::npos && at > 0 && text[at - 1] == '\r' ? "\r\n"
                                                                        : "\n";
}

namespace
{

bool parsesBadlyIn(TSNode node, bool strict, bool inBody)
{
  if (ts_node_is_missing(node))
    return !ts_node_is_named(node) &&
           (strict || !inBody || ts_node_type(node) == std::string_view("}"));

  if (!ts_node_has_error(node))
    return false;

  if (ts_node_is_error(node) && (strict || !inBody))
    return true;

  const uint32_t count = ts_node_child_count(node);

  for (uint32_t i = 0; i < count; ++i)
  {
    const TSNode child = ts_node_child(node, i);

    if (parsesBadlyIn(
            child, strict, inBody || isType(child, "compound_statement")))
      return true;
  }

  return false;
}

} // namespace

void Rewrite::render(uint32_t first, uint32_t last, std::string& out) const
{
  auto next = m_replaced.lower_bound(first);

  for (uint32_t row = first; row <= last && row < m_lines.count();)
  {
    while (next != m_replaced.end() && next->first < row)
      ++next;

    if (next != m_replaced.end() && next->first == row &&
        next->second.last <= last)
    {
      out += next->second.text;
      row = next->second.last + 1;
      continue;
    }

    out += m_lines.line(row);
    ++row;
  }
}

void Rewrite::replace(uint32_t first, uint32_t last, std::string text)
{
  m_replaced.erase(m_replaced.lower_bound(first), m_replaced.upper_bound(last));
  m_replaced[first] = Replacement{last, std::move(text)};
}

bool parsesBadly(TSNode node, bool strict)
{
  return !ts_node_is_null(node) && parsesBadlyIn(node, strict, false);
}

TSNode field(TSNode node, const char* name)
{
  if (ts_node_is_null(node))
    return node;

  return ts_node_child_by_field_name(
      node, name, static_cast<uint32_t>(std::strlen(name)));
}

bool isType(TSNode node, std::string_view type)
{
  return !ts_node_is_null(node) && ts_node_type(node) == type;
}

} // namespace sfs::lsp::format
