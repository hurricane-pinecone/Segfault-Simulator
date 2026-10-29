#pragma once

#include "sfs/codegen/model.h"

#include <filesystem>
#include <string>
#include <vector>

namespace sfs::codegen
{

struct GeneratorOptions
{
  // Module name stored on every registered type (used to unregister a module
  // before it is unloaded).
  std::string module;

  // The registration function to define, fully qualified; it must already be
  // declared by an included header, e.g. "sfs::reflect::registerEngineTypes".
  std::string function;

  // Header that declares `function` (included first).
  std::string functionHeader = "engine/core/reflection/reflection.h";

  // Headers are included relative to this directory when they are inside it,
  // otherwise by absolute path.
  std::filesystem::path includeRoot;
};

/** C++ source registering `types` with the reflection registry. */
std::string generateRegistration(const std::vector<TypeModel>& types,
                                 const GeneratorOptions& options);

/**
 * C++ source for a UI preview module (the editor's UI Preview loads it):
 * each view in `types` as a preview (sfs::ui::Preview, named after it),
 * filled with placeholders (sfs::ui::Placeholder, specialised for every type
 * so lists of them are filled too). Defines an empty module entry point.
 */
std::string generateUiPreviews(const std::vector<UiTypeModel>& types,
                               const GeneratorOptions& options);

/** "castsShadow" -> "Casts Shadow", "MeshRendererComponent" -> "Mesh Renderer". */
std::string displayName(std::string_view identifier, bool stripComponentSuffix);

} // namespace sfs::codegen
