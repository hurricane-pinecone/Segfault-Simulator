#pragma once

#include "sfs/codegen/model.h"

#include <filesystem>
#include <string_view>
#include <vector>

struct TSTree;

// Suppression comments: `sfs-ignore(rule, ...)` hides the named rules'
// warnings on its own line, `sfs-ignore-next-line(rule, ...)` on the line
// after it and `sfs-ignore-file(rule, ...)` in the whole file. Errors and
// bad-ignore are never hidden. Codegen and sfs-lsp share this one reading.
namespace sfs::codegen
{

/**
 * Finds a file's sfs-ignore comments.
 *
 * @param tree the file's syntax tree
 * @param source the file's text
 * @param path the file, for the diagnostics
 * @param diagnostics gains a bad-ignore warning for each malformed comment
 * @return the rules ignored, by line and for the whole file
 */
Suppressions findSuppressions(const TSTree* tree,
                              std::string_view source,
                              const std::filesystem::path& path,
                              std::vector<Diagnostic>& diagnostics);

/**
 * Whether `suppressions` hide `diagnostic`. Errors and bad-ignore never are.
 *
 * @param suppressions a file's sfs-ignore comments
 * @param diagnostic one of that file's diagnostics
 * @return true when an sfs-ignore comment names its rule for its line or file
 */
bool isSuppressed(const Suppressions& suppressions,
                  const Diagnostic& diagnostic);

/** Removes the diagnostics `suppressions` hide (never errors or bad-ignore). */
void applySuppressions(const Suppressions& suppressions,
                       std::vector<Diagnostic>& diagnostics);

/** The three sfs-ignore directives, by how much of the file they cover. */
enum class IgnoreForm : uint8_t
{
  Line,     // the comment's own line
  NextLine, // the line after the comment
  File,     // the whole file
};

/** One sfs-ignore comment, as written: where its parts are. */
struct IgnoreDirective
{
  /** Its form. */
  IgnoreForm form = IgnoreForm::Line;
  /** The comment holding it. */
  SourceRange comment;
  /** The comment's first byte. */
  uint32_t commentStart = 0;
  /** The byte after the comment. */
  uint32_t commentEnd = 0;
  /** The first byte of the directive's name. */
  uint32_t nameStart = 0;
  /** The byte just after the directive's name (where `(` should be). */
  uint32_t nameEnd = 0;
  /**
   * The name is none of the three (`sfs-ignore-nextline(...)`): it hides
   * nothing, and is a bad-ignore.
   */
  bool misspelt = false;
  /** The byte of its `(`, or 0 without one. */
  uint32_t open = 0;
  /** The byte of its `)`, or 0 when the list isn't closed. */
  uint32_t close = 0;
};

/**
 * The sfs-ignore directive a comment holds, if any. The directive must be the
 * first thing in the comment's text.
 *
 * @param source the file's text
 * @param commentStart the comment's first byte
 * @param commentEnd the byte after the comment
 * @param directive filled in when there is one (its `comment` range is left as
 * is)
 * @return whether the comment holds a directive, well-formed or not
 */
bool readIgnoreDirective(std::string_view source,
                         uint32_t commentStart,
                         uint32_t commentEnd,
                         IgnoreDirective& directive);

/**
 * Every comment of a file that holds an sfs-ignore directive, malformed and
 * misspelt ones included, in file order. Allocates nothing for a file without
 * one.
 *
 * @param tree the file's syntax tree
 * @param source the file's text
 * @return the directives, with their comments' ranges filled in
 */
std::vector<IgnoreDirective> findIgnoreDirectives(const TSTree* tree,
                                                  std::string_view source);

/** The directive's name as written, e.g. "sfs-ignore-next-line". */
std::string_view ignoreFormName(IgnoreForm form);

} // namespace sfs::codegen
