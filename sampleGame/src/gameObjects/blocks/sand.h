#pragma once

#include "config.h"
#include "engine/core/components/elevationComponent.h"
#include "engine/core/components/spriteComponent.h"
#include "engine/core/components/surfaceEffect.h"
#include "engine/core/components/tags/isometricTile.h"
#include "engine/core/components/transformComponent.h"
#include "engine/runtime/sceneManager/scene.h"
#include "gameObjects/blocks/block.h"
#include <map>
#include <utility>

class SandBlock : public Block
{
public:
  using Block::Block;

  void onCreate(sfs::Scene& scene) override
  {
    const auto& variant = getVariant(m_shape, m_direction);

    auto [sprite, normal] = scene.assetStore().getOrCreateSpriteWithNormal(
        variant.name,
        ASSET_ROOT + variant.path,
        variant.src,
        ASSET_ROOT + "sprites/block_normal.png");

    m_entity =
        scene.createEntity()
            .addComponent<sfs::TransformComponent>(m_position)
            .addComponent<sfs::SpriteComponent>(sprite)
            .addComponent<sfs::NormalMapComponent>(normal)
            .addComponent<sfs::ElevationComponent>(m_elevation)
            .addComponent<sfs::SurfaceEffect>(sfs::SurfaceEffect::Type::Sand)
            .addTag<sfs::IsometricTile>();
  }

protected:
  const BlockSpriteDef& getVariant(Shape shape,
                                   Direction direction) const override
  {
    static const std::map<std::pair<Shape, Direction>, BlockSpriteDef>
        variants = {{{Shape::Full, Direction::North},
                     {"sand_block", "sprites/sand.png", {0, 0, 32, 32}}},
                    {{Shape::Half, Direction::North},
                     {"sand_half",

                      "sprites/sand_half.png",
                      {0, 0, 32, 32}}}};
    return variants.at({shape, direction});
  }
};
