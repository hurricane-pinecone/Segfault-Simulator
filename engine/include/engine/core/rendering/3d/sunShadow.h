#pragma once

#include "glm/glm/ext/matrix_float4x4.hpp"
#include "glm/glm/ext/vector_float3.hpp"

#include <cstdint>

namespace sfs
{

/**
 * Light view-projection for a directional shadow map covering the sphere of
 * `radius` around `center`, seen from `sunDirection` (pointing toward the
 * sun). Depth maps to [0, 1]. A sphere rather than a box, so the covered
 * region is the same whichever way the sun points.
 *
 * The projection is snapped to whole shadow-map texels, so shadow edges stay
 * put while the covered region follows a moving camera instead of crawling.
 *
 * @param resolution shadow-map edge in texels (for the snapping)
 */
glm::mat4 fitSunShadow(const glm::vec3& sunDirection,
                       const glm::vec3& center,
                       float radius,
                       uint32_t resolution);

} // namespace sfs
