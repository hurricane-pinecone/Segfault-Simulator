#pragma once

#include "engine/core/rendering/iProjection.h"
#include "glm/glm/ext/vector_float2.hpp"

namespace sfs
{

/**
 * Flat 2D world-to-screen projection: a panned, zoomed orthographic view with no
 * elevation. World units map straight to pixels scaled by zoom, so the elevation
 * argument is ignored. This is the projection a plain 2D game (e.g. a side-on
 * platformer) renders through; IsometricProjection is the heightfield variant.
 */
struct FlatProjection final : IProjection
{
  // World position shown at the centre of the screen.
  glm::vec2 cameraCenter{0.0f, 0.0f};
  // Screen pixel that cameraCenter maps to (usually the viewport centre).
  glm::vec2 screenCenter{0.0f, 0.0f};
  // Screen pixels per world unit.
  float zoom = 1.0f;

  glm::vec2 worldToScreen(const glm::vec2& world,
                          float /*elevation*/) const final
  {
    return (world - cameraCenter) * zoom + screenCenter;
  }

  glm::vec2 screenToWorld(const glm::vec2& screen,
                          float /*elevation*/) const final
  {
    return (screen - screenCenter) / zoom + cameraCenter;
  }

  float worldUnitToPixels() const final { return zoom; }
};

} // namespace sfs
