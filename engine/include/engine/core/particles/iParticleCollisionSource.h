#pragma once

#include "engine/core/particles/decal.h" // DecalSurface
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_int2.hpp"
#include <cstdint>

namespace sfs
{

// A particle's motion this step, in the engine's world space: planar from->to
// plus the elevation either end (toZ is the current height; the flat path
// leaves the Z fields 0 and ignores them).
struct ParticleSweep
{
  glm::vec2 from{0.0f, 0.0f};
  glm::vec2 to{0.0f, 0.0f};
  float fromZ = 0.0f;
  float toZ = 0.0f;
};

// The result of sweeping a particle against a collision surface. Carries enough
// to place a decal on any path: the contact, the surface it hit (ground / wall
// / water), and -- for walls -- the face it stuck to. The flat path leaves the
// surface fields at their defaults (Ground) and fills `boundsMin/Max` for
// clipping; the iso path fills the surface fields and leaves bounds zero (its
// sink clips per tile/face).
struct ParticleHit
{
  bool hit = false;
  glm::vec2 pos{0.0f, 0.0f};    // contact point (world)
  glm::vec2 normal{0.0f, 0.0f}; // entry-face outward normal (flat: top vs side)

  DecalSurface surface = DecalSurface::Ground;
  glm::ivec2 tile{0, 0}; // tile hit (wall face placement)
  std::uint8_t wallSide = 0;
  float wallBottom = 0.0f; // wall face base elevation
  float wallTop = 0.0f;    // wall face top elevation
  float elevation = 0.0f;  // surface elevation the mark sits at
  float fadeRate = 0.0f;   // per-second alpha decay (water; else 0)

  // World AABB of a flat collider, so a flat decal can be clipped to it.
  glm::vec2 boundsMin{0.0f, 0.0f};
  glm::vec2 boundsMax{0.0f, 0.0f};
};

// A surface particles collide with and stick to, decoupled from any specific
// world model. Implemented by TerrainParticleCollision (marches a terrain
// heightfield) and ColliderParticleCollision (sweeps the scene's colliders); a
// Particles module owns the right one and picks it in enableStains().
class IParticleCollisionSource
{
public:
  virtual ~IParticleCollisionSource() = default;

  // Sweep a particle's motion; return the first surface it crosses into this
  // step (ParticleHit::hit == false if none).
  virtual ParticleHit sweep(const ParticleSweep& motion) const = 0;

  // Surface elevation at a world position, so a burst spawned "on the ground"
  // lands at the right height. 0 where there's no heightfield (the flat path).
  virtual float groundElevation(glm::vec2 /*worldPos*/) const { return 0.0f; }
};

} // namespace sfs
