#pragma once

#include <tree_sitter/api.h>

#include <cstdint>
#include <string>
#include <string_view>

// The formatter's blank-line pass (docs/editor/sfs-lsp, "Formatting").
namespace sfs::lsp
{

/**
 * Puts a blank line above every `if`, `for`, `while`, `do` and `switch`
 * statement (above the comments directly on top of it) unless it's the first
 * thing in its block or case, and below it when something follows it in the
 * same block. Only adds blank lines; nothing next to a preprocessor line or
 * inside a part that doesn't parse.
 *
 * @param text the file, ending in a line break
 * @param tree its syntax tree
 * @param firstRow the first row a blank line may go above (0-based)
 * @param lastRow the last row a blank line may go above
 * @return the new text (equal to `text` when nothing was added)
 */
std::string addControlBlankLines(std::string_view text,
                                 const TSTree* tree,
                                 uint32_t firstRow = 0,
                                 uint32_t lastRow = UINT32_MAX);

} // namespace sfs::lsp
