#include "engine/runtime/systems/prefabSystem.h"

#include "engine/core/logger/logger.h"
#include "engine/runtime/rendering/renderSystem.h"
#include "engine/runtime/rendering/standardScene.h"
#include "engine/runtime/scripting/scriptRegistry.h"
#include "engine/runtime/serialization/prefabComponents.h"

namespace sfs
{

PrefabSystem::PrefabSystem(StandardScene& scene)
    : m_scene(scene), m_serializer(scene.serializer())
{
}

Entity PrefabSystem::spawn(std::string_view prefab,
                           const TransformComponent& transform,
                           const Entity& parent)
{
  if (parent != Entity{} && !parent.isValid())
  {
    LOG_ERROR("PrefabSystem: can't spawn " + std::string(prefab) +
              " under a destroyed parent; nothing spawned");
    return {};
  }
  m_made.clear();
  Entity root = m_serializer.instantiate(prefab, m_scene, parent, &m_made);
  // Unreadable: the serializer leaves a placeholder (for saving back); a
  // spawn leaves nothing.
  if (!root.isValid() || root.hasComponent<MissingPrefabComponent>())
  {
    LOG_ERROR("PrefabSystem: can't spawn " + std::string(prefab) +
              "; nothing spawned");
    for (const Entity& entity : m_made)
      m_scene.destroyEntity(entity);
    return {};
  }
  if (TransformComponent* placed = root.getComponent<TransformComponent>())
    *placed = transform;
  else
    root.addComponent<TransformComponent>(transform);
  startObjects(m_scene, m_made, &m_scene.renderSystem().meshes());
  return root;
}

void PrefabSystem::despawn(const Entity& root)
{
  if (root.isValid())
    m_scene.destroyBranch(root);
}

} // namespace sfs
