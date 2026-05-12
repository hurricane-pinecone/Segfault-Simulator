#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include <cstdint>
#include <glm/glm/ext/vector_float3.hpp>

namespace sfs
{

/**
 * @param uint32_t spriteId
 */
struct SpriteComponent
{
  uint32_t spriteId;
  glm::vec2 anchor{0.5f, 1.0f};

  SpriteComponent(uint32_t spriteId, glm::vec2 anchor = {0.5f, 0.0f})
      : spriteId(spriteId), anchor(anchor)
  {
  }
};

} // namespace sfs
