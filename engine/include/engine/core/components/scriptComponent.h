#pragma once

#include "nlohmann/json.hpp"

#include <string>
#include <utility>

namespace sfs
{

/**
 * Attaches a game object script to the entity, by class name. When the scene
 * plays, the named GameObject class (from the game module) is created bound to
 * this entity, so its onCreate/onUpdate drive it -- the editor's equivalent of
 * adding a MonoBehaviour.
 *
 * @param std::string script - GameObject class name, e.g. "Player"
 * @param nlohmann::json fields - this entity's values for the script's fields
 *   ({ "speed": 2.5, ... }), applied before its onCreate; fields not listed
 *   keep the defaults from the class
 */
struct ScriptComponent
{
  [[sfs::script]] std::string script;
  nlohmann::json fields = nlohmann::json::object();

  ScriptComponent() = default;
  explicit ScriptComponent(std::string script) : script(std::move(script)) {}
};

} // namespace sfs
