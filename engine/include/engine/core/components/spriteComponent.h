#pragma once

#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include <string>

namespace sfs
{

/**
 * Draws a sprite at the entity's transform: a frame of an image -- a texture
 * asset ("asset:<id>"; a sprite sheet if its import settings give it a grid,
 * see TextureImportSettings::spriteCell) or an image added to the AssetStore
 * by name. Its size is its pixels over the image's pixelsPerUnit (an import
 * setting; else a 2D game's Space2D's), times the transform's scale.
 *
 * anchor is the sprite's pivot in 0..1 of its size ({0.5, 1} = bottom centre,
 * so the transform sits at the sprite's feet); renderOffset nudges the drawn
 * sprite by a fixed pixel amount after anchoring.
 *
 * @param std::string image - the image to draw from
 * @param int frame - which frame of its grid (0: the first)
 * @param glm::vec2 anchor - pivot in 0..1 of the sprite size
 * @param glm::vec2 renderOffset - extra pixel offset after anchoring
 * @param glm::vec4 tint - multiplies its colours, default white; alpha below 1
 *                         fades it
 * @param std::string normalMap - an image of the same layout holding its
 *                                normals, so lights shade it per pixel; none
 *                                lights it flat
 * @param bool castsShadow - whether it shadows what's behind it (from the sun
 *                           and point lights that cast shadows), default false
 */
struct SpriteComponent
{
  [[sfs::texture]] std::string image;
  int frame = 0;
  glm::vec2 anchor{0.5f, 0.5f};
  glm::vec2 renderOffset{0.0f, 0.0f};
  [[sfs::color]] glm::vec4 tint{1.0f};
  [[sfs::texture]] std::string normalMap;
  bool castsShadow = false;
};

} // namespace sfs
