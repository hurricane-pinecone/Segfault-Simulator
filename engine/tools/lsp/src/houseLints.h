#pragma once

#include "sfs/codegen/model.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct TSTree;

// The house lints: SFS's and its games' own code style, on where a tree's
// .sfs-lint includes `house`.
namespace sfs::lsp
{

/**
 * Runs the house lints over a file in one walk of its tree: constexpr-name,
 * nested-ternary and else-after-return. ERROR subtrees are skipped.
 *
 * @param source the file's text
 * @param tree its syntax tree; null: nothing is reported
 * @param path the file, for the diagnostics (a header's constants get no
 *   rename fix)
 * @param out gains a diagnostic for each finding, with its fix if it has one
 */
void runHouseLints(std::string_view source,
                   const TSTree* tree,
                   const std::filesystem::path& path,
                   std::vector<codegen::Diagnostic>& out);

/**
 * A name in UPPER_SNAKE_CASE: a `k` prefix dropped (`kMaxItems`), words split
 * where the case changes (an acronym stays one word: `kHTTPPort` gives
 * `HTTP_PORT`), digits kept with the word before them (`kVec3Size` gives
 * `VEC3_SIZE`), and underscores kept as one.
 *
 * @param name the name as written
 * @return the name in UPPER_SNAKE_CASE
 */
std::string upperSnakeName(std::string_view name);

} // namespace sfs::lsp
