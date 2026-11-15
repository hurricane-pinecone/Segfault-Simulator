#include "engine/runtime/rendering/debug/colliderLines.h"

#include "engine/core/components/boxCollider2D.h"
#include "engine/core/components/boxCollider3DComponent.h"
#include "engine/core/components/capsuleCollider3DComponent.h"
#include "engine/core/components/sphereCollider3DComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/ecs.h" // IWYU pragma: keep (Entity templates)
#include "engine/core/hierarchy/hierarchy.h"

#include "glm/glm/geometric.hpp"

#include <algorithm>

namespace sfs::colliderLines
{

namespace
{

// An entity's world matrix taken apart: turned and moved (`placed`), and
// its scale, which goes into the sizes the way the physics bakes it.
struct Placement
{
  glm::mat4 placed{1.0f};
  glm::vec3 scale{1.0f};
};

Placement placementOf(const Entity& entity)
{
  const glm::mat4 world = hierarchy::worldMatrix(entity);
  Placement out;
  out.placed = world;
  for (int axis = 0; axis < 3; ++axis)
  {
    out.scale[axis] = glm::length(glm::vec3(world[axis]));
    if (out.scale[axis] > 0.0f)
      out.placed[axis] = world[axis] / out.scale[axis];
  }
  return out;
}

uint32_t colorOf(bool trigger)
{
  static const uint32_t solid = DebugLines::pack(kSolid);
  static const uint32_t triggered = DebugLines::pack(kTrigger);
  return trigger ? triggered : solid;
}

void box3D(const Placement& at,
           const BoxCollider3DComponent& box,
           DebugLines& out)
{
  out.box(at.placed,
          box.offset * at.scale,
          glm::abs(box.size * at.scale) * 0.5f,
          colorOf(box.trigger));
}

void sphere3D(const Placement& at,
              const SphereCollider3DComponent& sphere,
              DebugLines& out)
{
  const float radius =
      sphere.radius * std::max({at.scale.x, at.scale.y, at.scale.z});
  out.sphere(
      at.placed, sphere.offset * at.scale, radius, colorOf(sphere.trigger));
}

void capsule3D(const Placement& at,
               const CapsuleCollider3DComponent& capsule,
               DebugLines& out)
{
  const float radius = capsule.radius * std::max(at.scale.x, at.scale.y);
  const float half =
      std::max(capsule.height * at.scale.z * 0.5f - radius, 0.0f);
  out.capsule(at.placed,
              capsule.offset * at.scale,
              radius,
              half,
              colorOf(capsule.trigger));
}

// A 2D game's box: on its plane, where PhysicsSystem2D has it (unturned,
// unscaled), drawn where the plane puts it.
void box2D(const Entity& entity,
           const BoxCollider2D& box,
           const Space2D& space,
           DebugLines& out)
{
  const TransformComponent placed = hierarchy::worldTransform(entity);
  const glm::vec2 centre = placed.position2D() + box.offset;
  const glm::vec2 half = glm::abs(box.half);
  const glm::vec2 corners[] = {centre - half,
                               {centre.x + half.x, centre.y - half.y},
                               centre + half,
                               {centre.x - half.x, centre.y + half.y}};
  const uint32_t color = colorOf(box.trigger);
  for (int i = 0; i < 4; ++i)
    out.line(space.place(corners[i], placed.position.z),
             space.place(corners[(i + 1) % 4], placed.position.z),
             color);
}

} // namespace

void append(const Entity& entity,
            const std::optional<Space2D>& space,
            DebugLines& out)
{
  if (!entity.isValid() || !entity.hasComponent<TransformComponent>())
    return;
  if (const BoxCollider2D* box2 = entity.getComponent<BoxCollider2D>();
      space && box2)
    box2D(entity, *box2, *space, out);

  const auto* box = entity.getComponent<BoxCollider3DComponent>();
  const auto* sphere = entity.getComponent<SphereCollider3DComponent>();
  const auto* capsule = entity.getComponent<CapsuleCollider3DComponent>();
  if (!box && !sphere && !capsule)
    return;
  const Placement at = placementOf(entity);
  if (box)
    box3D(at, *box, out);
  if (sphere)
    sphere3D(at, *sphere, out);
  if (capsule)
    capsule3D(at, *capsule, out);
}

std::size_t appendAll(Registry& registry,
                      const std::optional<Space2D>& space,
                      DebugLines& out)
{
  std::size_t drawn = 0;
  for (auto [entity, transform, boxCollider3D] :
       registry.query<TransformComponent, BoxCollider3DComponent>())
  {
    box3D(placementOf(entity), boxCollider3D, out);
    ++drawn;
  }
  for (auto [entity, transform, sphereCollider3D] :
       registry.query<TransformComponent, SphereCollider3DComponent>())
  {
    sphere3D(placementOf(entity), sphereCollider3D, out);
    ++drawn;
  }
  for (auto [entity, transform, capsuleCollider3D] :
       registry.query<TransformComponent, CapsuleCollider3DComponent>())
  {
    capsule3D(placementOf(entity), capsuleCollider3D, out);
    ++drawn;
  }
  if (space)
    for (auto [entity, transform, boxCollider2D] :
         registry.query<TransformComponent, BoxCollider2D>())
    {
      box2D(entity, boxCollider2D, *space, out);
      ++drawn;
    }
  return drawn;
}

} // namespace sfs::colliderLines
