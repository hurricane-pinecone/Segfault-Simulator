#pragma once

#include "engine/core/components/boxCollider2D.h"
#include "engine/core/components/tags/solidObject.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/ecs/system.h"
#include "engine/core/particles/iParticleCollisionSource.h"
#include "engine/core/util/algorithms/aabbSweep.h"

namespace sfs
{

// Default flat collision source: sticks particles to every static solid
// (SolidObject + BoxCollider2D) in the scene via a swept-AABB test. A passive
// System so it gets the registry injected -- add it and hand it to
// ParticleEngine::setCollisionSource, and any flat game gets particle-stick
// decals out of the box.
class ParticleCollisionSystem : public System, public IParticleCollisionSource
{
public:
  ParticleHit sweep(glm::vec2 from, glm::vec2 to) const override
  {
    ParticleHit best;
    if (!registry)
      return best;

    const glm::vec2 seg = to - from;
    float bestT = 2.0f;

    for (const auto& solid :
         registry->view<SolidObject, TransformComponent, BoxCollider2D>())
    {
      const auto& box = solid.getComponent<BoxCollider2D>();
      const glm::vec2 centre =
          solid.getComponent<TransformComponent>().position + box.offset;

      float t = 0.0f;
      glm::vec2 normal{0.0f, 0.0f};
      if (!sweepAabb(from, seg, centre - box.half, centre + box.half, t, normal))
        continue;

      if (t < bestT)
      {
        bestT = t;
        best.hit = true;
        best.pos = from + seg * t;
        best.normal = normal;
        best.boundsMin = centre - box.half;
        best.boundsMax = centre + box.half;
      }
    }

    return best;
  }
};

} // namespace sfs
