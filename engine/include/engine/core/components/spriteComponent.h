#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include <cstdint>
#include <glm/glm/ext/vector_float3.hpp>
#include <string>

namespace sfs
{

/**
 * Draws a sprite at the entity's transform: a frame of an image -- a texture
 * asset ("asset:<id>"; a sprite sheet if its import settings give it a grid,
 * see TextureImportSettings::spriteCell) or an image added to the AssetStore
 * by name -- or a sprite game code registered on the AssetStore (spriteId,
 * used instead when set).
 *
 * anchor is the sprite's pivot in 0..1 of its size ({0.5, 1} = bottom centre,
 * so the transform sits at the sprite's feet); renderOffset nudges the drawn
 * sprite by a fixed pixel amount after anchoring.
 *
 * @param std::string image - the image to draw from
 * @param int frame - which frame of its grid (0: the first)
 * @param glm::vec2 anchor - pivot in 0..1 of the sprite size
 * @param glm::vec2 renderOffset - extra pixel offset after anchoring
 */
struct SpriteComponent
{
  static constexpr uint32_t kNoSprite = UINT32_MAX;

  [[sfs::texture]] std::string image;
  int frame = 0;
  glm::vec2 anchor{0.5f, 0.5f};
  glm::vec2 renderOffset{0.0f, 0.0f};
  // A sprite game code registered (AssetStore::addSprite...); not saved.
  [[sfs::hide]] uint32_t spriteId = kNoSprite;

  SpriteComponent() = default;

  /** A registered sprite. */
  SpriteComponent(uint32_t spriteId,
                  glm::vec2 anchor = {0.5f, 0.0f},
                  glm::vec2 renderOffset = {0.0f, 0.0f})
      : anchor(anchor), renderOffset(renderOffset), spriteId(spriteId)
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
