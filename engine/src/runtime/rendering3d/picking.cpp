#include "engine/runtime/rendering3d/picking.h"

#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/transform3DComponent.h"
#include "engine/runtime/rendering3d/meshLibrary.h"
#include "engine/runtime/sceneManager/scene.h"

namespace sfs
{

namespace
{
float volume(const Aabb& box)
{
  const glm::vec3 size = box.max - box.min;
  return size.x * size.y * size.z;
}
} // namespace

std::optional<Aabb> entityBounds(const Entity& entity, const MeshLibrary& meshes)
{
  if (!entity.hasComponent<Transform3DComponent>())
    return std::nullopt;
  const auto& transform = entity.getComponent<Transform3DComponent>();

  if (entity.hasComponent<MeshRendererComponent>())
    if (const Aabb* local = meshes.bounds(entity.getComponent<MeshRendererComponent>().mesh))
      return local->transformed(transform.matrix());

  Aabb marker;
  marker.expand(transform.position - glm::vec3(kPickMarkerRadius));
  marker.expand(transform.position + glm::vec3(kPickMarkerRadius));
  return marker;
}

std::optional<PickHit> pickEntity(Scene& scene, const MeshLibrary& meshes, const Ray3D& ray)
{
  std::optional<PickHit> best;
  float bestVolume = 0.0f;

  for (const Entity& entity : scene.entities())
  {
    const std::optional<Aabb> bounds = entityBounds(entity, meshes);
    float distance = 0.0f;
    if (!bounds || !ray.intersectAabb(*bounds, distance))
      continue;

    constexpr float kTie = 1.0e-4f;
    const float size = volume(*bounds);
    const bool nearer = !best || distance < best->distance - kTie;
    const bool tiedButSmaller =
        best && distance <= best->distance + kTie && size < bestVolume;
    if (nearer || tiedButSmaller)
    {
      best = PickHit{entity, distance};
      bestVolume = size;
    }
  }
  return best;
}

} // namespace sfs
