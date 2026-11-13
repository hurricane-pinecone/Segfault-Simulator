#include "engine/runtime/rendering/picking.h"

#include "engine/core/components/cursorPickComponent.h"
#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/components/pickableComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/runtime/input/mouseInput.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "engine/runtime/rendering/modules/spriteModule.h"
#include "engine/runtime/rendering/renderSystem.h"
#include "engine/runtime/sceneManager/scene.h"

#include <limits>

namespace sfs
{

namespace
{
float volume(const Aabb& box)
{
  const glm::vec3 size = box.max - box.min;
  return size.x * size.y * size.z;
}

// The nearest pickable entity at or above `entity`, or an invalid entity.
Entity pickableOf(Entity entity)
{
  for (; entity.isValid(); entity = hierarchy::parentOf(entity))
    if (entity.hasComponent<PickableComponent>())
      return entity;
  return {};
}

constexpr MouseButton kButtons[] = {MouseButton::Left,
                                    MouseButton::Middle,
                                    MouseButton::Right,
                                    MouseButton::X1,
                                    MouseButton::X2};
} // namespace

std::optional<Aabb> entityBounds(const Entity& entity,
                                 const MeshLibrary& meshes,
                                 const RenderSystem* drawnBy)
{
  if (!entity.hasComponent<TransformComponent>())
    return std::nullopt;
  const glm::mat4 world = hierarchy::worldMatrix(entity);

  if (entity.hasComponent<MeshRendererComponent>())
    if (const Aabb* local = meshes.posedBounds(
            entity.getComponent<MeshRendererComponent>().mesh, entity.getId()))
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

  constexpr float kTie = 1.0e-4f;
  for (const Entity& entity : scene.entities())
  {
    const std::optional<Aabb> bounds = entityBounds(entity, meshes, drawnBy);
    float distance = 0.0f;
    if (!bounds || !ray.intersectAabb(*bounds, distance) ||
        (best && distance > best->distance + kTie))
      continue;
    // A mesh by where the ray meets its triangles: its bounds only say it
    // might be hit (a ray from inside a landscape's bounds enters them at
    // once, wherever it really meets the ground).
    if (entity.hasComponent<MeshRendererComponent>())
    {
      const auto hit =
          meshes.raycast(entity.getComponent<MeshRendererComponent>().mesh,
                         hierarchy::worldMatrix(entity),
                         ray,
                         entity.getId());
      if (!hit)
        continue;
      distance = hit->distance;
    }

    const float size = volume(*bounds);
    const bool nearer = !best || distance < best->distance - kTie;
    const bool tiedButSmaller =
        best && distance <= best->distance + kTie && size < bestVolume;
    if (nearer || tiedButSmaller)
    {
      best = PickHit{entity, distance, entity, ray.at(distance)};
      bestVolume = size;
    }
  }
  return best;
}

std::optional<PickHit>
pickPickable(Scene& scene, const MeshLibrary& meshes, const Ray& ray)
{
  std::optional<PickHit> best;
  float nearest = std::numeric_limits<float>::max();
  for (const Entity& part :
       scene.query<MeshRendererComponent, TransformComponent>())
  {
    const Entity pickable = pickableOf(part);
    if (!pickable.isValid() ||
        !pickable.getComponent<PickableComponent>().enabled)
      continue;
    const MeshHandle mesh = part.getComponent<MeshRendererComponent>().mesh;
    const Aabb* bounds = meshes.posedBounds(mesh, part.getId());
    const glm::mat4 world = hierarchy::worldMatrix(part);
    float entry = 0.0f;
    if (!bounds || bounds->empty() ||
        !ray.intersectAabb(bounds->transformed(world), entry) ||
        entry >= nearest)
      continue;
    // Where it meets the triangles: the bounds only say it might.
    if (const auto hit = meshes.raycast(mesh, world, ray, part.getId());
        hit && hit->distance < nearest)
    {
      nearest = hit->distance;
      best = PickHit{pickable, hit->distance, part, ray.at(hit->distance)};
    }
  }
  return best;
}

void pickUnderCursor(Scene& scene,
                     const MeshLibrary& meshes,
                     const Ray& ray,
                     const MouseInput& mouse,
                     CursorPickComponent& pick)
{
  pick.ray = ray;
  pick.blocked = mouse.blocked();
  pick.inView = mouse.inView();
  pick.picked = {};
  pick.part = {};
  pick.point = glm::vec3(0.0f);
  pick.distance = 0.0f;
  if (pick.blocked)
    return;
  bool pressed = pick.hover;
  for (const MouseButton button : kButtons)
    pressed = pressed || mouse.mousePressed(button);
  if (!pressed)
    return;
  if (const std::optional<PickHit> hit = pickPickable(scene, meshes, ray))
  {
    pick.picked = hit->entity;
    pick.part = hit->part;
    pick.point = hit->point;
    pick.distance = hit->distance;
  }
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
    const Aabb* local = meshes.posedBounds(mesh, entity.getId());
    if (!local)
      continue;
    // Bounds first: most entities are nowhere near the ray.
    const glm::mat4 world = hierarchy::worldMatrix(entity);
    float entry = 0.0f;
    if (!ray.intersectAabb(local->transformed(world), entry) ||
        (best && entry > best->distance))
      continue;
    if (const auto hit = meshes.raycast(mesh, world, ray, entity.getId());
        hit && (!best || hit->distance < best->distance))
      best = SurfaceHit{entity, hit->section, hit->distance};
  }
  return best;
}

} // namespace sfs
