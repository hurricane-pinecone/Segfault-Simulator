#include "lspFeatures.h"

#include <sfs/codegen/attributes.h>
#include <sfs/codegen/parser.h>
#include <sfs/codegen/suppressions.h>

#include <tree_sitter/api.h>

#include <cstring>

namespace sfs::lsp
{

namespace
{

nlohmann::json markdown(const Document& document,
                        const codegen::SourceRange& range,
                        std::string text,
                        const ClientSettings& settings)
{
  return {{"contents", {{"kind", "markdown"}, {"value", std::move(text)}}},
          {"range", rangeJson(document, range, settings.encoding)}};
}

// A rule id under the cursor in an sfs-ignore comment's list.
nlohmann::json ruleHover(const Document& document,
                         TSNode comment,
                         uint32_t cursor,
                         const ClientSettings& settings)
{
  codegen::IgnoreDirective directive;
  if (!codegen::readIgnoreDirective(document.text,
                                    ts_node_start_byte(comment),
                                    ts_node_end_byte(comment),
                                    directive) ||
      directive.open == 0 || cursor <= directive.open ||
      (directive.close != 0 && cursor >= directive.close))
    return nullptr;
  uint32_t from = cursor;
  while (from > directive.open + 1 && isRuleChar(document.text[from - 1]))
    --from;
  uint32_t to = cursor;
  while (to < document.text.size() && isRuleChar(document.text[to]))
    ++to;
  const codegen::RuleInfo* rule = codegen::findRule(
      std::string_view(document.text).substr(from, to - from));
  if (!rule)
    return nullptr;
  const auto [row, start] = pointOf(document, from);
  const auto [endRow, end] = pointOf(document, to);
  return markdown(document,
                  {row, start, endRow, end},
                  "**" + std::string(rule->name) + "** (" +
                      std::string(codegen::severityName(rule->severity)) +
                      ", reported by " +
                      std::string(codegen::toolName(rule->source)) + ")\n\n" +
                      std::string(rule->summary),
                  settings);
}

} // namespace

std::string attributeDocs(const codegen::AttributeInfo& info)
{
  std::string docs(info.summary);
  if (info.target == codegen::AttributeTarget::Type)
    docs += "\n\nOn a type.";
  else if (const std::string types = codegen::attributeFieldTypes(info, "`");
           !types.empty())
    docs += "\n\nFor " + types + " fields.";
  docs += "\n\n```cpp\n";
  docs += info.example;
  docs += "\n```";
  return docs;
}

nlohmann::json hover(const Document& document,
                     Position position,
                     const ClientSettings& settings)
{
  if (!document.tree)
    return nullptr;
  const uint32_t cursor = byteOffset(document, position, settings.encoding);
  const TSNode root = ts_tree_root_node(document.tree.get());
  TSNode node = ts_node_descendant_for_byte_range(root, cursor, cursor + 1);
  if (ts_node_is_null(node))
    return nullptr;
  if (std::strcmp(ts_node_type(node), "comment") == 0)
    return ruleHover(document, node, cursor, settings);

  // Up to the attribute the cursor is in, a few levels at most.
  for (int depth = 0; depth < 4 && !ts_node_is_null(node);
       ++depth, node = ts_node_parent(node))
  {
    if (std::strcmp(ts_node_type(node), "attribute") != 0)
      continue;
    const TSNode prefix = codegen::childField(node, "prefix");
    const TSNode name = codegen::childField(node, "name");
    if (codegen::nodeText(document.text, prefix) != "sfs")
      return nullptr;
    const codegen::AttributeInfo* info =
        codegen::findAttribute(codegen::nodeText(document.text, name));
    if (!info)
      return nullptr;
    std::string docs = "```cpp\n[[sfs::" + std::string(info->name) +
                       std::string(info->arguments) + "]]\n```\n" +
                       attributeDocs(*info);
    return markdown(
        document, codegen::rangeOf(node), std::move(docs), settings);
  }
  return nullptr;
}

} // namespace sfs::lsp
