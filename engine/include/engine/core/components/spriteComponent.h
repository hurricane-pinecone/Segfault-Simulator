#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include <cstdint>
#include <glm/glm/ext/vector_float3.hpp>

namespace sfs
{

/**
 * Draws a registered sprite (by id from the AssetStore) at the entity's
 * transform. anchor is the sprite's pivot in 0..1 of its size ({0.5, 1} =
 * bottom centre, so the transform sits at the sprite's feet); renderOffset
 * nudges the drawn sprite by a fixed pixel amount after anchoring.
 *
 * @param uint32_t spriteId - id of a sprite registered on the AssetStore
 * @param glm::vec2 anchor - pivot in 0..1 of the sprite size, default (0.5, 0)
 * @param glm::vec2 renderOffset - extra pixel offset after anchoring, default 0
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

/**
 * Pairs a normal-map sprite (by id) with an entity's SpriteComponent so the lit
 * render path shades it per pixel. Without it the sprite lights flat.
 *
 * @param uint32_t spriteId - id of the normal-map sprite on the AssetStore
 */
struct NormalMapComponent
{
  uint32_t spriteId;

  NormalMapComponent(uint32_t spriteId) : spriteId(spriteId) {}
};

} // namespace sfs
