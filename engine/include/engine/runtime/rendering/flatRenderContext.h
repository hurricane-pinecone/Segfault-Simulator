#pragma once

#include "engine/core/rendering/iProjection.h"
#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

/**
 * Render context for the flat 2D path: the per-frame state render modules read
 * through. Holds the projection (a FlatProjection in practice) and forwards the
 * world<->screen transform. The render-module host is templated on the context
 * type, so generic modules (e.g. Particles) work against this exactly as they do
 * against IsometricRenderContext, without any heightfield fields.
 */
struct FlatRenderContext
{
  // Non-null while the render pipeline runs; the forwarders below assume it.
  const IProjection* projection = nullptr;

  glm::vec2 worldToScreen(const glm::vec2& world, float elevation = 0.0f) const
  {
    return projection->worldToScreen(world, elevation);
  }

  glm::vec2 screenToWorld(const glm::vec2& screen, float elevation = 0.0f) const
  {
    return projection->screenToWorld(screen, elevation);
  }
};

} // namespace sfs
