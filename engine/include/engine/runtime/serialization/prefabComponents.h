#pragma once

#include "nlohmann/json.hpp"

#include <cstdint>
#include <string>

namespace sfs
{

/**
 * Marks the root of a prefab instance: the entities below it (those with a
 * PrefabLinkComponent) came from the prefab, and the scene saves the instance
 * as the prefab plus how it differs (see SceneSerializer). Not reflected: the
 * serializer and editor handle it themselves.
 *
 * @param std::string prefab - the prefab's name ("asset:<id>": a .prefab, or a
 *                             model, whose node tree is its content)
 */
struct PrefabInstanceComponent
{
  std::string prefab;
};

/**
 * The bit set in every model node's stable id (ModelNode::id). A prefab gives
 * its own entities ids below kAddedNodeIdBit, so they never clash.
 */
inline constexpr uint32_t kModelNodeIdBit = 0x80000000u;

/**
 * The bit set (and kModelNodeIdBit clear) in the id of an entity a prefab adds
 * inside one of its nested instances: keyed beside that instance's own
 * entities, it can't clash with their ids.
 */
inline constexpr uint32_t kAddedNodeIdBit = 0x40000000u;

/**
 * On an entity made from a prefab: its id in the prefab file that made it, so
 * an instance can tell which of its entities are which when it's saved. A
 * nested instance's root carries its id in the prefab containing it. A model
 * node's is its stable id (kModelNodeIdBit set).
 */
struct PrefabLinkComponent
{
  uint32_t id = 0;
};

/**
 * An instance whose prefab couldn't be read (missing or broken file, or a
 * prefab containing itself): its scene entry is kept here as read and written
 * back unchanged, so saving doesn't lose it.
 */
struct MissingPrefabComponent
{
  nlohmann::json entry;
};

} // namespace sfs
