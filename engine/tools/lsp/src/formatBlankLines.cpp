#include "formatBlankLines.h"

#include "formatLines.h"

#include <algorithm>
#include <vector>

namespace sfs::lsp
{

namespace
{

using format::firstRow;
using format::isType;
using format::lastRow;
using format::LineIndex;
using format::parsesBadly;

bool isControl(TSNode node)
{
  const std::string_view type = ts_node_type(node);
  return type == "if_statement" || type == "for_statement" ||
         type == "for_range_loop" || type == "while_statement" ||
         type == "do_statement" || type == "switch_statement";
}

bool isPreprocessor(TSNode node)
{
  return std::string_view(ts_node_type(node)).starts_with("preproc_");
}

// The rows a blank line goes above, for one block's or case's statements.
void inspectList(TSNode list,
                 bool isCase,
                 const LineIndex& lines,
                 std::vector<uint32_t>& rows)
{
  std::vector<TSNode> items;
  const uint32_t count = ts_node_child_count(list);

  for (uint32_t i = 0; i < count; ++i)
  {
    const TSNode child = ts_node_child(list, i);

    if (!ts_node_is_named(child))
      continue;

    // A case's value isn't one of its statements.
    if (isCase)
      if (const char* name = ts_node_field_name_for_child(list, i);
          name && std::string_view(name) == "value")
        continue;

    items.push_back(child);
  }

  for (std::size_t i = 0; i < items.size(); ++i)
  {
    const TSNode statement = items[i];

    if (!isControl(statement) || parsesBadly(statement, true))
      continue;

    // Above: above the comments directly on top of it, each on its own line.
    uint32_t top = firstRow(statement);
    std::size_t before = i;

    while (before > 0)
    {
      const TSNode comment = items[before - 1];

      if (!isType(comment, "comment") || lastRow(comment) + 1 != top ||
          !lines.startsAt(
              firstRow(comment), ts_node_start_point(comment).column))
        break;

      top = firstRow(comment);
      --before;
    }

    if (before > 0)
    {
      const TSNode previous = items[before - 1];

      if (!isPreprocessor(previous) && !parsesBadly(previous, true) &&
          lastRow(previous) + 1 == top)
        rows.push_back(top);
    }

    // Below: when something follows in the list, after a trailing comment.
    uint32_t bottom = lastRow(statement);
    std::size_t after = i + 1;

    if (after < items.size() && isType(items[after], "comment") &&
        firstRow(items[after]) == bottom)
    {
      bottom = lastRow(items[after]);
      ++after;
    }

    if (after < items.size())
    {
      const TSNode next = items[after];

      if (!isPreprocessor(next) && !parsesBadly(next, true) &&
          firstRow(next) == bottom + 1 &&
          lines.startsAt(firstRow(next), ts_node_start_point(next).column))
        rows.push_back(firstRow(next));
    }
  }
}

void walk(TSNode node, const LineIndex& lines, std::vector<uint32_t>& rows)
{
  const std::string_view type = ts_node_type(node);

  if (type == "ERROR" || ts_node_is_missing(node))
    return;

  if (type == "compound_statement" || type == "case_statement")
    inspectList(node, type == "case_statement", lines, rows);

  const uint32_t count = ts_node_named_child_count(node);

  for (uint32_t i = 0; i < count; ++i)
    walk(ts_node_named_child(node, i), lines, rows);
}

} // namespace

std::string addControlBlankLines(std::string_view text,
                                 const TSTree* tree,
                                 uint32_t firstRow,
                                 uint32_t lastRow)
{
  const LineIndex lines(text);
  std::vector<uint32_t> rows;
  walk(ts_tree_root_node(tree), lines, rows);
  std::sort(rows.begin(), rows.end());
  rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
  std::erase_if(
      rows, [&](uint32_t row) { return row < firstRow || row > lastRow; });

  if (rows.empty())
    return std::string(text);

  const std::string_view lineBreak = format::lineBreakOf(text);
  std::string out;
  out.reserve(text.size() + rows.size() * lineBreak.size());
  std::size_t next = 0;

  for (uint32_t row = 0; row < lines.count(); ++row)
  {
    if (next < rows.size() && rows[next] == row)
    {
      out += lineBreak;
      ++next;
    }

    out += lines.line(row);
  }

  return out;
}

} // namespace sfs::lsp
