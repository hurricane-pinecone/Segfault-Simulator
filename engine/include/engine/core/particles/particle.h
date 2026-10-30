#pragma once

#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

// One live particle. Colour/size/alpha/frame are derived at render time from
// the owning effect's curves and the normalized age, so they are not stored
// here.
struct Particle
{
  // Position on the plane (a 2D game's units; in 3D, the ground's x, y).
  glm::vec2 pos{0.0f, 0.0f};
  float height = 0.0f; // height above ground in world elevation units

  glm::vec2 vel{0.0f, 0.0f};
  float velZ = 0.0f;

  float age = 0.0f;
  float lifetime = 1.0f;

  float baseSize = 1.0f;
  float rotation = 0.0f;
  float angularVel = 0.0f;

  bool grounded = false; // GroundBehavior::Stick: resting on the ground
};

} // namespace sfs
