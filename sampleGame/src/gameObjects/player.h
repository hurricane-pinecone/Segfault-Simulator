#pragma once

#include "config.h"
#include "engine/components/cameraComponent.h"
#include "engine/components/lightEmitterComponent.h"
#include "engine/components/rigidBodyComponent.h"
#include "engine/components/screenSpaceCollider.h"
#include "engine/components/shadowCasterComponent.h"
#include "engine/components/spriteComponent.h"
#include "engine/components/transformComponent.h"
#include "engine/components/worldCollider.h"
#include "engine/game/gameObject.h"
#include "engine/input/input.h"
#include "engine/sceneManager/scene.h"
#include "glm/glm/ext/vector_float2.hpp"
#include "glm/glm/geometric.hpp"

class Player : public sfs::GameObject
{
public:
  void onCreate(sfs::Scene& scene) override
  {
    scene.assetStore().addTexture(
        "spritesheet", ASSET_ROOT + "spriteSheets/tilemap.png");
    auto sprite = scene.assetStore().addSpriteFromSheet(
        "spritesheet", "guy", 16, 16, 16, 6, 1, 0);

    m_entity =
        scene.createEntity()
            .addComponent<sfs::SpriteComponent>(sprite, glm::vec2{0.5f, 1.0f})
            .addComponent<sfs::TransformComponent>(glm::vec2{12.0, 12.0})
            // Billboard hit box in sprite pixels from the top-left (covers the
            // 16px sprite): bullet / sprite hits in screen space.
            .addComponent<sfs::ScreenSpaceCollider>(
                glm::vec2{0.0f, 0.0f}, glm::vec2{16.0f, 16.0f})
            // Ground footprint at the feet: drives terrain elevation / step-up
            // and world-object blocking, kept narrow so the body doesn't clip
            // into raised faces while climbing.
            .addComponent<sfs::WorldCollider>(
                glm::vec2{-6.0f, -6.0f}, glm::vec2{12.0f, 12.0f})
            .addComponent<sfs::RigidBodyComponent>(glm::vec2{0.0, 0.0})
            .addComponent<sfs::LightEmitterComponent>(640.0f, 1.0f, 10.0f)
            .addComponent<sfs::ShadowCasterComponent>();

    // Camera
    scene.createEntity()
        .addComponent<sfs::TransformComponent>(
            glm::vec2{0.0f, 0.0f}, glm::vec2{1.0f, 1.0f}, 0.0f)
        .addComponent<sfs::CameraComponent>(
            m_entity.getId(), glm::vec2{0.0f, 0.0f}, 8.0f, 1.0f);
  }

  void onProcessInput(const sfs::Input& input) override
  {
    glm::vec2 screenDirection(0.0f);

    if (input.keyboard().keyHeld(sfs::Key::A))
      screenDirection.x -= 1.0f;
    if (input.keyboard().keyHeld(sfs::Key::D))
      screenDirection.x += 1.0f;
    if (input.keyboard().keyHeld(sfs::Key::W))
      screenDirection.y -= 1.0f;
    if (input.keyboard().keyHeld(sfs::Key::S))
      screenDirection.y += 1.0f;

    if (glm::length(screenDirection) > 0.0f)
    {
      screenDirection = glm::normalize(screenDirection);
    }

    glm::vec2 gridDirection{screenDirection.y + screenDirection.x,
                            screenDirection.y - screenDirection.x};

    if (glm::length(gridDirection) > 0.0f)
    {
      gridDirection = glm::normalize(gridDirection);
    }

    auto& rb = m_entity.getComponent<sfs::RigidBodyComponent>();
    rb.velocity = gridDirection * 5.0f;
  }
};
