#include "engine/runtime/systems/colliderFit.h"

#include "engine/core/components/meshRendererComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/runtime/rendering/meshLibrary.h"
#include "glm/glm/matrix.hpp"

#include <algorithm>

namespace sfs
{

void fitCollider(BoxCollider3DComponent& collider, const Aabb& bounds)
{
  if (bounds.empty())
    return;
  collider.size = bounds.extents() * 2.0f;
  collider.offset = bounds.center();
  // A flat box (a plane) gets a slab under its surface.
  for (int axis = 0; axis < 3; ++axis)
    if (collider.size[axis] < kMinColliderThickness)
    {
      collider.offset[axis] -=
          (kMinColliderThickness - collider.size[axis]) * 0.5f;
      collider.size[axis] = kMinColliderThickness;
    }
}

void fitCollider(SphereCollider3DComponent& collider, const Aabb& bounds)
{
  if (bounds.empty())
    return;
  const glm::vec3 half = bounds.extents();
  collider.radius = std::max({half.x, half.y, half.z, 0.01f});
  collider.offset = bounds.center();
}

void fitCollider(CapsuleCollider3DComponent& collider, const Aabb& bounds)
{
  if (bounds.empty())
    return;
  const glm::vec3 half = bounds.extents();
  collider.radius = std::max({half.x, half.y, 0.01f});
  collider.height = std::max(half.z * 2.0f, collider.radius * 2.0f);
  collider.offset = bounds.center();
}

namespace
{
const Aabb* ownMeshBounds(const Entity& entity, const MeshLibrary& meshes)
{
  if (!entity.hasComponent<MeshRendererComponent>())
    return nullptr;
  const Aabb* bounds =
      meshes.bounds(entity.getComponent<MeshRendererComponent>().mesh);
  return bounds && !bounds->empty() ? bounds : nullptr;
}
} // namespace

Aabb entityMeshBounds(const Entity& entity,
                      const MeshLibrary& meshes,
                      const hierarchy::EntityTree& tree,
                      bool includeChildren)
{
  Aabb bounds;
  if (const Aabb* own = ownMeshBounds(entity, meshes))
  {
    bounds = *own;
    if (!includeChildren)
      return bounds;
  }
  const glm::mat4 toLocal = glm::inverse(hierarchy::worldMatrix(entity));
  for (const Entity& part : tree.subtree(entity))
    if (part != entity)
      if (const Aabb* mesh = ownMeshBounds(part, meshes))
        bounds.expand(
            mesh->transformed(toLocal * hierarchy::worldMatrix(part)));
  return bounds;
}

template <typename Collider>
bool fitColliderToMesh(const Entity& entity,
                       const MeshLibrary& meshes,
                       const hierarchy::EntityTree& tree,
                       bool includeChildren)
{
  if (!entity.hasComponent<Collider>())
    return false;
  const Aabb bounds = entityMeshBounds(entity, meshes, tree, includeChildren);
  if (bounds.empty())
    return false;
  fitCollider(entity.getComponent<Collider>(), bounds);
  return true;
}

template bool
fitColliderToMesh<BoxCollider3DComponent>(const Entity&,
                                          const MeshLibrary&,
                                          const hierarchy::EntityTree&,
                                          bool);
template bool
fitColliderToMesh<SphereCollider3DComponent>(const Entity&,
                                             const MeshLibrary&,
                                             const hierarchy::EntityTree&,
                                             bool);
template bool
fitColliderToMesh<CapsuleCollider3DComponent>(const Entity&,
                                              const MeshLibrary&,
                                              const hierarchy::EntityTree&,
                                              bool);

} // namespace sfs
