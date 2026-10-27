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
 * case-insensitively, so transform3DComponent.h declares
 * Transform3DComponent). Its base decides the kind: sfs::GameObject or
 * sfs::System, or none for a component. Fields are public data members (minus
 * [[sfs::hide]]) plus non-public ones marked [[sfs::show]]; static members
 * and methods are ignored. A field whose type the editor can't edit is skipped
 * with a warning.
 */
ParseResult parseHeader(std::string_view source, const std::filesystem::path& path);

} // namespace sfs::codegen
