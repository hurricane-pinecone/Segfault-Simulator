#pragma once

// Shared by the editor's reference tests: a component with part fields,
// registered as sfs-codegen would, and prefab files to load.

#include "scene/entityLabel.h"

#include "engine/core/components/nameComponent.h"
#include "engine/core/ecs/ecs.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/reflection/registration.h"
#include "engine/runtime/serialization/sceneSerializer.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace partsTests
{

namespace fs = std::filesystem;
using json = nlohmann::json;
using sfs::Entity;

// A machine's parts, filled from its model's node names: its arm is the node
// named "Arm", its blades every node matching "Blade-*".
struct PartsComponent
{
  Entity arm;
  std::vector<Entity> blades;
};

// Registered by hand, as sfs-codegen would.
inline void registerParts()
{
  if (sfs::reflect::types().find("PartsComponent"))
    return;
  sfs::reflect::TypeInfo type = sfs::reflect::componentType<PartsComponent>(
      "PartsComponent", "Parts", "tests");
  sfs::reflect::FieldInfo arm;
  arm.name = "arm";
  arm.displayName = "Arm";
  arm.kind = sfs::reflect::FieldKind::Entity;
  arm.attributes.part = "Arm";
  arm.address = [](void* object) -> void*
  { return &static_cast<PartsComponent*>(object)->arm; };
  type.fields.push_back(std::move(arm));
  sfs::reflect::FieldInfo blades;
  blades.name = "blades";
  blades.displayName = "Blades";
  blades.kind = sfs::reflect::FieldKind::EntityList;
  blades.attributes.part = "Blade-*";
  blades.address = [](void* object) -> void*
  { return &static_cast<PartsComponent*>(object)->blades; };
  type.fields.push_back(std::move(blades));
  sfs::reflect::types().add(std::move(type));
}

inline const sfs::reflect::TypeInfo& partsType()
{
  return *sfs::reflect::types().find("PartsComponent");
}

inline json named(const std::string& name, json extra = json::object())
{
  extra["NameComponent"] = {{"name", name}};
  return extra;
}

inline json moved(float z)
{
  return {{"TransformComponent", {{"position", {0, 0, z}}}}};
}

inline Entity childNamed(const Entity& parent, const std::string& name)
{
  for (const Entity& child : sfs::hierarchy::childrenOf(parent))
    if (entityLabel(child) == name)
      return child;
  return {};
}

// A rig prefab: its parts as children 1-3, its part fields as `fields` give
// them.
inline json rigPrefab(const json& fields)
{
  json rig = named("Rig", {{"PartsComponent", fields}});
  rig["children"] = json::array({named("Arm", {{"$id", 1}}),
                                 named("Blade-1", {{"$id", 2}}),
                                 named("Blade-2", {{"$id", 3}})});
  return {{"formatVersion", 2}, {"root", rig}};
}

inline void writeFile(const fs::path& path, const json& contents)
{
  fs::create_directories(path.parent_path());
  sfs::SceneSerializer::writeJsonFile(contents, path);
}

inline void writeMeta(const fs::path& file, const std::string& id)
{
  std::ofstream(fs::path(file.string() + ".meta"), std::ios::binary)
      << R"({"formatVersion": 1, "id": ")" + id + R"("})";
}

inline std::string readText(const fs::path& path)
{
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace partsTests
