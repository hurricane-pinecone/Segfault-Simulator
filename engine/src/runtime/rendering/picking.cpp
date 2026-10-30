#include "engine/runtime/rendering/picking.h"

#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "engine/runtime/rendering/modules/spriteModule.h"
#include "engine/runtime/rendering/renderSystem.h"
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
                                 const MeshLibrary& meshes,
                                 const RenderSystem* drawnBy)
{
  if (!entity.hasComponent<TransformComponent>())
    return std::nullopt;
  const glm::mat4 world = hierarchy::worldMatrix(entity);

  if (entity.hasComponent<MeshRendererComponent>())
    if (const Aabb* local =
            meshes.bounds(entity.getComponent<MeshRendererComponent>().mesh))
      return local->transformed(world);

  // A sprite's quad, where it was drawn (a hair thick, so a ray along it
  // still enters it).
  if (const SpriteModule* sprites =
          drawnBy ? drawnBy->module<SpriteModule>() : nullptr)
    if (const auto it = sprites->placed().find(entity.getId());
        it != sprites->placed().end())
    {
      Aabb quad;
      quad.expand(glm::vec3(-0.5f, -0.5f, 0.0f));
      quad.expand(glm::vec3(0.5f, 0.5f, 0.0f));
      quad = quad.transformed(it->second);
      quad.expand(quad.min - glm::vec3(1.0e-3f));
      quad.expand(quad.max + glm::vec3(1.0e-3f));
      return quad;
    }

  // A marker: in a 2D game, where its plane puts it, a few art pixels across.
  glm::vec3 position(world[3]);
  float radius = kPickMarkerRadius;
  if (drawnBy && drawnBy->space2D())
  {
    const Space2D& space = *drawnBy->space2D();
    position = space.place(glm::vec2(position), position.z);
    radius = 8.0f / std::max(space.pixelsPerUnit, 1.0e-3f);
  }
  Aabb marker;
  marker.expand(position - glm::vec3(radius));
  marker.expand(position + glm::vec3(radius));
  return marker;
}

std::optional<PickHit> pickEntity(Scene& scene,
                                  const MeshLibrary& meshes,
                                  const Ray& ray,
                                  const RenderSystem* drawnBy)
{
  std::optional<PickHit> best;
  float bestVolume = 0.0f;

  for (const Entity& entity : scene.entities())
  {
    const std::optional<Aabb> bounds = entityBounds(entity, meshes, drawnBy);
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
pickSurface(Scene& scene, const MeshLibrary& meshes, const Ray& ray)
{
  std::optional<SurfaceHit> best;
  for (const Entity& entity : scene.entities())
  {
    if (!entity.hasComponent<MeshRendererComponent>() ||
        !entity.hasComponent<TransformComponent>())
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
