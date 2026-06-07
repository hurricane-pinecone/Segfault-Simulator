#pragma once

#include "glm/glm/ext/vector_float3.hpp"

namespace sfs
{

/**
 * Optional multiply tint for a sprite on the flat render path. An absent
 * component means untinted (white). Lets a 2D game recolour sprites without
 * per-colour art.
 *
 * @param glm::vec3 color - RGB multiply tint
 * @param float alpha - opacity multiplier, default 1
 */
struct SpriteTint
{
  glm::vec3 color{1.0f, 1.0f, 1.0f};
  float alpha = 1.0f;

  SpriteTint() = default;
  SpriteTint(glm::vec3 color, float alpha = 1.0f) : color(color), alpha(alpha)
  {
  }
};

} // namespace sfs
