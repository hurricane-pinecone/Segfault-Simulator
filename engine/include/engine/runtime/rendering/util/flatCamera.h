#pragma once

#include "engine/core/rendering/flatProjection.h"
#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

/**
 * Build a FlatProjection for a 2D camera centred on cameraCenter (world units)
 * over a viewport of screenSize pixels, at the given zoom. The game resolves its
 * camera target/offset to a world centre (e.g. from a CameraComponent following
 * an entity) and calls this each frame, then hands the result to
 * FlatRenderSystem::setProjection -- the flat counterpart to the isometric
 * makeProjection.
 */
inline FlatProjection makeFlatProjection(const glm::vec2& cameraCenter,
                                         const glm::vec2& screenSize,
                                         float zoom = 1.0f)
{
  FlatProjection projection;
  projection.cameraCenter = cameraCenter;
  projection.screenCenter = screenSize * 0.5f;
  projection.zoom = zoom;
  return projection;
}

} // namespace sfs
