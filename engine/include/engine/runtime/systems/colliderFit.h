#pragma once

#include "engine/core/components/boxCollider3DComponent.h"
#include "engine/core/components/capsuleCollider3DComponent.h"
#include "engine/core/components/sphereCollider3DComponent.h"
#include "engine/core/rendering/aabb.h"

namespace sfs
{

class Entity;
class MeshLibrary;

/**
 * Size and centre a collider to a box in its entity's local space (e.g. its
 * mesh's bounds). The other fields (trigger, layer) are left alone. An empty
 * box changes nothing.
 *
 * - Box: the box itself. A flat one (a plane) gets a slab kMinColliderThickness
 *   thick under its surface instead of a sheet bodies could tunnel through.
 * - Sphere: centred, its radius the largest half-size (it covers the box's
 *   faces, not its corners).
 * - Capsule: upright (along Z), its radius the larger of the X and Y
 *   half-sizes, its height the box's (at least 2 * radius).
 */
void fitCollider(BoxCollider3DComponent& collider, const Aabb& bounds);
void fitCollider(SphereCollider3DComponent& collider, const Aabb& bounds);
void fitCollider(CapsuleCollider3DComponent& collider, const Aabb& bounds);

/** How thin a fitted box collider may be, in metres. */
inline constexpr float kMinColliderThickness = 0.1f;

/**
 * The bounds of an entity's meshes (MeshRendererComponent) in its own local
 * space. Its own mesh only, unless `includeChildren` or it has no mesh of its
 * own: then its descendants' too (so a collider on a group covers the group).
 * Empty if there are none, or their meshes haven't loaded.
 */
Aabb entityMeshBounds(const Entity& entity,
                      const MeshLibrary& meshes,
                      bool includeChildren = false);

/**
 * Fit the entity's collider of type `Collider` (BoxCollider3DComponent,
 * SphereCollider3DComponent or CapsuleCollider3DComponent) to its meshes'
 * bounds (entityMeshBounds()).
 *
 * @return false (nothing changed) if it has no such collider or no meshes
 */
template <typename Collider>
bool fitColliderToMesh(const Entity& entity,
                       const MeshLibrary& meshes,
                       bool includeChildren = false);

} // namespace sfs
