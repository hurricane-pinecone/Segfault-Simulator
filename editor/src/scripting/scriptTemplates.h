#pragma once

#include <filesystem>
#include <optional>
#include <string>

// Starting code for new scripts: what Scripts -> New ... writes into the
// project's scripts folder. The class is named after the file, which is how
// the engine finds it -- no registration needed.
namespace scriptTemplates
{

enum class Kind
{
  GameObject, // behaviour attached to an entity (onCreate / onUpdate)
  System,     // runs over every entity with certain components
  Component,  // plain data on an entity
};

const char* label(Kind kind);

/** Whether `name` is usable as a C++ class name. */
bool isValidName(const std::string& name);

/**
 * Write `<scriptsFolder>/<name>.h` from the template for `kind`.
 *
 * @return the new file, or nothing (logged) if the name is invalid or the file
 * already exists.
 */
std::optional<std::filesystem::path> create(Kind kind,
                                            const std::string& name,
                                            const std::filesystem::path& scriptsFolder);

} // namespace scriptTemplates
