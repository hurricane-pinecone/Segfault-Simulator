#pragma once

#include "engine/core/components/surfaceEffect.h"
#include "glm/glm/ext/vector_float4.hpp"
#include <cstdint>
#include <string>

namespace sfs
{

using BlockId = std::uint16_t;
constexpr BlockId kAirBlock = 0;

// Iso elevation levels per voxel cell. A cube sprite (block.png) is two levels
// tall, so one voxel spans two levels; a Slab fills the lower level only.
constexpr int kLevelsPerBlock = 2;

enum class BlockShape : std::uint8_t
{
  Cube, // fills the cell
  Slab, // half height, sits on the cell floor (added in a later phase)
};

// Static description of a block id. Pure data: the game builds these (resolving
// its sprite's srcRect to a normalised uv rect with its AssetStore) and
// registers them in an IBlockRegistry; the engine only reads them.
struct BlockType
{
  BlockShape shape = BlockShape::Cube;
  // The block's cube sprite (top diamond packed over the +x/+y side faces, like
  // block.png). Pointer is interned/owned by the registry (stable address).
  const std::string* textureId = nullptr;
  // Normalised sub-rect (u0, v0, u1, v1) of that texture the sprite occupies.
  glm::vec4 uvRect{0.0f, 0.0f, 1.0f, 1.0f};
  bool opaque = true; // hides the neighbour face it abuts
  bool solid =
      true; // collision + counts toward the terrain-top (walkable) height
  SurfaceEffect::Type effect = SurfaceEffect::Type::None;
  // A liquid (water): a physical block that occupies its cell, but the mesher
  // emits no opaque geometry for it -- its surface is drawn by the water render
  // path instead. Not solid (you can enter it), not opaque (terrain
  // under/behind it still shows).
  bool liquid = false;
};

} // namespace sfs
