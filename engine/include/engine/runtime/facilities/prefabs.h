#pragma once

#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/facilityAccess.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/service.h"
#include "engine/core/ecs/system.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/sceneManager/scene.h"

#include <string_view>

namespace sfs
{

class PrefabSystem;

namespace facility
{

/**
 * Prefab copies made as the game runs, as game code uses it (`prefabs()` on
 * a System, Service, GameObject or Scene): a system planting hundreds of
 * trees, each from its shape's prefab. A copy is made as a scene file's
 * prefab instances are (nested prefabs, every component, its game objects
 * started), and joins the lists at the next flush, as createEntity()'s do. A
 * value handle over the scene's PrefabSystem, valid for the step that got
 * it: get it where it's used and keep it in no member. In a scene without
 * one its commands do nothing, with one warning a run (available() tells).
 */
class Prefabs
{
public:
  /** @return whether the scene can copy prefabs behind this facade */
  inline bool available() const { return m_prefabs != nullptr; }

  /**
   * Destroys a copy: `root` and everything below it, their game objects
   * with them, at the next flush. An invalid or dead root is ignored.
   * @param root the copy's root
   */
  void despawn(const Entity& root) const;

  /**
   * The prefab copies of a registry's scene, as the accessors give it.
   * @param registry the registry (null: none)
   * @return its facade, absent when it has no PrefabSystem
   */
  static Prefabs in(Registry* registry);

  /**
   * Makes a copy of a prefab under `parent` (none: a root), its root placed
   * at `transform`, relative to the parent.
   * @param prefab the prefab ("asset:<id>": a .prefab, or a model's nodes)
   * @param transform where its root goes (replacing the prefab root's own)
   * @param parent what it goes under (none: a root)
   * @return its root; invalid (logged, nothing made) if the prefab can't be
   *         read, the parent is gone, or the scene can't copy prefabs
   */
  Entity spawn(std::string_view prefab,
               const TransformComponent& transform,
               const Entity& parent = {}) const;

private:
  inline explicit Prefabs(PrefabSystem* prefabs) : m_prefabs(prefabs) {}

  // The copier; null (and warned about once) when the scene has none.
  PrefabSystem* backend() const;

  PrefabSystem* m_prefabs = nullptr;
};

} // namespace facility

inline facility::Prefabs System::prefabs() const
{
  return facility::Prefabs::in(registry);
}

inline facility::Prefabs Service::prefabs() const
{
  return facility::Prefabs::in(m_registry);
}

inline facility::Prefabs GameObject::prefabs() const
{
  return m_scene ? m_scene->prefabs() : facility::Prefabs::in(nullptr);
}

inline facility::Prefabs Scene::prefabs()
{
  return facility::Prefabs::in(&m_registry);
}

} // namespace sfs
