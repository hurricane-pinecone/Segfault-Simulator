#pragma once

#include "sfs/codegen/model.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct TSTree;
struct TSNode;

namespace sfs::codegen
{

/**
 * Find a header's script types and their editable fields.
 *
 * The script type is the class or struct named after the file (compared
 * case-insensitively, so transformComponent.h declares
 * TransformComponent). Its base decides the kind: sfs::GameObject or
 * sfs::System, or none for a component. Fields are public data members (minus
 * [[sfs::hide]]) plus non-public ones marked [[sfs::show]]; static members
 * and methods are ignored. A field whose type the editor can't edit is skipped
 * with a warning.
 *
 * Any other class or struct at namespace scope marked [[sfs::component]]
 * (`struct [[sfs::component]] TipBody {};`) is a component too, in
 * ParseResult::components; a header may hold only marked types. A marked type
 * can't have a base; nested and template ones are warned about and skipped.
 */
// A UI header (*.ui.h) is skipped: nothing in it is a script or a component.
ParseResult parseHeader(std::string_view source, const std::filesystem::path& path);

/**
 * parseHeader() on a syntax tree the caller already has (sfs-lsp's, kept up to
 * date as the file is edited), instead of parsing the text again.
 *
 * @param source the header's text, which `tree` was parsed from
 * @param path the header
 * @param tree its tree-sitter C++ tree; null gives an empty result
 * @return as parseHeader(source, path)
 */
ParseResult parseHeader(std::string_view source,
                        const std::filesystem::path& path,
                        const TSTree* tree);

/**
 * The field kind (FieldModel::kind) codegen gives a field of a type.
 *
 * @param type the type as written, e.g. "glm::vec3" or "sfs::AssetRef<Spec>"
 * @param insideSfs whether the field is declared inside namespace sfs, where
 *   `Entity` means sfs::Entity
 * @return e.g. "Vec3" or "AssetRef"; empty for a type the editor can't edit
 */
std::string fieldKindOf(std::string_view type, bool insideSfs);

/** A file's whole contents; empty when it can't be read. */
std::string readFile(const std::filesystem::path& path);

/** The text of `source` a tree-sitter node covers; empty for a null node. */
std::string_view nodeText(std::string_view source, TSNode node);

/** A node's child by field name (tree-sitter's), e.g. "name". */
TSNode childField(TSNode node, std::string_view name);

/** Whether a namespace name is SFS's or inside it: "sfs" or "sfs::...". */
bool isSfsNamespace(std::string_view name);

/** The source range a tree-sitter node covers. */
SourceRange rangeOf(TSNode node);

/**
 * Find a UI header's (*.ui.h) structs and classes, at namespace scope: the
 * views (with a body() method) and what they show. Fields are the public,
 * non-static, non-const data members that aren't pointers, references or
 * arrays. Templates and nested types are left out.
 */
UiParseResult parseUiHeader(std::string_view source, const std::filesystem::path& path);

/**
 * Check that no two types share a registered (unqualified) name, which would
 * make one replace the other in the reflection registry.
 *
 * @param types Every type of one codegen run, in header order.
 * @return An error at each later duplicate, naming the header of the first.
 */
std::vector<Diagnostic> checkNames(const std::vector<TypeModel>& types);

} // namespace sfs::codegen
