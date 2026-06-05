#pragma once

#include "config.h"
#include "engine/runtime/assetStore/assetStore.h"
#include "engine/core/components/elevationComponent.h"
#include "engine/core/components/lightEmitterComponent.h"
#include "engine/core/components/shadowCasterComponent.h"
#include "engine/core/components/spriteComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/components/worldCollider.h"
#include "engine/runtime/game/gameObject.h"
#include "engine/runtime/sceneManager/scene.h"
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
            // Ground footprint in pixels, relative to the feet: a small base
            // centred on the lamp so the player is blocked by the base (not the
            // whole sprite).
            .addComponent<sfs::WorldCollider>(
                glm::vec2{-2.0f, -2.0f}, glm::vec2{4.0f, 4.0f})
            .addComponent<sfs::LightEmitterComponent>(
                640.0f, 1.0f, 32.0f, m_color)
            .addComponent<sfs::ShadowCasterComponent>()
            .addTag<sfs::SolidObject>();

    m_entity.getComponent<sfs::WorldCollider>().updateBounds(m_position);
  }

private:
  glm::vec2 m_position;
  glm::vec3 m_color;
};
