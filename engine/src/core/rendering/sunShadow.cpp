#include "engine/core/rendering/sunShadow.h"

#include "glm/glm/ext/matrix_clip_space.hpp"
#include "glm/glm/ext/matrix_transform.hpp"
#include "glm/glm/geometric.hpp"

#include <algorithm>
#include <cmath>

namespace sfs
{

glm::mat4 fitSunShadow(const glm::vec3& sunDirection,
                       const glm::vec3& center,
                       float radius,
                       uint32_t resolution)
{
  const glm::vec3 toSun = glm::normalize(sunDirection);

  // lookAt needs an up vector that isn't parallel to the view; near noon the
  // sun is straight overhead, so fall back to +Y.
  const glm::vec3 up = std::abs(toSun.z) > 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                 : glm::vec3(0.0f, 0.0f, 1.0f);

  // Sit the light outside the covered sphere so every caster in it lies
  // between the near and far planes.
  const glm::vec3 eye = center + toSun * (radius * 2.0f);
  const glm::mat4 view = glm::lookAtRH(eye, center, up);
  glm::mat4 projection =
      glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, radius * 4.0f);

  // Snap: move the projection so the world origin lands on a texel corner.
  // The covered region then only ever shifts by whole texels.
  const glm::mat4 viewProjection = projection * view;
  const float texelsPerUnit =
      static_cast<float>(std::max(resolution, 1u)) * 0.5f;
  const glm::vec4 origin = viewProjection * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
  const glm::vec2 originTexels = glm::vec2(origin) * texelsPerUnit;
  const glm::vec2 offset =
      (glm::vec2(std::round(originTexels.x), std::round(originTexels.y)) -
       originTexels) /
      texelsPerUnit;

  projection[3][0] += offset.x;
  projection[3][1] += offset.y;
  return projection * view;
}

} // namespace sfs
