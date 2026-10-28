#pragma once

#include "engine/core/reflection/reflection.h"

#include "nlohmann/json.hpp"

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Saved data (scenes, prefabs, the editor's own snapshots) names fields: a
// component's under its type's name, a game object script's in its
// ScriptComponent's "fields". When the game module is rebuilt with fields
// renamed, added or removed, these work out what changed by comparing the
// types before and after, and update saved data to match, so values follow
// a renamed field and removed ones don't linger.
//
// A rename is a field that's gone while a new one of the same kind appeared
// in its place (the same position among the type's fields, or the only such
// pair); anything else is a removal and an addition.
namespace fieldMigration
{

struct Field
{
  std::string name;
  sfs::reflect::FieldKind kind;
};

// A type's fields, in declaration order.
struct Layout
{
  std::string type;
  sfs::reflect::TypeKind kind;
  std::vector<Field> fields;
};

/** The layouts of the types a module registered (copied: they outlive its unloading). */
std::vector<Layout> layoutsOf(const sfs::reflect::TypeRegistry& registry, std::string_view module);

struct TypeChanges
{
  std::map<std::string, std::string> renamed; // old name -> new
  std::set<std::string> removed;
};

struct Changes
{
  std::map<std::string, TypeChanges> components; // by component type
  std::map<std::string, TypeChanges> scripts;    // by game object script
  // Field names still in use by some script after the change (so a saved
  // value that names no script isn't dropped when another script has it).
  std::set<std::string> scriptFieldsNow;

  bool empty() const { return components.empty() && scripts.empty(); }
  /** For the log, e.g. "Spin.degreesPerSecond -> Spin.speed; Spin.axis removed". */
  std::string describe() const;
};

/** What changed between two builds of a module's types. */
Changes diff(const std::vector<Layout>& before, const std::vector<Layout>& after);

/**
 * Rename and drop fields in saved data: a scene, a prefab file or a snapshot,
 * prefab overrides included. @return whether it changed anything
 */
bool apply(const Changes& changes, nlohmann::json& data);

} // namespace fieldMigration
