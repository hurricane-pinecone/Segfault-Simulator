#pragma once

#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/system.h"
#include "engine/runtime/serialization/sceneSerializer.h"

#include <string_view>
#include <vector>

namespace sfs
{

class StandardScene;

/**
 * Makes copies of prefab assets while the game runs, for systems and scripts:
 * e.g. a system planting hundreds of trees, each from its shape's prefab. A
 * copy is made the way a scene file's prefab instances are (nested prefabs,
 * every component, colliders included: PhysicsSystem3D gives them bodies on
 * its next update, and navigation sees the static ones from there), and its
 * game object scripts are started (onCreate), as when a scene plays.
 *
 * Every StandardScene has one: `registry->tryGetSystem<PrefabSystem>()` from
 * a system, `scene().tryGetSystem<PrefabSystem>()` from a game object.
 */
class PrefabSystem : public System
{
public:
  explicit PrefabSystem(StandardScene& scene);

  /**
   * Make a copy of a prefab ("asset:<id>": a .prefab, or a model's node
   * tree) under `parent` (none: a root), its root placed at `transform`
   * (relative to the parent, as every TransformComponent is; it replaces the
   * prefab root's own).
   *
   * @return its root; invalid (logged, nothing made) if the prefab can't be
   *         read or the parent is gone
   */
  Entity spawn(std::string_view prefab,
               const TransformComponent& transform,
               const Entity& parent = {});

  /**
   * Destroy a spawned copy: `root` and everything below it, their game
   * objects with them (at the scene's next flush). An invalid or dead root is
   * ignored.
   */
  void despawn(const Entity& root);

private:
  StandardScene& m_scene;
  SceneSerializer m_serializer;
  std::vector<Entity> m_made; // reused between spawns
};

} // namespace sfs
