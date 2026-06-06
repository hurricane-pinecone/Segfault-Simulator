#pragma once

#include "engine/core/components/boxCollider2D.h"
#include "engine/core/components/tags/solidObject.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/particles/iParticleCollisionSource.h"
#include "engine/core/util/algorithms/aabbSweep.h"
#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

// Sticks particles to the scene's static solids (SolidObject + BoxCollider2D) by
// swept AABB, filling the collider's world bounds so a flat decal can be clipped
// to it. Reads the ECS directly, so it lives with the particle engine rather than
// the renderer.
class ColliderParticleCollision : public IParticleCollisionSource
{
public:
  explicit ColliderParticleCollision(Registry* registry)
      : m_registry(registry)
  {
  }

  ParticleHit sweep(const ParticleSweep& m) const override
  {
    ParticleHit best;
    if (!m_registry)
      return best;

    const glm::vec2 seg = m.to - m.from;
    float bestT = 2.0f;

    for (const auto& solid :
         m_registry->view<SolidObject, TransformComponent, BoxCollider2D>())
    {
      const auto& box = solid.getComponent<BoxCollider2D>();
      const glm::vec2 centre =
          solid.getComponent<TransformComponent>().position + box.offset;

      float t = 0.0f;
      glm::vec2 normal{0.0f, 0.0f};
      if (!sweepAabb(m.from, seg, centre - box.half, centre + box.half, t,
                     normal))
        continue;

      if (t < bestT)
      {
        bestT = t;
        best.hit = true;
        best.pos = m.from + seg * t;
        best.normal = normal;
        best.boundsMin = centre - box.half;
        best.boundsMax = centre + box.half;
      }
    }

    return best;
  }

private:
  Registry* m_registry = nullptr;
};

} // namespace sfs
