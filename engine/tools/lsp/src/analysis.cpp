#include "analysis.h"

#include "lints.h"
#include "projectIndex.h"

#include <sfs/codegen/parser.h>
#include <sfs/codegen/suppressions.h>

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstring>

namespace sfs::lsp
{

bool isCodegenHeader(const Document& document, const ProjectIndex& index)
{
  if (!isHeader(document.path))
    return false;
  if (index.isScriptHeader(document.path))
    return true;
  if (!document.tree)
    return false;
  constexpr std::string_view marker = "[[sfs::";
  const TSNode root = ts_tree_root_node(document.tree.get());
  for (std::size_t at = document.text.find(marker); at != std::string::npos;
       at = document.text.find(marker, at + marker.size()))
  {
    const auto byte = static_cast<uint32_t>(at);
    const TSNode node = ts_node_descendant_for_byte_range(root, byte, byte + 2);
    const char* type = ts_node_type(node);
    if (std::strcmp(type, "comment") != 0 &&
        std::strstr(type, "string") == nullptr)
      return true;
  }
  return false;
}

void analyse(Document& document,
             TSParser* parser,
             Linter& linter,
             const ProjectIndex& index,
             const codegen::RuleSet& disabled,
             codegen::ParseResult* parsed,
             const FileReader* read)
{
  document.dirty = false;
  document.diagnostics.clear();
  document.suppressions = {};
  if (document.text.size() > kMaxAnalysedSize)
    return;
  reparse(document, parser);

  document.role = isCodegenHeader(document, index) ? FileRole::CodegenHeader
                  : isHeader(document.path)        ? FileRole::Header
                                                   : FileRole::Source;
  if (document.role == FileRole::CodegenHeader)
  {
    codegen::ParseResult result =
        codegen::parseHeader(document.text, document.path, document.tree.get());
    document.diagnostics = std::move(result.diagnostics);
    document.suppressions = std::move(result.suppressions);
    if (parsed)
      *parsed = std::move(result);
  }
  else
    document.suppressions = codegen::findSuppressions(document.tree.get(),
                                                      document.text,
                                                      document.path,
                                                      document.diagnostics);

  linter.run(document.text,
             document.tree.get(),
             document.path,
             index,
             document.diagnostics,
             read);
  codegen::applySuppressions(document.suppressions, document.diagnostics);
  if (disabled.any())
    std::erase_if(
        document.diagnostics,
        [&](const codegen::Diagnostic& diagnostic)
        {
          return codegen::canIgnore(diagnostic.rule) &&
                 disabled.test(static_cast<std::size_t>(diagnostic.rule));
        });
  std::stable_sort(
      document.diagnostics.begin(),
      document.diagnostics.end(),
      [](const codegen::Diagnostic& a, const codegen::Diagnostic& b)
      {
        return std::tie(a.range.startRow, a.range.startColumn) <
               std::tie(b.range.startRow, b.range.startColumn);
      });
}

} // namespace sfs::lsp
