#include "lspFeatures.h"

#include <sfs/codegen/parser.h>
#include <sfs/codegen/suppressions.h>
#include <sfs/lsp/protocol.h>

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstring>

namespace sfs::lsp
{

using codegen::Diagnostic;
using codegen::IgnoreDirective;
using codegen::IgnoreForm;
using codegen::RuleId;
using codegen::SourceRange;
using codegen::TextEdit;

namespace
{

// The document's well-formed sfs-ignore comments.
std::vector<IgnoreDirective> directivesIn(const Document& document)
{
  std::vector<IgnoreDirective> found =
      codegen::findIgnoreDirectives(document.tree.get(), document.text);
  std::erase_if(found,
                [](const IgnoreDirective& directive)
                { return directive.misspelt || directive.close == 0; });
  return found;
}

// The edit adding `rule` to a directive's list.
TextEdit addToList(const Document& document,
                   const IgnoreDirective& directive,
                   codegen::RuleId rule)
{
  uint32_t before = directive.close;
  while (before > directive.open + 1 && (document.text[before - 1] == ' ' ||
                                         document.text[before - 1] == '\t'))
    --before;
  const bool empty = before == directive.open + 1;
  const auto [row, column] = pointOf(document, before);
  return {{row, column, row, column},
          (empty ? "" : ", ") + std::string(codegen::ruleInfo(rule).name)};
}

// A workspace edit of `edits`: the document's own, and those of other files
// (TextEdit::file), each file's ranges converted against its own text (an
// open document's, else the file on disk).
nlohmann::json editsJson(const Document& document,
                         const std::vector<TextEdit>& edits,
                         const ClientSettings& settings,
                         const OpenDocuments& open = {})
{
  // The files in the order their first edit comes; the document first.
  std::vector<std::filesystem::path> files = {{}};
  for (const TextEdit& edit : edits)
    if (std::find(files.begin(), files.end(), edit.file) == files.end())
      files.push_back(edit.file);
  nlohmann::json documentChanges = nlohmann::json::array();
  nlohmann::json changes = nlohmann::json::object();
  for (const std::filesystem::path& file : files)
  {
    const Document* target = &document;
    Document fromDisk;
    if (!file.empty())
    {
      target = open ? open(file) : nullptr;
      if (!target)
      {
        fromDisk.path = file;
        fromDisk.uri = pathToUri(file);
        setText(fromDisk, codegen::readFile(file));
        target = &fromDisk;
      }
    }
    nlohmann::json list = nlohmann::json::array();
    for (const TextEdit& edit : edits)
      if (edit.file == file)
        list.push_back(
            {{"range", rangeJson(*target, edit.range, settings.encoding)},
             {"newText", edit.text}});
    if (list.empty())
      continue;
    // A file that isn't open has no version to check against.
    const nlohmann::json version = target == &fromDisk
                                       ? nlohmann::json()
                                       : nlohmann::json(target->version);
    documentChanges.push_back(
        {{"textDocument", {{"uri", target->uri}, {"version", version}}},
         {"edits", list}});
    changes[target->uri] = std::move(list);
  }
  if (settings.documentChanges)
    return {{"documentChanges", std::move(documentChanges)}};
  return {{"changes", std::move(changes)}};
}

} // namespace

nlohmann::json diagnosticJson(const Document& document,
                              const Diagnostic& diagnostic,
                              const ClientSettings& settings)
{
  const codegen::RuleInfo& rule = codegen::ruleInfo(diagnostic.rule);
  nlohmann::json json = {
      {"range", rangeJson(document, diagnostic.range, settings.encoding)},
      {"severity", rule.severity == codegen::Severity::Error ? 1 : 2},
      {"code", rule.name},
      {"source",
       rule.source == codegen::RuleSource::Codegen ? "sfs-codegen" : "sfs"},
      {"message", diagnostic.message}};
  if (settings.diagnosticTags && (rule.unnecessary || rule.deprecated))
  {
    nlohmann::json tags = nlohmann::json::array();
    if (rule.unnecessary)
      tags.push_back(1);
    if (rule.deprecated)
      tags.push_back(2);
    json["tags"] = std::move(tags);
  }
  return json;
}

std::vector<TextEdit>
ignoreEdits(const Document& document, RuleId rule, uint32_t row, bool wholeFile)
{
  const std::vector<IgnoreDirective> directives = directivesIn(document);
  const std::string name(codegen::ruleInfo(rule).name);
  if (wholeFile)
  {
    for (const IgnoreDirective& found : directives)
      if (found.form == IgnoreForm::File)
        return {addToList(document, found, rule)};
    // Below #pragma once, or at the top.
    uint32_t line = 0;
    for (uint32_t i = 0; i < document.lineStarts.size(); ++i)
    {
      std::string_view text = lineText(document, i);
      while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
      if (text.starts_with("#pragma once"))
      {
        line = i + 1;
        break;
      }
    }
    if (line >= document.lineStarts.size())
    {
      // #pragma once on the last line, with no line break after it.
      const auto end =
          static_cast<uint32_t>(lineText(document, line - 1).size());
      return {{{line - 1, end, line - 1, end},
               "\n// sfs-ignore-file(" + name + ")"}};
    }
    return {{{line, 0, line, 0}, "// sfs-ignore-file(" + name + ")\n"}};
  }

  for (const IgnoreDirective& found : directives)
    if ((found.form == IgnoreForm::NextLine &&
         found.comment.endRow + 1 == row) ||
        (found.form == IgnoreForm::Line && found.comment.startRow == row))
      return {addToList(document, found, rule)};
  const std::string_view text = lineText(document, row);
  const std::size_t indent = text.find_first_not_of(" \t");
  const std::string_view indentation =
      text.substr(0, indent == std::string_view::npos ? text.size() : indent);
  return {
      {{row, 0, row, 0},
       std::string(indentation) + "// sfs-ignore-next-line(" + name + ")\n"}};
}

nlohmann::json codeActions(const Document& document,
                           Position start,
                           Position end,
                           const ClientSettings& settings,
                           const OpenDocuments& open)
{
  nlohmann::json actions = nlohmann::json::array();
  for (const Diagnostic& diagnostic : document.diagnostics)
  {
    // Any diagnostic on the lines asked about: editors ask at the cursor, which
    // may be anywhere on the line.
    if (diagnostic.range.endRow < start.line ||
        diagnostic.range.startRow > end.line)
      continue;
    const nlohmann::json lspDiagnostic =
        nlohmann::json::array({diagnosticJson(document, diagnostic, settings)});
    if (!diagnostic.fix.empty())
      actions.push_back(
          {{"title", diagnostic.fixTitle.empty() ? "Fix" : diagnostic.fixTitle},
           {"kind", "quickfix"},
           {"diagnostics", lspDiagnostic},
           {"isPreferred", true},
           {"edit", editsJson(document, diagnostic.fix, settings, open)}});
    if (!codegen::canIgnore(diagnostic.rule))
      continue;
    const std::string rule(codegen::ruleInfo(diagnostic.rule).name);
    actions.push_back(
        {{"title", "Ignore " + rule + " on this line"},
         {"kind", "quickfix"},
         {"diagnostics", lspDiagnostic},
         {"edit",
          editsJson(
              document,
              ignoreEdits(
                  document, diagnostic.rule, diagnostic.range.startRow, false),
              settings)}});
    actions.push_back(
        {{"title", "Ignore " + rule + " in this file"},
         {"kind", "quickfix"},
         {"diagnostics", lspDiagnostic},
         {"edit",
          editsJson(document,
                    ignoreEdits(document, diagnostic.rule, 0, true),
                    settings)}});
  }
  return actions;
}

} // namespace sfs::lsp
