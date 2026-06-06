#pragma once

#include "engine/core/particles/iParticleCollisionSource.h"
#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

class ITerrainSurfaceSource;

// Sticks particles to a terrain heightfield: stepping up into a taller tile on a
// camera-facing face is a wall, otherwise the drop lands on the ground or water
// beneath. The game's terrain generator is the ITerrainSurfaceSource.
class TerrainParticleCollision : public IParticleCollisionSource
{
public:
  explicit TerrainParticleCollision(const ITerrainSurfaceSource* terrain)
      : m_terrain(terrain)
  {
  }

  ParticleHit sweep(const ParticleSweep& motion) const override;
  float groundElevation(glm::vec2 worldPos) const override;

private:
  const ITerrainSurfaceSource* m_terrain = nullptr;
};

} // namespace sfs
