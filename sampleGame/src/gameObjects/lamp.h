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
#include "glm/glm/ext/vector_float3.hpp"

class Lamp : public sfs::GameObject
{
public:
  struct Color
  {
    static constexpr glm::vec3 WarmWhite{1.0f, 0.9f, 0.7f};
    static constexpr glm::vec3 White{1.0f, 1.0f, 1.0f};
    static constexpr glm::vec3 Red{1.0f, 0.05f, 0.05f};
    static constexpr glm::vec3 SoftRed{1.0f, 0.25f, 0.25f};
    static constexpr glm::vec3 Blue{0.25f, 0.35f, 1.0f};
    static constexpr glm::vec3 SoftBlue{0.45f, 0.55f, 1.0f};
    static constexpr glm::vec3 Green{0.2f, 1.0f, 0.2f};
    static constexpr glm::vec3 SoftGreen{0.45f, 1.0f, 0.45f};
    static constexpr glm::vec3 Purple{0.8f, 0.3f, 1.0f};
    static constexpr glm::vec3 Pink{1.0f, 0.35f, 0.75f};
    static constexpr glm::vec3 Orange{1.0f, 0.45f, 0.15f};
    static constexpr glm::vec3 Gold{1.0f, 0.75f, 0.2f};
    static constexpr glm::vec3 Cyan{0.2f, 1.0f, 1.0f};
    static constexpr glm::vec3 Moonlight{0.55f, 0.65f, 1.0f};
    static constexpr glm::vec3 Torch{1.0f, 0.5f, 0.2f};
  };

  Lamp(glm::vec2 position, glm::vec3 color = {1.0f, 0.9f, 0.7f})
      : m_position(position), m_color(color)
  {
  }

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
            .addComponent<sfs::LightEmitterComponent>(
                10.0f, 1.0f, 32.0f, m_color)
            .addTag<sfs::SolidObject>();

    m_entity.getComponent<sfs::ColliderComponent>().updateBounds(m_position);
  }

private:
  glm::vec2 m_position;
  glm::vec3 m_color;
};
