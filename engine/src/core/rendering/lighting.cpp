#include "engine/core/rendering/lighting.h"

#include "glm/glm/common.hpp"
#include "glm/glm/geometric.hpp"

namespace sfs
{

float clearSky(float cloudCover)
{
  // Even an overcast sky lets a little through.
  return 1.0f - 0.85f * glm::clamp(cloudCover, 0.0f, 1.0f);
}

float moonlight(const SceneLighting& lighting)
{
  const glm::vec3& moon = lighting.moon.direction;
  const glm::vec3& sun = lighting.sun.direction;
  if (glm::dot(moon, moon) < 1e-12f)
    return 0.0f;
  const float moonUp = glm::normalize(moon).z;
  const float sunUp =
      glm::dot(sun, sun) > 1e-12f ? glm::normalize(sun).z : -1.0f;
  // A half moon gives far less than half a full one's light.
  const float phase = glm::clamp(lighting.moon.phase, 0.0f, 1.0f);
  const float risen = glm::smoothstep(0.0f, 0.15f, moonUp);
  const float dark = 1.0f - glm::smoothstep(-0.10f, -0.005f, sunUp);
  return phase * phase * risen * dark * clearSky(lighting.cloudCover);
}

} // namespace sfs
