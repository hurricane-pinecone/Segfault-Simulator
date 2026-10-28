#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/rendering/3d/aabb.h"
#include "engine/core/rendering/3d/camera3D.h"

#include <optional>

namespace sfs
{

class MeshLibrary;
class Scene;

/** Half-size of the box that stands in for an entity with no mesh (a light, an empty). */
inline constexpr float kPickMarkerRadius = 0.3f;

/**
 * An entity's world-space bounds: its mesh's bounds through its world matrix
 * (parents included), or a small marker box at its position if it has no mesh.
 *
 * @return nothing if the entity has no Transform3DComponent.
 */
std::optional<Aabb> entityBounds(const Entity& entity, const MeshLibrary& meshes);

struct PickHit
{
  Entity entity;
  float distance = 0.0f; // along the ray
};

/**
 * The entity under a ray (e.g. a mouse click through Camera3D::screenRay): the
 * nearest whose bounds the ray enters. When several are entered at the same
 * distance (the ray starts inside them), the smallest wins, so an object
 * standing on a large floor is picked over the floor.
 */
std::optional<PickHit> pickEntity(Scene& scene, const MeshLibrary& meshes, const Ray3D& ray);

} // namespace sfs
