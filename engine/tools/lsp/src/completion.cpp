#include "lspFeatures.h"

#include <sfs/codegen/attributes.h>
#include <sfs/codegen/parser.h>
#include <sfs/codegen/suppressions.h>

#include <tree_sitter/api.h>

#include <cstdio>
#include <cstring>

namespace sfs::lsp
{

using codegen::AttributeInfo;
using codegen::AttributeTarget;

namespace
{

bool is(TSNode node, const char* type)
{
  return !ts_node_is_null(node) && std::strcmp(ts_node_type(node), type) == 0;
}

std::string_view textOf(const Document& document, TSNode node)
{
  return codegen::nodeText(document.text, node);
}

std::string_view trimFront(std::string_view text)
{
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  return text;
}

std::string_view trimBack(std::string_view text)
{
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    text.remove_suffix(1);
  return text;
}

bool allOf(std::string_view text, bool (*test)(char))
{
  for (const char c : text)
    if (!test(c))
      return false;
  return true;
}

std::string sortKey(int group, std::size_t index)
{
  char key[16];
  std::snprintf(key, sizeof key, "%d%03zu", group, index);
  return key;
}

nlohmann::json list(nlohmann::json items)
{
  return {{"isIncomplete", false}, {"items", std::move(items)}};
}

// An item whose text replaces [from, cursor) on the cursor's line.
nlohmann::json item(const Document& document,
                    uint32_t from,
                    uint32_t cursor,
                    std::string label,
                    std::string newText,
                    bool snippet,
                    const ClientSettings& settings)
{
  const auto [row, startColumn] = pointOf(document, from);
  const auto [endRow, endColumn] = pointOf(document, cursor);
  return {{"label", label},
          {"filterText", label},
          {"textEdit",
           {{"range",
             rangeJson(document,
                       {row, startColumn, endRow, endColumn},
                       settings.encoding)},
            {"newText", std::move(newText)}}},
          {"insertTextFormat", snippet ? 2 : 1}};
}

// ------------------------------------------------------------- sfs-ignore

nlohmann::json ignoreCompletion(const Document& document,
                                TSNode comment,
                                uint32_t cursor,
                                bool invoked,
                                const ClientSettings& settings)
{
  const uint32_t start = ts_node_start_byte(comment);
  codegen::IgnoreDirective directive;
  if (!codegen::readIgnoreDirective(
          document.text, start, ts_node_end_byte(comment), directive) ||
      directive.open == 0)
  {
    // A comment beginning `sfs-`: the directive names, when asked for.
    std::string_view text =
        std::string_view(document.text).substr(start, cursor - start);
    if (!invoked || text.size() < 2)
      return list(nlohmann::json::array());
    text.remove_prefix(2);
    const std::string_view typed = trimFront(text);
    if (!typed.starts_with("sfs-") ||
        !allOf(typed, [](char c) { return isRuleChar(c); }))
      return list(nlohmann::json::array());
    const auto from = static_cast<uint32_t>(cursor - typed.size());
    nlohmann::json items = nlohmann::json::array();
    std::size_t index = 0;
    for (const codegen::IgnoreForm form : {codegen::IgnoreForm::Line,
                                           codegen::IgnoreForm::NextLine,
                                           codegen::IgnoreForm::File})
    {
      const std::string name(codegen::ignoreFormName(form));
      nlohmann::json entry = item(document,
                                  from,
                                  cursor,
                                  name,
                                  settings.snippets ? name + "($1)" : name,
                                  settings.snippets,
                                  settings);
      entry["kind"] = 14;
      entry["sortText"] = sortKey(0, index++);
      items.push_back(std::move(entry));
    }
    return list(std::move(items));
  }

  if (cursor <= directive.open ||
      (directive.close != 0 && cursor > directive.close))
    return list(nlohmann::json::array());
  uint32_t from = cursor;
  while (from > directive.open + 1 && isRuleChar(document.text[from - 1]))
    --from;
  const std::string_view listed =
      std::string_view(document.text)
          .substr(directive.open + 1,
                  (directive.close ? directive.close : cursor) -
                      directive.open - 1);

  // The rules reported where the comment applies come first.
  const uint32_t row = ts_node_start_point(comment).row;
  const uint32_t target = directive.form == codegen::IgnoreForm::NextLine
                              ? ts_node_end_point(comment).row + 1
                              : row;
  codegen::RuleSet reported;
  for (const codegen::Diagnostic& diagnostic : document.diagnostics)
    if (directive.form == codegen::IgnoreForm::File ||
        diagnostic.range.startRow == target)
      reported.set(static_cast<std::size_t>(diagnostic.rule));

  nlohmann::json items = nlohmann::json::array();
  std::size_t index = 0;
  for (const codegen::RuleInfo& rule : codegen::rules())
  {
    ++index;
    if (!codegen::canIgnore(rule.id) || rule.reserved)
      continue;
    // Skip the ones the list already names.
    const std::size_t at = listed.find(rule.name);
    if (at != std::string_view::npos &&
        (at == 0 || !isRuleChar(listed[at - 1])) &&
        (at + rule.name.size() == listed.size() ||
         !isRuleChar(listed[at + rule.name.size()])) &&
        directive.open + 1 + at + rule.name.size() != cursor)
      continue;
    nlohmann::json entry = item(document,
                                from,
                                cursor,
                                std::string(rule.name),
                                std::string(rule.name),
                                false,
                                settings);
    entry["kind"] = 21;
    entry["detail"] = std::string(codegen::toolName(rule.source)) + " " +
                      std::string(codegen::severityName(rule.severity));
    entry["documentation"] = {{"kind", "markdown"}, {"value", rule.summary}};
    entry["sortText"] = sortKey(
        reported.test(static_cast<std::size_t>(rule.id)) ? 0 : 1, index);
    items.push_back(std::move(entry));
  }
  return list(std::move(items));
}

// ------------------------------------------------------------- attributes

// The class body a position is in, or null.
TSNode classBodyAt(TSNode node)
{
  for (; !ts_node_is_null(node); node = ts_node_parent(node))
  {
    if (is(node, "field_declaration_list"))
      return node;
    if (is(node, "compound_statement") || is(node, "translation_unit"))
      break;
  }
  return TSNode{};
}

bool insideSfs(const Document& document, TSNode node)
{
  for (; !ts_node_is_null(node); node = ts_node_parent(node))
    if (is(node, "namespace_definition"))
    {
      if (codegen::isSfsNamespace(
              textOf(document, codegen::childField(node, "name"))))
        return true;
    }
  return false;
}

// Whether declarations at `cursor` in a class body are public.
bool isPublicAt(const Document& document, TSNode body, uint32_t cursor)
{
  const TSNode type = ts_node_parent(body);
  bool isPublic = !is(type, "class_specifier");
  const uint32_t count = ts_node_named_child_count(body);
  for (uint32_t i = 0; i < count; ++i)
  {
    const TSNode child = ts_node_named_child(body, i);
    if (ts_node_start_byte(child) >= cursor)
      break;
    if (is(child, "access_specifier"))
      isPublic = textOf(document, child) == "public";
  }
  return isPublic;
}

// The field kind of the declaration the cursor's attribute is on, from the
// rest of the line: `]] glm::vec3 tint{1.0f};` gives "Vec3". Empty when the
// type isn't written yet or isn't one the editor edits.
std::string
kindAfter(const Document& document, uint32_t row, uint32_t cursor, bool sfs)
{
  const std::string_view line = lineText(document, row);
  const uint32_t column = cursor - document.lineStarts[row];
  const std::size_t close = line.rfind("]]");
  if (close == std::string_view::npos || close < column)
    return {};
  std::string_view declaration = line.substr(close + 2);
  const std::size_t end = declaration.find_first_of("=;{");
  declaration = trimBack(trimFront(declaration.substr(0, end)));
  // The name is the last word; the type is what's before it.
  std::size_t split = declaration.size();
  while (split > 0 && isIdentifierChar(declaration[split - 1]))
    --split;
  if (split == 0 || split == declaration.size())
    return {};
  return codegen::fieldKindOf(trimBack(declaration.substr(0, split)), sfs);
}

// The sfs attribute names written on the cursor's line, but the one being
// typed.
std::vector<std::string_view>
attributesOnLine(const Document& document, uint32_t row, uint32_t cursor)
{
  std::vector<std::string_view> names;
  const std::string_view line = lineText(document, row);
  const uint32_t lineStart = document.lineStarts[row];
  for (std::size_t at = line.find("sfs::"); at != std::string_view::npos;
       at = line.find("sfs::", at + 5))
  {
    std::size_t end = at + 5;
    while (end < line.size() && isIdentifierChar(line[end]))
      ++end;
    if (lineStart + end != cursor)
      names.push_back(line.substr(at + 5, end - at - 5));
  }
  return names;
}

nlohmann::json showIfCompletion(const Document& document,
                                TSNode body,
                                uint32_t from,
                                uint32_t cursor,
                                const ClientSettings& settings)
{
  nlohmann::json items = nlohmann::json::array();
  if (ts_node_is_null(body))
    return list(std::move(items));
  const uint32_t count = ts_node_named_child_count(body);
  std::size_t index = 0;
  for (uint32_t i = 0; i < count; ++i)
  {
    const TSNode member = ts_node_named_child(body, i);
    if (!is(member, "field_declaration"))
      continue;
    const TSNode type = ts_node_child_by_field_name(member, "type", 4);
    if (ts_node_is_null(type) || textOf(document, type) != "bool")
      continue;
    const uint32_t children = ts_node_named_child_count(member);
    for (uint32_t c = 0; c < children; ++c)
    {
      const TSNode child = ts_node_named_child(member, c);
      if (is(child, "storage_class_specifier") &&
          textOf(document, child) == "static")
        break;
      const char* field = ts_node_field_name_for_named_child(member, c);
      if (!field || std::strcmp(field, "declarator") != 0 ||
          !is(child, "field_identifier"))
        continue;
      const std::string name(textOf(document, child));
      nlohmann::json entry =
          item(document, from, cursor, name, name, false, settings);
      entry["kind"] = 5;
      entry["detail"] = "bool " + name;
      entry["sortText"] = sortKey(0, index++);
      items.push_back(std::move(entry));
    }
  }
  return list(std::move(items));
}

} // namespace

nlohmann::json completion(const Document& document,
                          Position position,
                          bool invoked,
                          const ClientSettings& settings)
{
  if (!document.tree || position.line >= document.lineStarts.size())
    return list(nlohmann::json::array());
  const uint32_t cursor = byteOffset(document, position, settings.encoding);
  const uint32_t lineStart = document.lineStarts[position.line];
  const std::string_view before =
      std::string_view(document.text).substr(lineStart, cursor - lineStart);
  // Cheap checks first: most requests are neither in an attribute nor a
  // comment.
  const std::size_t brackets = before.rfind("[[");
  const bool maybeAttribute =
      brackets != std::string_view::npos &&
      before.find("]]", brackets) == std::string_view::npos;
  const bool maybeComment = before.find("//") != std::string_view::npos ||
                            before.find("/*") != std::string_view::npos ||
                            before.find("sfs-") != std::string_view::npos;
  if (!maybeAttribute && !maybeComment)
    return list(nlohmann::json::array());

  const TSNode root = ts_tree_root_node(document.tree.get());
  const TSNode node = ts_node_descendant_for_byte_range(
      root, cursor > 0 ? cursor - 1 : 0, cursor);
  if (is(node, "comment"))
    return ignoreCompletion(document, node, cursor, invoked, settings);
  if (!maybeAttribute || std::strstr(ts_node_type(node), "string") ||
      std::strstr(ts_node_type(node), "char_literal"))
    return list(nlohmann::json::array());

  // The attribute being typed: what follows the last `[[`, or the last comma
  // between attributes (one inside an attribute's arguments isn't).
  const std::string_view segment = before.substr(brackets + 2);
  std::size_t comma = std::string_view::npos;
  int depth = 0;
  for (std::size_t i = 0; i < segment.size(); ++i)
  {
    if (segment[i] == '(')
      ++depth;
    else if (segment[i] == ')')
      depth = std::max(0, depth - 1);
    else if (segment[i] == ',' && depth == 0)
      comma = i;
  }
  std::string_view typed = trimFront(
      comma == std::string_view::npos ? segment : segment.substr(comma + 1));

  const TSNode body = classBodyAt(node);
  const std::string_view head = trimBack(before.substr(0, brackets));
  const bool onType = head.ends_with("struct") || head.ends_with("class");
  if (!onType && ts_node_is_null(body))
    return list(nlohmann::json::array());

  if (typed.starts_with("sfs::showIf"))
  {
    std::string_view rest = trimFront(typed.substr(11));
    if (!rest.starts_with('('))
      return list(nlohmann::json::array());
    rest = trimFront(rest.substr(1));
    if (rest.starts_with('!'))
      rest = trimFront(rest.substr(1));
    if (!allOf(rest, [](char c) { return isIdentifierChar(c); }))
      return list(nlohmann::json::array());
    return showIfCompletion(document,
                            body,
                            static_cast<uint32_t>(cursor - rest.size()),
                            cursor,
                            settings);
  }

  const bool qualified = typed.starts_with("sfs::");
  const std::string_view partial = qualified ? typed.substr(5) : typed;
  if (!allOf(partial, [](char c) { return isIdentifierChar(c); }))
    return list(nlohmann::json::array());
  const auto from = static_cast<uint32_t>(cursor - partial.size());

  const std::vector<std::string_view> present =
      attributesOnLine(document, position.line, cursor);
  std::string kind;
  bool isPublic = true;
  bool accessKnown = false;
  if (!onType)
  {
    kind =
        kindAfter(document, position.line, cursor, insideSfs(document, body));
    isPublic = isPublicAt(document, body, cursor);
    accessKnown = true;
  }

  nlohmann::json items = nlohmann::json::array();
  std::size_t index = 0;
  for (const AttributeInfo& info : codegen::attributes())
  {
    ++index;
    if ((info.target == AttributeTarget::Type) != onType)
      continue;
    if (std::find(present.begin(), present.end(), info.name) != present.end())
      continue;
    if (!kind.empty() && !codegen::appliesTo(info, kind))
      continue;
    if (accessKnown && ((info.id == codegen::AttributeId::Show && isPublic) ||
                        (info.id == codegen::AttributeId::Hide && !isPublic)))
      continue;
    const std::string name(info.name);
    const std::string label = qualified ? name : "sfs::" + name;
    nlohmann::json entry = item(
        document,
        from,
        cursor,
        label,
        label + (settings.snippets ? std::string(info.snippet) : std::string()),
        settings.snippets && !info.snippet.empty(),
        settings);
    entry["kind"] = 10;
    entry["detail"] = "[[sfs::" + name + std::string(info.arguments) + "]]";
    entry["documentation"] = {
        {"kind", "markdown"}, {"value", attributeDocs(info)}};
    entry["sortText"] = sortKey(0, index);
    items.push_back(std::move(entry));
  }
  return list(std::move(items));
}

} // namespace sfs::lsp
