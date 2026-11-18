#include "engine/runtime/navigation/geometry.h"

#include <glm/glm/common.hpp>
#include <glm/glm/ext/matrix_float4x4.hpp>
#include <glm/glm/ext/vector_float3.hpp>
#include <glm/glm/ext/vector_float4.hpp>

#include <algorithm>
#include <limits>

namespace sfs::nav
{

Box3 mountedBox(const glm::mat4& inAgent,
                const glm::vec3& lo,
                const glm::vec3& hi)
{
  glm::vec3 a(std::numeric_limits<float>::infinity());
  glm::vec3 b(-std::numeric_limits<float>::infinity());

  for (int corner = 0; corner < 8; ++corner)
  {
    const glm::vec3 c((corner & 1) ? hi.x : lo.x,
                      (corner & 2) ? hi.y : lo.y,
                      (corner & 4) ? hi.z : lo.z);
    const glm::vec3 p(inAgent * glm::vec4(c, 1.0f));
    a = glm::min(a, p);
    b = glm::max(b, p);
  }

  Box3 box;
  box.back = a.x;
  box.front = b.x;
  box.half = 0.5f * (b.y - a.y);
  box.shift = 0.5f * (b.y + a.y);
  box.bottom = std::max(a.z, 0.0f);
  box.top = b.z;
  return box;
}

} // namespace sfs::nav
