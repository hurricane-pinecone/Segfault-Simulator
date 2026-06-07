#include "engine/core/rendering/projection/flatProjection.h"

namespace sfs
{

glm::vec2 FlatProjection::worldToScreen(const glm::vec2& world,
                                        float /*elevation*/) const
{
  return (world - cameraCenter) * zoom + screenCenter;
}

glm::vec2 FlatProjection::screenToWorld(const glm::vec2& screen,
                                        float /*elevation*/) const
{
  return (screen - screenCenter) / zoom + cameraCenter;
}

float FlatProjection::worldUnitToPixels() const { return zoom; }

} // namespace sfs
