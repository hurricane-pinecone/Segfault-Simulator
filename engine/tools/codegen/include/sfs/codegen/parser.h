#pragma once

#include "sfs/codegen/model.h"

#include <filesystem>
#include <string_view>

namespace sfs::codegen
{

/**
 * Find a header's script type and its editable fields.
 *
 * The script type is the class or struct named after the file (compared
 * case-insensitively, so transformComponent.h declares
 * TransformComponent). Its base decides the kind: sfs::GameObject or
 * sfs::System, or none for a component. Fields are public data members (minus
 * [[sfs::hide]]) plus non-public ones marked [[sfs::show]]; static members
 * and methods are ignored. A field whose type the editor can't edit is skipped
 * with a warning.
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

} // namespace sfs::codegen
