#include "engine/runtime/facilities/prefabs.h"

#include "absentBackend.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/registry.h"
#include "engine/runtime/systems/prefabSystem.h"

#include <atomic>
#include <string_view>

namespace sfs::facility
{

namespace
{
std::atomic<bool> s_warned{false};
} // namespace

PrefabSystem* Prefabs::backend() const
{
  if (!m_prefabs)
    warnAbsent(s_warned, "prefabs", "PrefabSystem");

  return m_prefabs;
}

void Prefabs::despawn(const Entity& root) const
{
  if (PrefabSystem* prefabs = backend())
    prefabs->despawn(root);
}

Prefabs Prefabs::in(Registry* registry)
{
  return Prefabs(registry ? registry->tryGetSystem<PrefabSystem>() : nullptr);
}

Entity Prefabs::spawn(std::string_view prefab,
                      const TransformComponent& transform,
                      const Entity& parent) const
{
  PrefabSystem* prefabs = backend();
  return prefabs ? prefabs->spawn(prefab, transform, parent) : Entity{};
}

} // namespace sfs::facility
