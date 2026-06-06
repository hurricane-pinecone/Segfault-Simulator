#pragma once

#include "engine/core/particles/iParticleCollisionSource.h"

namespace sfs
{

class Registry;

// Sticks particles to the scene's static solids (SolidObject + BoxCollider2D) by
// swept AABB, filling the collider's world bounds so a flat decal can be clipped
// to it.
class ColliderParticleCollision : public IParticleCollisionSource
{
public:
  explicit ColliderParticleCollision(Registry* registry)
      : m_registry(registry)
  {
  }

  ParticleHit sweep(const ParticleSweep& motion) const override;

private:
  Registry* m_registry = nullptr;
};

} // namespace sfs
