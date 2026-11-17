#pragma once

#include "lints.h"

#include <tree_sitter/api.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct TSParser;

// sfs-lsp's formatter: clang-format, then the house passes where a tree's
// .sfs-format asks for them (docs/editor/sfs-lsp, "Formatting").
namespace sfs::lsp
{

/** A tree's house style, from the nearest .sfs-format above a file. */
struct HouseStyle
{
  /** A .sfs-format was found: the house passes run. */
  bool on = false;
  /** The folder holding it; `exclude` is relative to it. */
  std::filesystem::path root;
  /** Sort member variables alphabetically too (`sort-fields: true`). */
  bool sortFields = false;
  /** Folders whose files aren't formatted at all, `/`-separated. */
  std::vector<std::string> exclude;
  /** What in the file couldn't be read: unknown keys or values. */
  std::vector<std::string> problems;
};

/**
 * The house style for `file`: the nearest .sfs-format in its folder or one
 * above; `on` is false when there is none.
 */
HouseStyle houseStyleFor(const std::filesystem::path& file);

/** Reads a .sfs-format's text (`#` comments, `key: value` lines). */
HouseStyle parseHouseStyle(std::string_view text);

/** Whether `file` is under one of the style's excluded folders. */
bool isExcluded(const HouseStyle& style, const std::filesystem::path& file);

/**
 * The clang-format to run: `configured` when given, else the
 * SFS_CLANG_FORMAT environment variable, else `clang-format` or
 * `clang-format-17` on PATH. A bare name is looked up on PATH.
 *
 * @return its path; empty when none is found
 */
std::string findClangFormat(const std::string& configured);

/** What to format. */
struct FormatRequest
{
  /** The file's text. */
  std::string_view text;
  /** The file: its .clang-format and .sfs-format are found from it. */
  std::filesystem::path path;
  /** The clang-format to run: empty finds one (findClangFormat). */
  std::string clangFormat;
  /**
   * For range formatting, the 1-based first and last lines to format; 0: the
   * whole file. A range gets no reordering.
   */
  uint32_t firstLine = 0;
  uint32_t lastLine = 0;
  /** Gives a header's text for the ordering of a source; null: from disk. */
  const FileReader* read = nullptr;
};

/** What formatting a file came to. */
struct FormatResult
{
  /** The formatted text; the input when nothing could be done. */
  std::string text;
  /** Why something kept its order or was left out, one line each. */
  std::vector<std::string> notes;
  /** The file is in an excluded folder: nothing was done. */
  bool excluded = false;
  /** No clang-format was found: only the house passes ran. */
  bool noClangFormat = false;
  /** clang-format failed: its part was left out (`notes` says why). */
  bool clangFormatFailed = false;
  /**
   * The house passes' result lost or added a line and was dropped (a bug:
   * `notes` says where).
   */
  bool refused = false;
};

/**
 * Formats a file's text: clang-format with the file's .clang-format, then,
 * under a .sfs-format, the house passes (class order in headers, definition
 * order in sources, blank lines round control blocks), then clang-format again
 * if they changed anything. The house passes' result is kept only when it has
 * the same non-blank lines as their input, access labels aside.
 *
 * @param request the text, its file and the clang-format to use
 * @param parser a C++ parser
 * @return the formatted text and notes
 */
FormatResult formatSource(const FormatRequest& request, TSParser* parser);

/**
 * Whether two texts have the same non-blank lines (trailing whitespace aside),
 * as multisets, access labels (`public:` and the like) left out.
 */
bool sameLines(std::string_view before, std::string_view after);

/** One hunk of a line diff: original bytes [from, to) become `text`. */
struct LineEdit
{
  /** Where the replaced lines start in the original text. */
  uint32_t from = 0;
  /** Where they end (the start of the next line kept). */
  uint32_t to = 0;
  /** What replaces them: whole lines. */
  std::string text;
};

/**
 * The hunks that turn `before` into `after`, line by line (Myers' diff), in
 * order and not overlapping. A diff too large to work out cheaply is one hunk.
 */
std::vector<LineEdit> lineEdits(std::string_view before,
                                std::string_view after);

} // namespace sfs::lsp
