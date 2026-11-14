#include "sfs/codegen/suppressions.h"

#include "sfs/codegen/attributes.h"
#include "sfs/codegen/parser.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace sfs::codegen
{

namespace
{

constexpr std::string_view kKeyword = "sfs-ignore";

bool isNameChar(char c)
{
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
}

bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// The tree-sitter point of `byte`, counted from a known point before it.
TSPoint
pointOf(std::string_view source, uint32_t fromByte, TSPoint from, uint32_t byte)
{
  for (uint32_t i = fromByte; i < byte; ++i)
  {
    if (source[i] == '\n')
    {
      ++from.row;
      from.column = 0;
    }
    else
      ++from.column;
  }
  return from;
}

// Reads one comment's directive and records what it hides.
class DirectiveReader
{
public:
  DirectiveReader(std::string_view source,
                  const std::filesystem::path& path,
                  std::vector<Diagnostic>& diagnostics)
      : m_source(source), m_path(path), m_diagnostics(diagnostics)
  {
  }

  void read(const IgnoreDirective& directive, Suppressions& out)
  {
    m_start = directive.commentStart;
    m_startPoint = {directive.comment.startRow, directive.comment.startColumn};
    const uint32_t end = directive.commentEnd;

    if (directive.misspelt)
    {
      misspelt(directive);
      return;
    }
    if (directive.open == 0)
    {
      warn(directive.comment,
           "This sfs-ignore comment names no rule. Name the rules it hides, "
           "as in sfs-ignore(own-entity).");
      return;
    }
    if (directive.close == 0)
    {
      warn(directive.comment,
           "This sfs-ignore comment's rule list isn't closed. Add ')' after "
           "the last rule.");
      return;
    }

    RuleSet named;
    bool any = false;
    uint32_t item = directive.open + 1;
    while (item <= directive.close)
    {
      uint32_t itemEnd = item;
      while (itemEnd < directive.close && m_source[itemEnd] != ',')
        ++itemEnd;
      uint32_t first = item;
      uint32_t last = itemEnd;
      while (first < last && isBlank(m_source[first]))
        ++first;
      while (last > first && isBlank(m_source[last - 1]))
        --last;
      if (first < last)
      {
        any = true;
        readRule(
            m_source.substr(first, last - first), range(first, last), named);
      }
      item = itemEnd + 1;
    }
    if (!any)
    {
      warn(directive.comment,
           "This sfs-ignore comment names no rule. Name the rules it hides, "
           "as in sfs-ignore(own-entity).");
      return;
    }

    if (directive.form == IgnoreForm::File)
    {
      out.file |= named;
      return;
    }
    if (directive.form == IgnoreForm::NextLine)
    {
      out.lines.emplace_back(directive.comment.endRow + 1, named);
      return;
    }
    if (aloneOnItsLine(m_start, end))
    {
      const TSPoint at =
          pointOf(m_source, m_start, m_startPoint, directive.nameEnd);
      warn(directive.comment,
           "This sfs-ignore comment covers only its own line, which has no "
           "code. Use sfs-ignore-next-line instead.",
           "Change to sfs-ignore-next-line",
           {{{at.row, at.column, at.row, at.column}, "-next-line"}});
      return;
    }
    out.lines.emplace_back(m_startPoint.row, named);
  }

private:
  // A directive name that's none of the three, with a did-you-mean when one
  // is close.
  void misspelt(const IgnoreDirective& directive)
  {
    const std::string_view written = m_source.substr(
        directive.nameStart, directive.nameEnd - directive.nameStart);
    const SourceRange where = range(directive.nameStart, directive.nameEnd);
    std::string message = "There is no directive " + std::string(written) + ".";
    std::string_view closest;
    std::size_t best = 3;
    for (const IgnoreForm form :
         {IgnoreForm::Line, IgnoreForm::NextLine, IgnoreForm::File})
    {
      const std::size_t distance = editDistance(written, ignoreFormName(form));
      if (distance < best)
      {
        best = distance;
        closest = ignoreFormName(form);
      }
    }
    if (closest.empty())
    {
      warn(where,
           message + " Use sfs-ignore, sfs-ignore-next-line or "
                     "sfs-ignore-file.");
      return;
    }
    message += " Use " + std::string(closest) + " if that's what you meant.";
    warn(where,
         std::move(message),
         "Change to " + std::string(closest),
         {{where, std::string(closest)}});
  }

  SourceRange range(uint32_t from, uint32_t to) const
  {
    const TSPoint start = pointOf(m_source, m_start, m_startPoint, from);
    const TSPoint end = pointOf(m_source, from, start, to);
    return {start.row, start.column, end.row, end.column};
  }

  void warn(const SourceRange& where,
            std::string message,
            std::string fixTitle = {},
            std::vector<TextEdit> fix = {})
  {
    m_diagnostics.push_back({RuleId::BadIgnore,
                             m_path,
                             where,
                             std::move(message),
                             std::move(fixTitle),
                             std::move(fix)});
  }

  void readRule(std::string_view name, const SourceRange& where, RuleSet& named)
  {
    const RuleInfo* rule = findRule(name);
    if (!rule)
    {
      std::string message =
          "There is no rule " + std::string(name) + " to ignore.";
      if (const RuleInfo* closest = closestRule(name))
      {
        message +=
            " Use " + std::string(closest->name) + " if that's what you meant.";
        warn(where,
             std::move(message),
             "Change to " + std::string(closest->name),
             {{where, std::string(closest->name)}});
      }
      else
        warn(where, message + " Check its name in the rule table.");
      return;
    }
    if (rule->severity == Severity::Error)
    {
      warn(where,
           "The rule " + std::string(name) +
               " is an error, so it can't be ignored. Fix the code it reports "
               "instead.");
      return;
    }
    if (rule->id == RuleId::BadIgnore)
    {
      warn(where,
           "The rule bad-ignore can't be ignored. Fix the sfs-ignore comment "
           "instead.");
      return;
    }
    named.set(static_cast<std::size_t>(rule->id));
  }

  // Whether nothing but blanks shares the comment's lines with it.
  bool aloneOnItsLine(uint32_t start, uint32_t end) const
  {
    for (uint32_t i = start; i > 0 && m_source[i - 1] != '\n'; --i)
      if (!isBlank(m_source[i - 1]))
        return false;
    for (uint32_t i = end; i < m_source.size() && m_source[i] != '\n'; ++i)
      if (!isBlank(m_source[i]))
        return false;
    return true;
  }

  std::string_view m_source;
  const std::filesystem::path& m_path;
  std::vector<Diagnostic>& m_diagnostics;
  uint32_t m_start = 0;
  TSPoint m_startPoint{};
};

} // namespace

std::string_view ignoreFormName(IgnoreForm form)
{
  switch (form)
  {
  case IgnoreForm::Line:
    return "sfs-ignore";
  case IgnoreForm::NextLine:
    return "sfs-ignore-next-line";
  case IgnoreForm::File:
    return "sfs-ignore-file";
  }
  return "sfs-ignore";
}

bool readIgnoreDirective(std::string_view source,
                         uint32_t commentStart,
                         uint32_t commentEnd,
                         IgnoreDirective& directive)
{
  std::string_view text =
      source.substr(commentStart, commentEnd - commentStart);
  const bool block = text.starts_with("/*");
  if (!block && !text.starts_with("//"))
    return false;
  if (block && text.ends_with("*/") && text.size() >= 4)
    text.remove_suffix(2);
  std::size_t at = 2;
  while (at < text.size() && (isBlank(text[at]) || text[at] == '/' ||
                              text[at] == '*' || text[at] == '!'))
    ++at;
  if (text.substr(at, kKeyword.size()) != kKeyword)
    return false;
  directive.nameStart = commentStart + static_cast<uint32_t>(at);
  directive.misspelt = false;
  at += kKeyword.size();

  directive.form = IgnoreForm::Line;
  if (text.substr(at).starts_with("-next-line"))
  {
    directive.form = IgnoreForm::NextLine;
    at += std::string_view("-next-line").size();
  }
  else if (text.substr(at).starts_with("-file"))
  {
    directive.form = IgnoreForm::File;
    at += std::string_view("-file").size();
  }
  // A longer word: a misspelt name when a list follows it
  // (sfs-ignore-nextline(...)), else another word ("sfs-ignored").
  if (at < text.size() && isNameChar(text[at]))
  {
    while (at < text.size() && isNameChar(text[at]))
      ++at;
    std::size_t open = at;
    while (open < text.size() && isBlank(text[open]))
      ++open;
    if (open >= text.size() || text[open] != '(')
      return false;
    directive.misspelt = true;
  }
  directive.nameEnd = commentStart + static_cast<uint32_t>(at);

  directive.open = 0;
  directive.close = 0;
  while (at < text.size() && isBlank(text[at]))
    ++at;
  // Without its list, it's a directive only when nothing but a reason follows:
  // in "sfs-ignore comments hide warnings" it's a word in a sentence.
  if (at >= text.size() || text[at] == ':' || text[at] == '\n')
    return true;
  if (text[at] != '(')
    return false;
  directive.open = commentStart + static_cast<uint32_t>(at);
  const std::size_t close = text.find(')', at);
  if (close != std::string_view::npos)
    directive.close = commentStart + static_cast<uint32_t>(close);
  return true;
}

std::vector<IgnoreDirective> findIgnoreDirectives(const TSTree* tree,
                                                  std::string_view source)
{
  std::vector<IgnoreDirective> found;
  if (!tree)
    return found;
  const TSNode root = ts_tree_root_node(tree);
  std::size_t at = source.find(kKeyword);
  while (at != std::string_view::npos)
  {
    const uint32_t byte = static_cast<uint32_t>(at);
    const TSNode node = ts_node_descendant_for_byte_range(
        root, byte, byte + static_cast<uint32_t>(kKeyword.size()));
    std::size_t next = at + kKeyword.size();
    if (!ts_node_is_null(node) &&
        std::strcmp(ts_node_type(node), "comment") == 0)
    {
      IgnoreDirective directive;
      const uint32_t start = ts_node_start_byte(node);
      const uint32_t end = ts_node_end_byte(node);
      if (readIgnoreDirective(source, start, end, directive))
      {
        directive.comment = rangeOf(node);
        directive.commentStart = start;
        directive.commentEnd = end;
        found.push_back(directive);
      }
      next = std::max<std::size_t>(next, end);
    }
    at = source.find(kKeyword, next);
  }
  return found;
}

Suppressions findSuppressions(const TSTree* tree,
                              std::string_view source,
                              const std::filesystem::path& path,
                              std::vector<Diagnostic>& diagnostics)
{
  Suppressions out;
  DirectiveReader reader(source, path, diagnostics);
  for (const IgnoreDirective& directive : findIgnoreDirectives(tree, source))
    reader.read(directive, out);

  std::sort(out.lines.begin(),
            out.lines.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  // One entry per row: comments for the same line add up.
  std::size_t kept = 0;
  for (std::size_t i = 0; i < out.lines.size(); ++i)
  {
    if (kept > 0 && out.lines[kept - 1].first == out.lines[i].first)
      out.lines[kept - 1].second |= out.lines[i].second;
    else
      out.lines[kept++] = out.lines[i];
  }
  out.lines.resize(kept);
  return out;
}

bool isSuppressed(const Suppressions& suppressions,
                  const Diagnostic& diagnostic)
{
  if (!canIgnore(diagnostic.rule))
    return false;
  const std::size_t bit = static_cast<std::size_t>(diagnostic.rule);
  if (suppressions.file.test(bit))
    return true;
  const auto line = std::lower_bound(suppressions.lines.begin(),
                                     suppressions.lines.end(),
                                     diagnostic.range.startRow,
                                     [](const auto& entry, unsigned row)
                                     { return entry.first < row; });
  return line != suppressions.lines.end() &&
         line->first == diagnostic.range.startRow && line->second.test(bit);
}

void applySuppressions(const Suppressions& suppressions,
                       std::vector<Diagnostic>& diagnostics)
{
  if (suppressions.file.none() && suppressions.lines.empty())
    return;
  std::erase_if(diagnostics,
                [&](const Diagnostic& diagnostic)
                { return isSuppressed(suppressions, diagnostic); });
}

} // namespace sfs::codegen
