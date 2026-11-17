#include "engine/core/sceneData/wind.h"

#include <glm/glm/common.hpp>
#include <glm/glm/ext/vector_float2.hpp>
#include <glm/glm/trigonometric.hpp>

#include <cmath>

namespace sfs
{

glm::vec2 windVelocity(const Wind& wind)
{
  if (!std::isfinite(wind.speed) || !std::isfinite(wind.direction))
    return glm::vec2(0.0f);

  const float angle = glm::radians(wind.direction);
  return glm::vec2(std::cos(angle), std::sin(angle)) * wind.speed;
}

float windSway(const Wind& wind)
{
  const float speed = std::isfinite(wind.speed) ? wind.speed : 0.0f;
  return kSwayAtBreeze *
         glm::clamp(speed / kBreezeSpeed, kLeastSway, kMostSway);
}

} // namespace sfs
