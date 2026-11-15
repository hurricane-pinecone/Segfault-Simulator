#pragma once

// Helpers the sfs-lsp tests share: documents made from text, analysed as the
// server would, and fixes applied to text.

#include "analysis.h"
#include "document.h"
#include "lints.h"
#include "projectIndex.h"

#include <sfs/codegen/rules.h>

#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-cpp.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace lspTest
{

/** A C++ parser for the tests' documents. */
inline TSParser* parser()
{
  static const std::unique_ptr<TSParser, decltype(&ts_parser_delete)> instance(
      []
      {
        TSParser* made = ts_parser_new();
        ts_parser_set_language(made, tree_sitter_cpp());
        return made;
      }(),
      &ts_parser_delete);
  return instance.get();
}

/** A document holding `text`, parsed. */
inline std::unique_ptr<sfs::lsp::Document>
document(std::string text, std::filesystem::path path = "/t/file.cpp")
{
  auto made = std::make_unique<sfs::lsp::Document>();
  made->uri = "file://" + path.generic_string();
  made->path = std::move(path);
  sfs::lsp::setText(*made, std::move(text));
  sfs::lsp::reparse(*made, parser());
  return made;
}

/**
 * `text` analysed as the server would, with an optional index and disabled
 * rules.
 */
inline std::unique_ptr<sfs::lsp::Document>
analysed(std::string text,
         std::filesystem::path path = "/t/file.cpp",
         const sfs::lsp::ProjectIndex* index = nullptr,
         sfs::codegen::RuleSet disabled = {})
{
  static sfs::lsp::Linter linter;
  static const sfs::lsp::ProjectIndex none;
  auto made = document(std::move(text), std::move(path));
  sfs::lsp::analyse(*made, parser(), linter, index ? *index : none, disabled);
  return made;
}

/** How many of a document's diagnostics are of `rule`. */
inline std::size_t count(const sfs::lsp::Document& document,
                         sfs::codegen::RuleId rule)
{
  return static_cast<std::size_t>(std::count_if(
      document.diagnostics.begin(),
      document.diagnostics.end(),
      [&](const sfs::codegen::Diagnostic& d) { return d.rule == rule; }));
}

/** The first diagnostic of `rule`, or null. */
inline const sfs::codegen::Diagnostic* first(const sfs::lsp::Document& document,
                                             sfs::codegen::RuleId rule)
{
  for (const sfs::codegen::Diagnostic& d : document.diagnostics)
    if (d.rule == rule)
      return &d;
  return nullptr;
}

/** `text` with `edits` (source ranges: rows and byte columns) applied. */
inline std::string applyEdits(std::string text,
                              std::vector<sfs::codegen::TextEdit> edits)
{
  const std::vector<uint32_t> starts = sfs::lsp::countLines(text);
  const auto offset = [&](unsigned row, unsigned column)
  {
    return row < starts.size() ? starts[row] + column
                               : static_cast<uint32_t>(text.size());
  };
  std::sort(edits.begin(),
            edits.end(),
            [](const sfs::codegen::TextEdit& a, const sfs::codegen::TextEdit& b)
            {
              return std::tie(a.range.startRow, a.range.startColumn) >
                     std::tie(b.range.startRow, b.range.startColumn);
            });
  for (const sfs::codegen::TextEdit& edit : edits)
  {
    const uint32_t from = offset(edit.range.startRow, edit.range.startColumn);
    const uint32_t to = offset(edit.range.endRow, edit.range.endColumn);
    text.replace(from, to - from, edit.text);
  }
  return text;
}

/** Writes `text` to `path`, making its folders. */
inline void writeFile(const std::filesystem::path& path, std::string_view text)
{
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

} // namespace lspTest
