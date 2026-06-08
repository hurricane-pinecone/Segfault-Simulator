#pragma once

#include "config.h"
#include "engine/core/voxel/voxelView.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/ext/vector_float4.hpp"
#include <SDL_rect.h>
#include <SDL_surface.h>
#include <string>

// The sample's block types. Owns the interned texture-id strings the engine's
// BlockType points at, so it must live as long as the world renders (held as a
// scene member). Grass + sand are opaque cubes (block.png / sand.png,
// grass/dirt sides show on stacked voxels). Water is a physical liquid block:
// it occupies its cell (actors stand on the floor beneath, future water
// movement reads it) but draws nothing as geometry -- the water render path
// draws its surface.
class GameBlockRegistry : public sfs::IBlockRegistry
{
public:
  static constexpr sfs::BlockId kGrass = 1;
  static constexpr sfs::BlockId kSand = 2;
  static constexpr sfs::BlockId kWater = 3;
  static constexpr sfs::BlockId kGrassSlab = 4;
  static constexpr sfs::BlockId kSandSlab = 5;

  void setup(sfs::AssetStore& assets)
  {
    m_grass = makeCube(assets,
                       "voxel_grass",
                       "sprites/block.png",
                       m_grassTex,
                       sfs::SurfaceEffect::Type::Grass);
    m_sand = makeCube(assets,
                      "voxel_sand",
                      "sprites/sand.png",
                      m_sandTex,
                      sfs::SurfaceEffect::Type::Sand);

    // Slabs reuse their cube's texture (the mesher samples the top half of the
    // side), so they're the cube with a Slab shape.
    m_grassSlab = m_grass;
    m_grassSlab.shape = sfs::BlockShape::Slab;
    m_sandSlab = m_sand;
    m_sandSlab.shape = sfs::BlockShape::Slab;

    m_water = sfs::BlockType{};
    m_water.shape = sfs::BlockShape::Cube;
    m_water.textureId = nullptr;
    m_water.opaque = false;
    m_water.solid = false;
    m_water.liquid = true;
    m_water.effect = sfs::SurfaceEffect::Type::Water;
  }

  const sfs::BlockType& type(sfs::BlockId id) const override
  {
    switch (id)
    {
    case kGrass:
      return m_grass;
    case kSand:
      return m_sand;
    case kWater:
      return m_water;
    case kGrassSlab:
      return m_grassSlab;
    case kSandSlab:
      return m_sandSlab;
    default:
      return m_air;
    }
  }

private:
  static sfs::BlockType makeCube(sfs::AssetStore& assets,
                                 const std::string& name,
                                 const std::string& path,
                                 std::string& texOut,
                                 sfs::SurfaceEffect::Type effect)
  {
    const auto [spriteId, normalId] = assets.getOrCreateSpriteWithNormal(
        name, ASSET_ROOT + path, SDL_Rect{0, 0, 32, 32});
    (void)normalId;

    sfs::BlockType type{};
    type.effect = effect;

    const sfs::Sprite* sprite = assets.getSprite(spriteId);
    if (!sprite)
      return type;

    texOut = sprite->textureId;

    glm::vec2 size{32.0f, 32.0f};
    if (SDL_Surface* surface = assets.getSurface(texOut))
      size = {static_cast<float>(surface->w), static_cast<float>(surface->h)};

    const SDL_Rect& r = sprite->srcRect;
    type.textureId = &texOut;
    type.uvRect = {static_cast<float>(r.x) / size.x,
                   static_cast<float>(r.y) / size.y,
                   static_cast<float>(r.x + r.w) / size.x,
                   static_cast<float>(r.y + r.h) / size.y};
    return type;
  }

  std::string m_grassTex;
  std::string m_sandTex;
  sfs::BlockType m_air{sfs::BlockShape::Cube,
                       nullptr,
                       {},
                       false,
                       false,
                       sfs::SurfaceEffect::Type::None};
  sfs::BlockType m_grass{};
  sfs::BlockType m_sand{};
  sfs::BlockType m_grassSlab{};
  sfs::BlockType m_sandSlab{};
  sfs::BlockType m_water{};
};
