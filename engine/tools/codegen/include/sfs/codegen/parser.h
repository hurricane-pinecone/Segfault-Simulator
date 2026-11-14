#pragma once

#include "sfs/codegen/model.h"

#include <filesystem>
#include <string_view>
#include <vector>

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
