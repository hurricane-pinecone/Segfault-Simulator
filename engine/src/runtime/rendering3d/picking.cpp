#include "engine/runtime/rendering3d/picking.h"

#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/transform3DComponent.h"
#include "engine/core/hierarchy/hierarchy.h"
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

std::optional<Aabb> entityBounds(const Entity& entity,
                                 const MeshLibrary& meshes)
{
  if (!entity.hasComponent<Transform3DComponent>())
    return std::nullopt;
  const glm::mat4 world = hierarchy::worldMatrix(entity);

  if (entity.hasComponent<MeshRendererComponent>())
    if (const Aabb* local =
            meshes.bounds(entity.getComponent<MeshRendererComponent>().mesh))
      return local->transformed(world);

  const glm::vec3 position(world[3]);
  Aabb marker;
  marker.expand(position - glm::vec3(kPickMarkerRadius));
  marker.expand(position + glm::vec3(kPickMarkerRadius));
  return marker;
}

std::optional<PickHit>
pickEntity(Scene& scene, const MeshLibrary& meshes, const Ray3D& ray)
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

std::optional<SurfaceHit>
pickSurface(Scene& scene, const MeshLibrary& meshes, const Ray3D& ray)
{
  std::optional<SurfaceHit> best;
  for (const Entity& entity : scene.entities())
  {
    if (!entity.hasComponent<MeshRendererComponent>() ||
        !entity.hasComponent<Transform3DComponent>())
      continue;
    const MeshHandle mesh = entity.getComponent<MeshRendererComponent>().mesh;
    const Aabb* local = meshes.bounds(mesh);
    if (!local)
      continue;
    // Bounds first: most entities are nowhere near the ray.
    const glm::mat4 world = hierarchy::worldMatrix(entity);
    float entry = 0.0f;
    if (!ray.intersectAabb(local->transformed(world), entry) ||
        (best && entry > best->distance))
      continue;
    if (const auto hit = meshes.raycast(mesh, world, ray);
        hit && (!best || hit->distance < best->distance))
      best = SurfaceHit{entity, hit->section, hit->distance};
  }
  return best;
}

} // namespace sfs
