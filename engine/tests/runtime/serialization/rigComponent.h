#pragma once

// A component with entity reference fields for the serializer tests,
// registered by hand as sfs-codegen would.

#include <engine/core/logger/logger.h>
#include <engine/core/reflection/registration.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace sfs::testing3d
{

/** References to other entities: one, and a list. */
struct RigComponent
{
  /** One entity, e.g. a model's arm. */
  Entity arm;
  /** Several, e.g. a mower's blades. */
  std::vector<Entity> blades;
};

/** A model's part, filled from its node named "Arm" ([[sfs::part]]). */
struct PartRigComponent
{
  /** The part. */
  Entity arm;
};

/** Register RigComponent and PartRigComponent with the process's types. */
inline void registerRig()
{
  if (reflect::types().find("RigComponent"))
    return;
  reflect::TypeInfo part = reflect::componentType<PartRigComponent>(
      "PartRigComponent", "Part Rig", "tests");
  reflect::FieldInfo partArm;
  partArm.name = "arm";
  partArm.displayName = "Arm";
  partArm.kind = reflect::FieldKind::Entity;
  partArm.attributes.part = "Arm";
  partArm.address = [](void* object) -> void*
  { return &static_cast<PartRigComponent*>(object)->arm; };
  part.fields.push_back(std::move(partArm));
  reflect::types().add(std::move(part));

  reflect::TypeInfo type =
      reflect::componentType<RigComponent>("RigComponent", "Rig", "tests");
  reflect::FieldInfo arm;
  arm.name = "arm";
  arm.displayName = "Arm";
  arm.kind = reflect::FieldKind::Entity;
  arm.address = [](void* object) -> void*
  { return &static_cast<RigComponent*>(object)->arm; };
  type.fields.push_back(std::move(arm));
  reflect::FieldInfo blades;
  blades.name = "blades";
  blades.displayName = "Blades";
  blades.kind = reflect::FieldKind::EntityList;
  blades.address = [](void* object) -> void*
  { return &static_cast<RigComponent*>(object)->blades; };
  type.fields.push_back(std::move(blades));
  reflect::types().add(std::move(type));
}

/** The log's lines from its making on, to count warnings by. */
struct LogLines
{
  std::vector<std::string> lines;
  Logger::SinkId sink;

  LogLines()
  {
    Logger::flush(); // nothing from before
    sink = Logger::addSink([this](Logger::Level, const std::string& line)
                           { lines.push_back(line); });
  }
  ~LogLines() { Logger::removeSink(sink); }

  /** How many lines so far hold `text`. */
  std::size_t count(std::string_view text)
  {
    Logger::flush();
    return static_cast<std::size_t>(
        std::count_if(lines.begin(),
                      lines.end(),
                      [&](const std::string& line)
                      { return line.find(text) != std::string::npos; }));
  }
};

} // namespace sfs::testing3d
