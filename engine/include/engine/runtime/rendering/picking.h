#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/rendering/aabb.h"
#include "engine/core/rendering/camera.h"

#include <cstddef>
#include <optional>

namespace sfs
{

class MeshLibrary;
class RenderSystem;
class Scene;

/** Half-size of the box that stands in for an entity with no mesh (a light, an
 * empty). */
inline constexpr float kPickMarkerRadius = 0.3f;

/**
 * An entity's world-space bounds: its mesh's bounds through its world matrix
 * (parents included), or a small marker box at its position if it has no mesh.
 * Given the render system that draws it, also its sprite where that was drawn
 * last frame, and in a 2D game (a Space2D) its position on the game's plane.
 *
 * @return nothing if the entity has no TransformComponent.
 */
std::optional<Aabb> entityBounds(const Entity& entity,
                                 const MeshLibrary& meshes,
                                 const RenderSystem* drawnBy = nullptr);

struct PickHit
{
  Entity entity;
  float distance = 0.0f; // along the ray
};

/**
 * The entity under a ray (e.g. a mouse click through Camera::screenRay): the
 * nearest whose bounds (entityBounds()) the ray enters. When several are
 * entered at the same distance (the ray starts inside them), the smallest wins,
 * so an object standing on a large floor is picked over the floor.
 */
std::optional<PickHit> pickEntity(Scene& scene,
                                  const MeshLibrary& meshes,
                                  const Ray& ray,
                                  const RenderSystem* drawnBy = nullptr);

struct SurfaceHit
{
  Entity entity;
  std::size_t section = 0; // the mesh section (material slot) hit
  float distance = 0.0f;
};

/**
 * The mesh surface under a ray: the nearest triangle of any entity's mesh,
 * exact where pickEntity uses bounds. For dropping things onto objects, e.g. a
 * material onto the part of a model under the cursor.
 */
std::optional<SurfaceHit>
pickSurface(Scene& scene, const MeshLibrary& meshes, const Ray& ray);

} // namespace sfs
