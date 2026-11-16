#pragma once

#include "lints.h"

#include <tree_sitter/api.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The formatter's ordering passes: a header's class bodies, and a source
// file's definitions (docs/editor/sfs-lsp, "Formatting").
namespace sfs::lsp
{

/**
 * Orders every class and struct body of a header whose braces are on lines of
 * their own: sections public, protected, private (each access's sections
 * merged); in each, the nested types, aliases, using and friend declarations
 * and static_asserts in their order, then the methods (constructors, the
 * destructor and assignment operators first, then alphabetically), then the
 * member variables (in their order, or alphabetically with `sortFields`). Each
 * declaration moves with its comments. A class whose order can't be changed
 * safely keeps it, and `notes` says why.
 *
 * @param text the header, ending in a line break
 * @param tree its syntax tree
 * @param sortFields sort member variables too
 * @param notes gains a line for each class that kept its order
 * @return the new text (equal to `text` when nothing moved)
 */
std::string orderClassBodies(std::string_view text,
                             const TSTree* tree,
                             bool sortFields,
                             std::vector<std::string>& notes);

/**
 * Orders a source file's definitions in each scope (the file, each namespace
 * body): constants, variables, types and declarations first, in their order;
 * then each class's member definitions (constructors, the destructor,
 * assignment operators, the overrides, then the other methods alphabetically),
 * classes in the order their headers declare them; then free functions, in
 * their order. Preprocessor lines, namespaces, using-directives and the like
 * stay where they are, and nothing moves across them. A helper that something
 * moving above it uses, and that isn't declared earlier, stays with the
 * constants. A file whose order can't be changed safely keeps it, and `notes`
 * says why.
 *
 * @param text the source, ending in a line break
 * @param tree its syntax tree
 * @param path the file, to find its headers
 * @param parser a C++ parser, for the headers
 * @param read gives a header's text (an open buffer's, else the file's)
 * @param notes gains a line when the file kept its order
 * @return the new text (equal to `text` when nothing moved)
 */
std::string orderDefinitions(std::string_view text,
                             const TSTree* tree,
                             const std::filesystem::path& path,
                             TSParser* parser,
                             const FileReader& read,
                             std::vector<std::string>& notes);

} // namespace sfs::lsp
