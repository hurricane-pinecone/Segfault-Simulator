#pragma once

#include <sfs/codegen/model.h>
#include <sfs/lsp/protocol.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct TSParser;
struct TSTree;

namespace sfs::lsp
{

/** How sfs-lsp treats a file. */
enum class FileRole : uint8_t
{
  CodegenHeader, // a header codegen reads: codegen's diagnostics too
  Header,        // any other header
  Source,        // a .cpp (or other) file
};

/**
 * An LSP position: a 0-based line and a character in the session's encoding.
 */
struct Position
{
  uint32_t line = 0;
  uint32_t character = 0;
};

/** Deletes a tree-sitter tree. */
struct TreeDeleter
{
  void operator()(TSTree* tree) const;
};

/** Deletes a tree-sitter parser. */
struct ParserDeleter
{
  void operator()(TSParser* parser) const;
};

/** A tree-sitter parser, deleted with its owner. */
using ParserPtr = std::unique_ptr<TSParser, ParserDeleter>;

/** A new tree-sitter parser for C++. */
ParserPtr makeCppParser();

/**
 * One open file: its text, line index and syntax tree, kept current edit by
 * edit.
 */
struct Document
{
  /** The URI the client opened it by (answers name it the same way). */
  std::string uri;
  /** Its local path. */
  std::filesystem::path path;
  /** The client's version of the text. */
  int version = 0;
  /** How it's checked; refreshed at each analysis. */
  FileRole role = FileRole::Source;
  /** The text, with spare capacity so most edits don't reallocate. */
  std::string text;
  /** The byte offset each line starts at; [0] is 0. */
  std::vector<uint32_t> lineStarts;
  /** The syntax tree: edited in place on every change, reparsed lazily. */
  std::unique_ptr<TSTree, TreeDeleter> tree;
  /** The tree needs a reparse before it's read. */
  bool stale = true;
  /** The diagnostics need an analysis and a publish. */
  bool dirty = true;
  /** When the text last changed: an analysis waits for it to settle. */
  std::chrono::steady_clock::time_point lastChange;
  /**
   * The diagnostics as published, with their fixes (code actions read them).
   */
  std::vector<codegen::Diagnostic> diagnostics;
  /** Its sfs-ignore comments as of the last analysis. */
  codegen::Suppressions suppressions;
};

/**
 * Replaces a document's whole text and recounts its lines; its tree goes stale.
 */
void setText(Document& document, std::string text);

/**
 * Applies one edit of an LSP didChange to the text, the line index and the tree
 * (ts_tree_edit), without reparsing. The tree goes stale.
 *
 * @param document the document
 * @param start where the replaced text starts
 * @param end where it ends
 * @param text what replaces it
 * @param encoding what the positions' characters count
 */
void applyEdit(Document& document,
               Position start,
               Position end,
               std::string_view text,
               PositionEncoding encoding);

/** Line `line`'s text, without its line break; empty past the end. */
std::string_view lineText(const Document& document, uint32_t line);

/** The byte offset of an LSP position, clamped to the text. */
uint32_t byteOffset(const Document& document,
                    Position position,
                    PositionEncoding encoding);

/** The LSP position of a tree-sitter point (a row and a byte column). */
Position positionOf(const Document& document,
                    uint32_t row,
                    uint32_t byteColumn,
                    PositionEncoding encoding);

/** The row and byte column of a byte offset. */
std::pair<uint32_t, uint32_t> pointOf(const Document& document, uint32_t byte);

/** Reparses a stale tree, reusing what the edits left unchanged. */
void reparse(Document& document, TSParser* parser);

/** The line starts of `text`, counted afresh. */
std::vector<uint32_t> countLines(std::string_view text);

/** An LSP Range for a source range of the document. */
nlohmann::json rangeJson(const Document& document,
                         const codegen::SourceRange& range,
                         PositionEncoding encoding);

/**
 * An LSP Position read from JSON ({"line", "character"}); zeros where it's
 * malformed.
 */
Position positionFrom(const nlohmann::json& position);

/** Whether `c` can be part of a C++ identifier. */
inline bool isIdentifierChar(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

/** Whether `c` can be part of a rule id (kebab-case). */
inline bool isRuleChar(char c) { return (c >= 'a' && c <= 'z') || c == '-'; }

/** Whether `path` names a C or C++ header (.h, .hh, .hpp, .hxx). */
bool isHeader(const std::filesystem::path& path);

} // namespace sfs::lsp
