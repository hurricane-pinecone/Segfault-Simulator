#pragma once

#include <tree_sitter/api.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// What the formatter's house passes share: a text's lines, the rows a node is
// on, and the rendering of rows with some replaced.
namespace sfs::lsp::format
{

/** A text's lines, each with its line break. */
class LineIndex
{
public:
  /** Indexes `text`, which must outlive the index. */
  explicit LineIndex(std::string_view text);

  /** Whether line `row` holds only whitespace. */
  bool blank(uint32_t row) const;

  /** How many lines there are (a last line without a break counts). */
  uint32_t count() const { return static_cast<uint32_t>(m_starts.size()) - 1; }

  /** The whitespace line `row` starts with. */
  std::string_view indent(uint32_t row) const;

  /** Line `row` with its line break; empty past the end. */
  std::string_view line(uint32_t row) const;

  /** Whether line `row` holds only whitespace before byte column `column`. */
  bool startsAt(uint32_t row, uint32_t column) const;

  /** The text indexed. */
  std::string_view text() const { return m_text; }

private:
  std::vector<uint32_t> m_starts; // each line's first byte, then the text's end
  std::string_view m_text;
};

/** The row a node starts on. */
inline uint32_t firstRow(TSNode node) { return ts_node_start_point(node).row; }

/**
 * The last row a node's text is on: a node that ends at the start of a row
 * (a preprocessor line takes its line break) ends on the row before.
 */
uint32_t lastRow(TSNode node);

/** A node's text; empty for a null node. */
std::string_view nodeText(TSNode node, std::string_view text);

/** "\r\n" when the text's first line break is one, else "\n". */
std::string_view lineBreakOf(std::string_view text);

/** A child of `node` by field name; null when it has none. */
TSNode field(TSNode node, const char* name);

/**
 * Whether a subtree has a parse error that may cut it in the wrong places: an
 * ERROR node or a missing token. A missing name (tree-sitter's reading of a
 * default argument `= {}`) never counts. Unless `strict`, errors inside
 * function bodies don't count either (a body's text moves whole), but for a
 * missing `}`, which means the body may end in the wrong place.
 */
bool parsesBadly(TSNode node, bool strict);

/** Whether `node` is of the type named `type`. */
bool isType(TSNode node, std::string_view type);

/**
 * Rows of a text with some ranges of rows replaced: what one pass builds as it
 * reorders nested bodies innermost first. A replacement covering others takes
 * their place.
 */
class Rewrite
{
public:
  /** @param lines the original text's lines */
  explicit Rewrite(const LineIndex& lines) : m_lines(lines) {}

  /** Whether anything was replaced. */
  bool changed() const { return !m_replaced.empty(); }

  /**
   * Rows `first` to `last` (inclusive) as they now read, appended to `out`.
   */
  void render(uint32_t first, uint32_t last, std::string& out) const;

  /** Replaces rows `first` to `last` (inclusive) with `text` (whole lines). */
  void replace(uint32_t first, uint32_t last, std::string text);

private:
  struct Replacement
  {
    uint32_t last;
    std::string text;
  };

  const LineIndex& m_lines;
  std::map<uint32_t, Replacement> m_replaced; // by first row; never overlapping
};

} // namespace sfs::lsp::format
