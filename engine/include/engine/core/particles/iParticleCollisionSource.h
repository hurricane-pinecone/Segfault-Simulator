#pragma once

#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

// The result of sweeping a particle against a collision surface.
struct ParticleHit
{
  bool hit = false;
  glm::vec2 pos{0.0f, 0.0f};    // contact point (world)
  glm::vec2 normal{0.0f, 0.0f}; // entry-face outward normal (top vs side)
  // World AABB of the surface that was hit, so a decal can be kept on it (clip /
  // clamp to the platform rather than spilling past its edges).
  glm::vec2 boundsMin{0.0f, 0.0f};
  glm::vec2 boundsMax{0.0f, 0.0f};
};

// A surface particles can collide with and stick to, decoupled from any specific
// world model. The isometric path uses a terrain heightfield (ITerrainSurfaceSource);
// a flat game provides one of these to make engine particles stick to its
// colliders and leave decals. See ParticleCollisionSystem for the default flat
// implementation.
class IParticleCollisionSource
{
public:
  virtual ~IParticleCollisionSource() = default;

  // Sweep a particle from `from` to `to`; return the first surface it crosses
  // into this step (ParticleHit::hit == false if none).
  virtual ParticleHit sweep(glm::vec2 from, glm::vec2 to) const = 0;
};

} // namespace sfs
