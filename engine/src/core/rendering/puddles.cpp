#include "engine/core/rendering/puddles.h"

#include "engine/core/rendering/deformation.h"
#include "engine/core/rendering/terrain.h"
#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <cmath>

namespace sfs
{

glm::vec2 puddleWindowCenter(glm::vec3 eye, glm::vec3 view, float ground)
{
  const glm::vec2 flat(view);
  const float across = glm::length(flat);
  if (across < 1e-4f)
    return glm::vec2(eye);
  float out = kPuddleReach;
  if (view.z < -1e-4f)
    out = std::min(out, std::max(eye.z - ground, 0.0f) / -view.z * across);
  return glm::vec2(eye) + flat / across * out;
}

glm::vec2 puddleWindowOrigin(glm::vec2 center)
{
  return glm::floor((center - 0.5f * kPuddleWindow) / kPuddleGroundTexel) *
         kPuddleGroundTexel;
}

bool puddleWindowMoves(glm::vec2 origin, glm::vec2 center)
{
  return glm::length(center - (origin + 0.5f * kPuddleWindow)) > kPuddleMove;
}

std::vector<glm::vec4> puddleGround(const GroundSurface& surface,
                                    const TerrainMaterial& terrain,
                                    glm::vec2 origin)
{
  std::vector<glm::vec4> ground = surface.raster(
      origin, kPuddleGroundTexel, glm::ivec2(kPuddleGroundTexels));
  for (glm::vec4& texel : ground)
    texel.w = texel.w > 0.5f
                  ? terrainPuddles(terrain, glm::vec2(texel.y, texel.z))
                  : -1.0f;
  return ground;
}

} // namespace sfs
