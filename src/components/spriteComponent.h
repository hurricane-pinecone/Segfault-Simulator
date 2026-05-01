#pragma once

#include <cstdint>
#include <glm/ext/vector_float3.hpp>

struct SpriteComponent
{
  uint32_t spriteId;

  SpriteComponent(uint32_t spriteId) : spriteId(spriteId) {}
};
