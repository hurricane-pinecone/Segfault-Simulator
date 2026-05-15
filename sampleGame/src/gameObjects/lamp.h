#pragma once

#include "config.h"
#include "engine/assetStore/assetStore.h"
#include "engine/components/colliderComponent.h"
#include "engine/components/lightEmitterComponent.h"
#include "engine/components/spriteComponent.h"
#include "engine/components/transformComponent.h"
#include "engine/game/gameObject.h"
#include "engine/sceneManager/scene.h"
#include "engine/systems/isometricRenderSystem.h"

class Lamp : public sfs::GameObject
{
public:
  Lamp(glm::vec2 position) : m_position(position) {}

  void onCreate(sfs::Scene& scene) override
  {
    auto [sprite, normal] = scene.assetStore().getOrCreateSpriteWithNormal(
        "lamp", ASSET_ROOT + "sprites/lamp.png", {0, 0, 32, 32});

    m_entity =
        scene.createEntity()
            .addComponent<sfs::TransformComponent>(m_position)
            .addComponent<sfs::ElevationComponent>(0)
            .addComponent<sfs::SpriteComponent>(sprite, glm::vec2{0.5f, 1.0f})
            .addComponent<sfs::NormalMapComponent>(normal)
            .addComponent<sfs::ColliderComponent>(
                glm::vec2{-0.15f, -0.15f}, glm::vec2{0.3f, 0.3f})
            .addComponent<sfs::SolidObject>()
            .addComponent<sfs::LightEmitterComponent>(10.0f, 1.0f, 32.0f);

    m_entity.getComponent<sfs::ColliderComponent>().updateBounds(m_position);
  }

private:
  glm::vec2 m_position;
};
