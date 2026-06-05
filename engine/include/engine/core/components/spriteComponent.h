#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include <cstdint>
#include <glm/glm/ext/vector_float3.hpp>

namespace sfs
{

/**
 * @param uint32_t spriteId
 * @param glm::vec2 anchor
 * @param glm::vec2 renderOffset in pixels
 */
struct SpriteComponent
{
  uint32_t spriteId;
  glm::vec2 anchor{0.5f, 1.0f};
  glm::vec2 renderOffset{0.0f, 0.0f};

  SpriteComponent(uint32_t spriteId,
                  glm::vec2 anchor = {0.5f, 0.0f},
                  glm::vec2 renderOffset = {0.0f, 0.0f})
      : spriteId(spriteId), anchor(anchor), renderOffset(renderOffset)
  {
  }
};

struct NormalMapComponent
{
  uint32_t spriteId;

  NormalMapComponent(uint32_t spriteId) : spriteId(spriteId) {}
};

} // namespace sfs
