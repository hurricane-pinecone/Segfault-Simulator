#pragma once

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

  SpriteComponent(uint32_t spriteId) : spriteId(spriteId) {}
};

} // namespace sfs
