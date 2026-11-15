#pragma once

#include "document.h"

#include <sfs/codegen/attributes.h>
#include <sfs/codegen/rules.h>

#include <nlohmann/json.hpp>

#include <functional>

// What sfs-lsp answers about a document: completion, hover and code actions.
namespace sfs::lsp
{

/** What the client can take, read from its initialize request. */
struct ClientSettings
{
  /** What positions' characters count. */
  PositionEncoding encoding = PositionEncoding::Utf16;
  /** Completion items may be snippets. */
  bool snippets = false;
  /** Workspace edits may be versioned documentChanges. */
  bool documentChanges = false;
  /** Diagnostics may carry tags (Unnecessary, Deprecated). */
  bool diagnosticTags = false;
  /** Rules the user turned off (initializationOptions.disabledRules). */
  codegen::RuleSet disabledRules;
};

/**
 * The completion items at a position: [[sfs::...]] attributes, showIf's bool
 * fields, and rule ids and directive names inside an sfs-ignore comment.
 *
 * @param document the document, with a current tree
 * @param position where completion was asked for
 * @param invoked asked for by the user rather than by a trigger character
 * @param settings the client's
 * @return an LSP CompletionList; empty where nothing applies
 */
nlohmann::json completion(const Document& document,
                          Position position,
                          bool invoked,
                          const ClientSettings& settings);

/**
 * The hover at a position: an [[sfs::...]] attribute's docs, or a rule's in
 * an sfs-ignore comment.
 *
 * @param document the document, with a current tree
 * @param position where the pointer is
 * @param settings the client's
 * @return an LSP Hover, or null
 */
nlohmann::json hover(const Document& document,
                     Position position,
                     const ClientSettings& settings);

/** Finds an open document by its path; null when it isn't open. */
using OpenDocuments =
    std::function<const Document*(const std::filesystem::path&)>;

/**
 * The quick fixes for the document's diagnostics in a range: each one's fix,
 * then "Ignore <rule> on this line" and "Ignore <rule> in this file". A fix
 * that edits other files too (a class's header) gives a workspace edit with
 * an entry per file.
 *
 * @param document the document, analysed and with a current tree
 * @param start the range's start
 * @param end the range's end
 * @param settings the client's
 * @param open the other open documents, for edits of other files; files that
 *   aren't open are read from disk
 * @return an array of LSP CodeActions
 */
nlohmann::json codeActions(const Document& document,
                           Position start,
                           Position end,
                           const ClientSettings& settings,
                           const OpenDocuments& open = {});

/**
 * An attribute's docs as Markdown, for its hover and completion item: its
 * summary, the fields it's for and its example as a cpp code block.
 */
std::string attributeDocs(const codegen::AttributeInfo& info);

/** One of the document's diagnostics as LSP JSON. */
nlohmann::json diagnosticJson(const Document& document,
                              const codegen::Diagnostic& diagnostic,
                              const ClientSettings& settings);

/**
 * The edits that hide `rule` with an sfs-ignore comment: on `row` (a next-line
 * comment above it, or added to one there already) or in the whole file.
 *
 * @param document the document, with a current tree
 * @param rule the rule to hide
 * @param row the 0-based line, for the line form
 * @param wholeFile the file form instead
 * @return the edits, in the document's source ranges
 */
std::vector<codegen::TextEdit> ignoreEdits(const Document& document,
                                           codegen::RuleId rule,
                                           uint32_t row,
                                           bool wholeFile);

} // namespace sfs::lsp
