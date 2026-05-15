#pragma once

#include "config.h"
#include "engine/components/spriteComponent.h"
#include "engine/components/transformComponent.h"
#include "engine/sceneManager/scene.h"
#include "engine/systems/isometricRenderSystem.h"
#include "gameObjects/blocks/block.h"

class GrassBlock : public Block
{
public:
  using Block::Block;

  void onCreate(sfs::Scene& scene) override
  {
    const auto& variant = getVariant(m_shape, m_direction);

    auto [sprite, normal] = scene.assetStore().getOrCreateSpriteWithNormal(
        variant.name, ASSET_ROOT + variant.path, variant.src);

    m_entity = scene.createEntity()
                   .addComponent<sfs::TransformComponent>(m_position)
                   .addComponent<sfs::SpriteComponent>(sprite)
                   .addComponent<sfs::NormalMapComponent>(normal)
                   .addComponent<sfs::ElevationComponent>(m_elevation)
                   .addTag<sfs::IsometricTile>();
  }

protected:
  const BlockSpriteDef& getVariant(BlockShape shape,
                                   Direction direction) const override
  {
    static const std::map<std::pair<BlockShape, Direction>, BlockSpriteDef>
        variants = {
            {{BlockShape::Full, Direction::North},
             {"grass_block", "sprites/block.png", {0, 0, 32, 32}}},

            {{BlockShape::Half, Direction::North},
             {"grass_block_half", "sprites/block_half.png", {0, 0, 32, 32}}},
        };

    return variants.at({shape, direction});
  }
};
